/* PC Engine CD — CHD backend (libchdr). */
#include "pce_cd_chd.h"

#ifdef PCE_CD_CHD

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define PCE_CD_CHD_ALLOC_NO_REMAP 1
#include "pce_cd_chd_alloc.h"
#include <libchdr/chd.h>
#include "gw_malloc.h"
#include "odroid_overlay.h"

/* Declared in main.h / bridge — kick WWDG during long map expands on device. */
void wdog_refresh(void);
uint32_t HAL_GetTick(void);

#ifndef CHD_MAKE_TAG
#define CHD_MAKE_TAG(a,b,c,d) (((a)<<24)|((b)<<16)|((c)<<8)|(d))
#endif

#define PCE_CD_CHD_TRACK_PAD   4
/* Codec heap in RAM_EMU. Ys peaks ~177 KiB at open then needs another ~42 KiB
 * for CD-audio decode — 192 KiB OOMs mid-play. 240 KiB leaves ~43 KiB
 * (+ LUT8 bonus 150 KiB + exram/save) for 32 CD-RAM banks. */
#define PCE_CD_CHD_HEAP        (240u * 1024u)

/* Stutter diagnosis: log slow hunks + 1 Hz summary. Thresh in ms (tick = 1 ms).
 * Normal hunk IO is ~8–12 ms; FatFs thrash spikes were ~200 ms — log those. */
#define PCE_CD_CHD_PERF_SLOW_MS   40u
#define PCE_CD_CHD_PERF_PERIOD_MS 1000u

typedef struct {
	uint32_t start_lba;     /* INDEX 01 absolute LBA */
	uint32_t length_lba;    /* sectors from INDEX 01 */
	uint32_t chd_frame;     /* CHD frame of INDEX 01 */
	uint8_t  is_audio;
	uint8_t  pregap_in_file;
	uint32_t pregap;        /* TOC pregap before INDEX 01 */
	uint32_t chd_track0;    /* CHD frame at start of track data (incl. in-file pregap) */
} pce_chd_track_t;

static chd_file *s_chd;
static char s_chd_path[256];
static char s_map_path[260];
static uint8_t *s_hunk_buf;
static int32_t s_hunk_cached = -1;
static uint32_t s_hunkbytes;
static uint32_t s_unitbytes;
static uint32_t s_frames_per_hunk;
static pce_chd_track_t s_tr[PCE_CD_MAX_TRACKS];
static int s_ntr;
/* .map mirrored in QSPI flash (or host malloc). Not freed by libchdr. */
static uint8_t *s_map_flash;
static int s_map_flash_host_owned;

/* Perf window (reset every PCE_CD_CHD_PERF_PERIOD_MS). */
static uint32_t s_perf_t0;
static uint32_t s_hit, s_miss;
static uint32_t s_map_ms, s_io_ms, s_decomp_ms;
static uint32_t s_max_total_ms, s_max_map_ms, s_max_io_ms, s_max_decomp_ms;
static uint32_t s_sec_data, s_sec_aud, s_seek_jumps;
static uint32_t s_last_abs = 0xffffffffu;
static uint32_t s_last_codec;

static const char *codec_name(uint32_t tag)
{
	switch (tag) {
	case CHD_CODEC_CD_LZMA: return "cdlz";
	case CHD_CODEC_CD_FLAC: return "cdfl";
	case CHD_CODEC_CD_ZLIB: return "cdzl";
	case CHD_CODEC_CD_ZSTD: return "cdzs";
	case CHD_CODEC_LZMA:    return "lzma";
	case CHD_CODEC_FLAC:    return "flac";
	case CHD_CODEC_ZLIB:    return "zlib";
	case CHD_CODEC_ZSTD:    return "zstd";
	case CHD_CODEC_HUFFMAN: return "huff";
	default:                return tag ? "other" : "-";
	}
}

static void perf_flush(int force)
{
	uint32_t now = HAL_GetTick();
	uint32_t dt = now - s_perf_t0;
	if (!force && dt < PCE_CD_CHD_PERF_PERIOD_MS)
		return;
	if (s_hit || s_miss || s_sec_data || s_sec_aud) {
		printf("CHD perf %ums: hit=%u miss=%u map=%ums io=%ums decomp=%ums "
		       "max(tot/map/io/dec)=%u/%u/%u/%u data=%u aud=%u seeks=%u codec=%s\n",
		       (unsigned)dt,
		       (unsigned)s_hit, (unsigned)s_miss,
		       (unsigned)s_map_ms, (unsigned)s_io_ms, (unsigned)s_decomp_ms,
		       (unsigned)s_max_total_ms, (unsigned)s_max_map_ms,
		       (unsigned)s_max_io_ms, (unsigned)s_max_decomp_ms,
		       (unsigned)s_sec_data, (unsigned)s_sec_aud, (unsigned)s_seek_jumps,
		       codec_name(s_last_codec));
	}
	s_perf_t0 = now;
	s_hit = s_miss = 0;
	s_map_ms = s_io_ms = s_decomp_ms = 0;
	s_max_total_ms = s_max_map_ms = s_max_io_ms = s_max_decomp_ms = 0;
	s_sec_data = s_sec_aud = s_seek_jumps = 0;
}

static void on_hunk_perf(uint32_t hunknum, uint32_t map_ms, uint32_t io_ms,
                         uint32_t decomp_ms, uint32_t codec)
{
	uint32_t total = map_ms + io_ms + decomp_ms;
	s_miss++;
	s_map_ms += map_ms;
	s_io_ms += io_ms;
	s_decomp_ms += decomp_ms;
	if (total > s_max_total_ms) s_max_total_ms = total;
	if (map_ms > s_max_map_ms) s_max_map_ms = map_ms;
	if (io_ms > s_max_io_ms) s_max_io_ms = io_ms;
	if (decomp_ms > s_max_decomp_ms) s_max_decomp_ms = decomp_ms;
	if (codec)
		s_last_codec = codec;

	if (total >= PCE_CD_CHD_PERF_SLOW_MS) {
		printf("CHD slow hunk=%u map=%ums io=%ums decomp=%ums tot=%ums codec=%s\n",
		       (unsigned)hunknum, (unsigned)map_ms, (unsigned)io_ms,
		       (unsigned)decomp_ms, (unsigned)total, codec_name(codec));
	}
	perf_flush(0);
}

static void map_path_from_chd(const char *chd_path, char *out, size_t out_sz)
{
	snprintf(out, out_sz, "%s.map", chd_path);
}

static int type_is_audio(const char *type)
{
	return strncmp(type, "AUDIO", 5) == 0;
}

static bool build_toc_from_metadata(pce_cd_toc_t *toc)
{
	char meta[256];
	uint32_t disc_lba = 0;
	uint32_t chd_lba = 0;

	memset(toc, 0, sizeof(*toc));
	s_ntr = 0;

	for (uint32_t i = 0; i < PCE_CD_MAX_TRACKS; i++) {
		uint32_t resultlen = 0, resulttag = 0;
		uint8_t flags = 0;
		int tracknum = 0, frames = 0, pregap = 0, postgap = 0;
		char type[32] = {0}, subtype[32] = {0}, pgtype[32] = {0}, pgsub[32] = {0};
		chd_error err = chd_get_metadata(s_chd, CDROM_TRACK_METADATA2_TAG, i,
			meta, sizeof(meta), &resultlen, &resulttag, &flags);
		if (err != CHDERR_NONE) {
			err = chd_get_metadata(s_chd, CDROM_TRACK_METADATA_TAG, i,
				meta, sizeof(meta), &resultlen, &resulttag, &flags);
			if (err != CHDERR_NONE)
				break;
			if (sscanf(meta, CDROM_TRACK_METADATA_FORMAT,
			           &tracknum, type, subtype, &frames) != 4)
				break;
			pregap = postgap = 0;
			pgtype[0] = 0;
		} else {
			meta[resultlen < sizeof(meta) ? resultlen : sizeof(meta) - 1] = 0;
			if (sscanf(meta, CDROM_TRACK_METADATA2_FORMAT,
			           &tracknum, type, subtype, &frames, &pregap,
			           pgtype, pgsub, &postgap) != 8)
				break;
		}

		int pregap_in_file = (pregap > 0 && pgtype[0] == 'V');
		int tr_frames = frames;
		uint32_t chd_track0 = chd_lba;

		if (pregap_in_file) {
			disc_lba += (uint32_t)pregap;
			chd_lba += (uint32_t)pregap;
			tr_frames -= pregap;
		} else if (pregap > 0) {
			/* Pregap not stored — TOC gap only (silence). */
			disc_lba += (uint32_t)pregap;
		}

		if (s_ntr >= PCE_CD_MAX_TRACKS)
			break;

		pce_chd_track_t *ct = &s_tr[s_ntr];
		ct->start_lba = disc_lba;
		ct->length_lba = (uint32_t)tr_frames;
		ct->chd_frame = chd_lba;
		ct->is_audio = type_is_audio(type) ? 1 : 0;
		ct->pregap_in_file = pregap_in_file ? 1 : 0;
		ct->pregap = (uint32_t)pregap;
		ct->chd_track0 = chd_track0;

		pce_cd_track_t *t = &toc->tracks[s_ntr];
		t->number = (uint8_t)tracknum;
		t->type = ct->is_audio ? PCE_TRACK_AUDIO : PCE_TRACK_DATA;
		t->file_fmt = PCE_FILE_BINARY; /* sector path routes via chd_active */
		t->sector_size = PCE_CD_SECTOR_RAW;
		t->start_lba = ct->start_lba;
		t->length_lba = ct->length_lba;
		t->file_offset = 0;
		t->sample_offset = 0;
		t->pcm_samples = 0;
		strncpy(t->bin_path, s_chd_path, sizeof(t->bin_path) - 1);

		disc_lba += (uint32_t)tr_frames;
		chd_lba += (uint32_t)tr_frames;
		chd_lba = (chd_lba + (PCE_CD_CHD_TRACK_PAD - 1)) / PCE_CD_CHD_TRACK_PAD * PCE_CD_CHD_TRACK_PAD;

		s_ntr++;
		toc->num_tracks = s_ntr;
	}

	toc->total_lba = disc_lba;
	return s_ntr > 0;
}

static int ensure_hunk(uint32_t hunknum)
{
	if ((int32_t)hunknum == s_hunk_cached) {
		s_hit++;
		perf_flush(0);
		return 1;
	}
	if (!s_chd || !s_hunk_buf)
		return 0;
	/* Miss: chd_read → on_hunk_perf (map/io/decomp). */
	if (chd_read(s_chd, hunknum, s_hunk_buf) != CHDERR_NONE) {
		s_hunk_cached = -1;
		return 0;
	}
	s_hunk_cached = (int32_t)hunknum;
	return 1;
}

static int frame_to_sector2352(uint32_t chd_frame, uint8_t *dst)
{
	uint32_t hunk = chd_frame / s_frames_per_hunk;
	uint32_t idx = chd_frame % s_frames_per_hunk;
	if (!ensure_hunk(hunk))
		return 0;
	const uint8_t *src = s_hunk_buf + idx * s_unitbytes;
	/* unit = 2448 (2352 + 96 subcode) or 2352 */
	memcpy(dst, src, PCE_CD_SECTOR_RAW);
	/* Audio tracks in CHD are often big-endian PCM — swab 16-bit samples. */
	return 1;
}

bool pce_cd_chd_open(const char *chd_path, pce_cd_toc_t *toc)
{
	pce_cd_chd_close();

	if (!chd_path || !toc)
		return false;

	strncpy(s_chd_path, chd_path, sizeof(s_chd_path) - 1);
	s_chd_path[sizeof(s_chd_path) - 1] = 0;
	map_path_from_chd(s_chd_path, s_map_path, sizeof(s_map_path));

	/* Codec mallocs must not hit the AHB newlib heap (too small for 3 CD codecs).
	 * LUT8 bonus for Super CD-RAM is enabled by the caller (pce_cd_enable_lut8_bonus). */
	if (!chd_heap_init(PCE_CD_CHD_HEAP)) {
		printf("CHD: heap init failed (%u KiB)\n", (unsigned)(PCE_CD_CHD_HEAP / 1024u));
		return false;
	}

	chd_set_map_file_path(s_map_path);
	/* Map expand mallocs from the CHD heap and frees before codec init. */
	chd_set_map_scratch(NULL, 0);
	chd_set_map_wdog(wdog_refresh);
	chd_set_hunk_perf(HAL_GetTick, on_hunk_perf);
	s_perf_t0 = HAL_GetTick();
	s_hit = s_miss = 0;
	s_map_ms = s_io_ms = s_decomp_ms = 0;
	s_max_total_ms = s_max_map_ms = s_max_io_ms = s_max_decomp_ms = 0;
	s_sec_data = s_sec_aud = s_seek_jumps = 0;
	s_last_abs = 0xffffffffu;
	s_last_codec = 0;

	chd_file *chd = NULL;
	chd_error err = chd_open(s_chd_path, CHD_OPEN_READ, NULL, &chd);
	chd_set_map_file_path(NULL);
	chd_set_map_scratch(NULL, 0);
	chd_set_map_wdog(NULL);
	if (err != CHDERR_NONE || !chd) {
		printf("CHD: open failed err=%d (%s)\n", (int)err, chd_error_string(err));
		chd_heap_shutdown();
		return false;
	}
	printf("CHD: open ok heap_used=%u/%u KiB tracks pending\n",
	       (unsigned)(chd_heap_used() / 1024u), (unsigned)(chd_heap_size() / 1024u));

	s_chd = chd;
	const chd_header *h = chd_get_header(s_chd);
	s_hunkbytes = h->hunkbytes;
	s_unitbytes = h->unitbytes ? h->unitbytes : 2448;
	s_frames_per_hunk = s_hunkbytes / s_unitbytes;
	if (s_frames_per_hunk == 0) {
		printf("CHD: bad frames_per_hunk\n");
		pce_cd_chd_close();
		return false;
	}

	/* Mirror .map into external flash so map lookups never touch SD again
	 * (keeps FatFs focused on the .chd stream). */
	{
		uint32_t map_sz = 0;
		uint32_t need = (h->hunkcount ? h->hunkcount : h->totalhunks) * h->mapentrybytes;
		wdog_refresh();
		s_map_flash = odroid_overlay_cache_file_in_flash(s_map_path, &map_sz, false);
		wdog_refresh();
		if (s_map_flash && map_sz >= need &&
		    chd_adopt_rawmap(s_chd, s_map_flash, map_sz, 0) == CHDERR_NONE) {
#ifdef HOST_BUILD
			s_map_flash_host_owned = 1; /* host stub malloc's the buffer */
#else
			s_map_flash_host_owned = 0; /* firmware QSPI pool owns it */
#endif
			printf("CHD: map in flash %u KiB @%p (SD sidecar idle)\n",
			       (unsigned)(map_sz / 1024u), (void *)s_map_flash);
		} else {
			printf("CHD: map flash cache failed (sz=%u need=%u) — SD sidecar\n",
			       (unsigned)map_sz, (unsigned)need);
			s_map_flash = NULL;
			s_map_flash_host_owned = 0;
		}
	}

	s_hunk_buf = (uint8_t *)chd_arena_malloc(s_hunkbytes);
	if (!s_hunk_buf) {
		printf("CHD: hunk buf OOM\n");
		pce_cd_chd_close();
		return false;
	}
	s_hunk_cached = -1;

	if (!build_toc_from_metadata(toc)) {
		printf("CHD: TOC metadata failed\n");
		pce_cd_chd_close();
		return false;
	}
	printf("CHD: %d tracks total_lba=%lu heap=%u/%u KiB hunk=%uB fph=%u\n",
	       toc->num_tracks, (unsigned long)toc->total_lba,
	       (unsigned)(chd_heap_used() / 1024u), (unsigned)(chd_heap_size() / 1024u),
	       (unsigned)s_hunkbytes, (unsigned)s_frames_per_hunk);
	printf("CHD: codecs=[%c%c%c%c %c%c%c%c %c%c%c%c %c%c%c%c] (perf log every %ums, slow>=%ums)\n",
	       (int)((h->compression[0] >> 24) & 0xff), (int)((h->compression[0] >> 16) & 0xff),
	       (int)((h->compression[0] >> 8) & 0xff), (int)(h->compression[0] & 0xff),
	       (int)((h->compression[1] >> 24) & 0xff), (int)((h->compression[1] >> 16) & 0xff),
	       (int)((h->compression[1] >> 8) & 0xff), (int)(h->compression[1] & 0xff),
	       (int)((h->compression[2] >> 24) & 0xff), (int)((h->compression[2] >> 16) & 0xff),
	       (int)((h->compression[2] >> 8) & 0xff), (int)(h->compression[2] & 0xff),
	       (int)((h->compression[3] >> 24) & 0xff), (int)((h->compression[3] >> 16) & 0xff),
	       (int)((h->compression[3] >> 8) & 0xff), (int)(h->compression[3] & 0xff),
	       (unsigned)PCE_CD_CHD_PERF_PERIOD_MS, (unsigned)PCE_CD_CHD_PERF_SLOW_MS);
	return true;
}

bool pce_cd_chd_lut8(void)
{
	return pce_cd_lut8();
}

void pce_cd_chd_cd_ram_bonus(uint8_t **ptr, size_t *size)
{
	pce_cd_ram_bonus(ptr, size);
}

void pce_cd_chd_close(void)
{
	perf_flush(1);
	chd_set_hunk_perf(NULL, NULL);
	if (s_chd) {
		chd_close(s_chd);
		s_chd = NULL;
	}
	if (s_map_flash_host_owned && s_map_flash)
		free(s_map_flash);
	s_map_flash = NULL;
	s_map_flash_host_owned = 0;
	s_hunk_buf = NULL;
	s_hunk_cached = -1;
	s_ntr = 0;
	chd_heap_shutdown();
}

void pce_cd_chd_invalidate_handles(void)
{
	/* libchdr owns FILE* inside core_file — after FatFs remount they dangle.
	 * Force full reopen on next disc use by dropping the chd object. */
	if (s_chd) {
		/* Do not fclose — volume was torn down. Leak the chd_file object
		 * rather than touch FatFs; next open allocates fresh. */
		s_chd = NULL;
	}
	s_hunk_cached = -1;
}

bool pce_cd_chd_active(void)
{
	return s_chd != NULL;
}

int pce_cd_chd_read_sectors(const pce_cd_toc_t *toc, uint32_t lba,
                            uint8_t *buf, int max_n)
{
	(void)toc;
	if (!s_chd || max_n <= 0)
		return 0;

	int written = 0;
	for (int n = 0; n < max_n; n++) {
		uint32_t abs = lba + (uint32_t)n;
		/* Find track containing this LBA (INDEX01..end). Pregap LBAs before
		 * INDEX01 with in-file pregap still map into CHD. */
		int ti = -1;
		for (int i = 0; i < s_ntr; i++) {
			uint32_t gap_start = s_tr[i].start_lba - (s_tr[i].pregap_in_file ? 0 : s_tr[i].pregap);
			/* For !pregap_in_file, pregap LBAs are synthetic silence. */
			uint32_t start = s_tr[i].pregap_in_file
				? (s_tr[i].start_lba - s_tr[i].pregap)
				: s_tr[i].start_lba;
			uint32_t end = s_tr[i].start_lba + s_tr[i].length_lba;
			if (abs >= start && abs < end) {
				ti = i;
				break;
			}
			(void)gap_start;
		}
		if (ti < 0)
			break;

		pce_chd_track_t *ct = &s_tr[ti];
		uint8_t *dst = buf + (size_t)n * PCE_CD_SECTOR_RAW;

		if (!ct->pregap_in_file && abs < ct->start_lba) {
			memset(dst, 0, PCE_CD_SECTOR_RAW);
			written++;
			continue;
		}

		uint32_t chd_frame;
		if (ct->pregap_in_file)
			chd_frame = ct->chd_track0 + (abs - (ct->start_lba - ct->pregap));
		else
			chd_frame = ct->chd_frame + (abs - ct->start_lba);

		if (s_last_abs != 0xffffffffu && abs != s_last_abs + 1u)
			s_seek_jumps++;
		s_last_abs = abs;

		if (!frame_to_sector2352(chd_frame, dst))
			break;

		if (ct->is_audio) {
			/* CHD stores audio big-endian; host/PCE want little-endian PCM. */
			uint16_t *s = (uint16_t *)dst;
			for (int i = 0; i < PCE_CD_SECTOR_RAW / 2; i++)
				s[i] = (uint16_t)((s[i] << 8) | (s[i] >> 8));
			s_sec_aud++;
		} else {
			s_sec_data++;
		}
		written++;
	}
	perf_flush(0);
	return written;
}

#endif /* PCE_CD_CHD */
