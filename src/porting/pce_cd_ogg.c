/* PC Engine CD — OGG Vorbis CD-DA via stb_vorbis (streamed, not whole-file). */
#include "pce_cd_ogg.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "gw_malloc.h"
#include "utils.h"

/* utils.h uses osd_log; declare it (osd.h may pull heavier deps). */
extern void osd_log(int type, const char *format, ...);

/* stb_vorbis config — keep footprint small. */
#define STB_VORBIS_MAX_CHANNELS      2
#define STB_VORBIS_NO_PUSHDATA_API

#include "stb_vorbis.c"

/* Red Book: 44100 / 75 = 588 stereo frames per sector. */
#define PCE_CDDA_FRAMES_PER_SEC  588

static char     *s_arena;
static int       s_arena_size;
static stb_vorbis *s_v;
static FILE     *s_file;
static char      s_path[256];
static uint32_t  s_pcm_pos;       /* next sample frame to decode (absolute in file) */
static uint32_t  s_track_base;    /* sample offset of current track's INDEX 01 */
static int       s_channels;
static int       s_rate;

bool pce_cd_ogg_toc_needs_decoder(const pce_cd_toc_t *toc)
{
	if (!toc) return false;
	for (int i = 0; i < toc->num_tracks; i++) {
		if (toc->tracks[i].file_fmt == PCE_FILE_OGG)
			return true;
	}
	return false;
}

bool pce_cd_ogg_prepare(const pce_cd_toc_t *toc)
{
	if (!pce_cd_ogg_toc_needs_decoder(toc))
		return true;
	if (s_arena)
		return true;

	/* Leave enough bump for CD-RAM (19 banks) + EXRAM/save_buffer → 32 banks.
	 * Host has a 1 MiB pool so 256 KiB arenas succeed there; device tops out
	 * near 168–176 KiB after ADPCM moved out of BSS. */
	const size_t leave_for_cd = 152u * 1024u;
	static const size_t try_sz[] = {
		256u * 1024u,
		PCE_CD_OGG_ARENA_SIZE, /* 176 */
		168u * 1024u,
		160u * 1024u,
	};
	size_t free_now = ram_get_free_size();
	for (size_t i = 0; i < sizeof(try_sz) / sizeof(try_sz[0]); i++) {
		if (try_sz[i] + leave_for_cd > free_now && try_sz[i] > 168u * 1024u)
			continue;
		s_arena = (char *)ram_malloc(try_sz[i]);
		if (!s_arena)
			s_arena = (char *)ahb_malloc(try_sz[i]);
		if (s_arena) {
			s_arena_size = (int)try_sz[i];
			MESSAGE_INFO("PCE-CD OGG: arena %d KiB @ %p (free was %u KiB)\n",
				s_arena_size / 1024, (void *)s_arena,
				(unsigned)(free_now / 1024));
			return true;
		}
	}
	MESSAGE_INFO("PCE-CD OGG: arena alloc FAILED — OGG tracks won't play\n");
	return false;
}

/* Scan the file for the last Ogg page with a valid granule → total samples. */
bool pce_cd_ogg_probe_samples(const char *path, uint32_t *out_samples)
{
	if (!path || !out_samples) return false;
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
	long fsz = ftell(f);
	if (fsz < 27) { fclose(f); return false; }

	/* Read a tail window large enough for several pages. */
	const long win = 65536;
	long start = (fsz > win) ? (fsz - win) : 0;
	size_t len = (size_t)(fsz - start);
	uint8_t *buf = (uint8_t *)malloc(len);
	if (!buf) {
		/* Fallback: tiny stackless scan in 4K chunks from EOF. */
		uint8_t chunk[4096];
		int64_t best = -1;
		long pos = fsz;
		while (pos > 0) {
			long chunk_start = pos - (long)sizeof(chunk);
			if (chunk_start < 0) chunk_start = 0;
			size_t n = (size_t)(pos - chunk_start);
			if (fseek(f, chunk_start, SEEK_SET) != 0) break;
			if (fread(chunk, 1, n, f) != n) break;
			for (size_t i = 0; i + 27 <= n; i++) {
				if (chunk[i] == 'O' && chunk[i+1] == 'g' && chunk[i+2] == 'g' && chunk[i+3] == 'S') {
					int64_t g = (int64_t)(
						(uint64_t)chunk[i+6] |
						((uint64_t)chunk[i+7] << 8) |
						((uint64_t)chunk[i+8] << 16) |
						((uint64_t)chunk[i+9] << 24) |
						((uint64_t)chunk[i+10] << 32) |
						((uint64_t)chunk[i+11] << 40) |
						((uint64_t)chunk[i+12] << 48) |
						((uint64_t)chunk[i+13] << 56));
					if (g >= 0)
						best = g;
				}
			}
			if (chunk_start == 0) break;
			pos = chunk_start + 26; /* overlap so a page straddling chunks is seen */
		}
		fclose(f);
		if (best < 0) return false;
		*out_samples = (uint32_t)best;
		return true;
	}

	if (fseek(f, start, SEEK_SET) != 0 || fread(buf, 1, len, f) != len) {
		free(buf);
		fclose(f);
		return false;
	}
	fclose(f);

	int64_t best = -1;
	for (size_t i = 0; i + 27 <= len; i++) {
		if (buf[i] == 'O' && buf[i+1] == 'g' && buf[i+2] == 'g' && buf[i+3] == 'S') {
			int64_t g = (int64_t)(
				(uint64_t)buf[i+6] |
				((uint64_t)buf[i+7] << 8) |
				((uint64_t)buf[i+8] << 16) |
				((uint64_t)buf[i+9] << 24) |
				((uint64_t)buf[i+10] << 32) |
				((uint64_t)buf[i+11] << 40) |
				((uint64_t)buf[i+12] << 48) |
				((uint64_t)buf[i+13] << 56));
			if (g >= 0)
				best = g;
		}
	}
	free(buf);
	if (best < 0) return false;
	*out_samples = (uint32_t)best;
	return true;
}

void pce_cd_ogg_close_stream(void)
{
	if (s_v) {
		stb_vorbis_close(s_v);
		s_v = NULL;
	}
	if (s_file) {
		fclose(s_file);
		s_file = NULL;
	}
	s_path[0] = 0;
	s_pcm_pos = 0;
	s_track_base = 0;
	s_channels = 0;
	s_rate = 0;
}

void pce_cd_ogg_invalidate_handles(void)
{
	/* FatFs volume torn down — do not fclose. */
	s_v = NULL;
	s_file = NULL;
	s_path[0] = 0;
	s_pcm_pos = 0;
}

static bool ogg_open_track(const pce_cd_track_t *t)
{
	if (!s_arena || s_arena_size <= 0)
		return false;

	if (s_v && s_file && strcmp(s_path, t->bin_path) == 0) {
		/* Same file — only adjust track base / seek if needed by caller. */
		s_track_base = t->sample_offset;
		return true;
	}

	pce_cd_ogg_close_stream();

	s_file = fopen(t->bin_path, "rb");
	if (!s_file)
		return false;

	stb_vorbis_alloc alloc;
	alloc.alloc_buffer = s_arena;
	alloc.alloc_buffer_length_in_bytes = s_arena_size;

	int err = 0;
	s_v = stb_vorbis_open_file(s_file, 0 /* keep FILE open */, &err, &alloc);
	if (!s_v) {
		MESSAGE_INFO("PCE-CD OGG: open failed err=%d path=%s arena=%d\n",
			err, t->bin_path, s_arena_size);
		fclose(s_file);
		s_file = NULL;
		return false;
	}

	stb_vorbis_info info = stb_vorbis_get_info(s_v);
	s_channels = info.channels;
	s_rate = info.sample_rate;
	strncpy(s_path, t->bin_path, sizeof(s_path) - 1);
	s_path[sizeof(s_path) - 1] = 0;
	s_track_base = t->sample_offset;
	s_pcm_pos = 0;

	if (s_rate != 44100) {
		MESSAGE_INFO("PCE-CD OGG: warning rate=%d (expect 44100) path=%s\n",
			s_rate, t->bin_path);
	}
	if (s_channels < 1 || s_channels > 2) {
		MESSAGE_INFO("PCE-CD OGG: bad channels=%d\n", s_channels);
		pce_cd_ogg_close_stream();
		return false;
	}
	return true;
}

static bool ogg_seek_abs(uint32_t sample_frame)
{
	if (!s_v) return false;
	if (sample_frame == s_pcm_pos)
		return true;
	/* Prefer forward decode for small gaps (seek is expensive on SD). */
	if (sample_frame > s_pcm_pos && (sample_frame - s_pcm_pos) < (uint32_t)(PCE_CDDA_FRAMES_PER_SEC * 4)) {
		int16_t skip[PCE_CDDA_FRAMES_PER_SEC * 2];
		uint32_t left = sample_frame - s_pcm_pos;
		while (left) {
			int n = (int)((left > PCE_CDDA_FRAMES_PER_SEC) ? PCE_CDDA_FRAMES_PER_SEC : left);
			int got = stb_vorbis_get_samples_short_interleaved(s_v, s_channels, skip, n * s_channels);
			if (got <= 0) break;
			s_pcm_pos += (uint32_t)got;
			left -= (uint32_t)got;
		}
		return s_pcm_pos == sample_frame;
	}
	if (stb_vorbis_seek(s_v, sample_frame) == 0)
		return false;
	s_pcm_pos = sample_frame;
	return true;
}

int pce_cd_ogg_read_sectors(const pce_cd_toc_t *toc, uint32_t lba,
                            uint8_t *buf, int max_n)
{
	if (!toc || !buf || max_n <= 0 || !s_arena)
		return 0;

	int ti = pce_cd_track_at_lba(toc, lba);
	if (ti < 0) return 0;
	const pce_cd_track_t *t = &toc->tracks[ti];
	if (t->file_fmt != PCE_FILE_OGG || t->type != PCE_TRACK_AUDIO)
		return 0;

	if (!ogg_open_track(t))
		return 0;

	uint32_t track_end = (ti + 1 < toc->num_tracks) ? toc->tracks[ti + 1].start_lba
	                                                : toc->total_lba;
	uint32_t remain = (track_end > lba) ? (track_end - lba) : 1;
	int n = (max_n < (int)remain) ? max_n : (int)remain;

	uint32_t sec_in_track = lba - t->start_lba;
	uint32_t want_sample = t->sample_offset + sec_in_track * PCE_CDDA_FRAMES_PER_SEC;

	if (!ogg_seek_abs(want_sample)) {
		/* Hard seek failed — reopen and retry once. */
		pce_cd_ogg_close_stream();
		if (!ogg_open_track(t) || !ogg_seek_abs(want_sample))
			return 0;
	}

	int written = 0;
	for (int s = 0; s < n; s++) {
		int16_t *dst = (int16_t *)(buf + (size_t)s * PCE_CD_SECTOR_RAW);
		int need = PCE_CDDA_FRAMES_PER_SEC;
		int filled = 0;

		if (s_channels == 2) {
			while (filled < need) {
				int got = stb_vorbis_get_samples_short_interleaved(
					s_v, 2, dst + filled * 2, (need - filled) * 2);
				if (got <= 0) break;
				filled += got;
				s_pcm_pos += (uint32_t)got;
			}
		} else {
			/* Mono → stereo duplicate. */
			int16_t mono[PCE_CDDA_FRAMES_PER_SEC];
			while (filled < need) {
				int got = stb_vorbis_get_samples_short_interleaved(
					s_v, 1, mono + filled, need - filled);
				if (got <= 0) break;
				for (int i = 0; i < got; i++) {
					int16_t v = mono[filled + i];
					dst[(filled + i) * 2]     = v;
					dst[(filled + i) * 2 + 1] = v;
				}
				filled += got;
				s_pcm_pos += (uint32_t)got;
			}
		}

		if (filled < need) {
			/* Pad short end with silence. */
			memset(dst + filled * 2, 0, (size_t)(need - filled) * 4);
		}
		written++;
		if (filled == 0 && s == 0)
			return 0;
	}
	return written;
}
