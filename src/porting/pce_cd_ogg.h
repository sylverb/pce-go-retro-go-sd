/* PC Engine CD — OGG Vorbis CD-DA track support (stb_vorbis).
 *
 * Enabled with -DPCE_CD_OGG=1 (always on in the unified pce.bin).
 * Without it, the stubs below keep call sites compiling; cue FILE … OGG
 * tracks are ignored. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pce_cd.h"

#ifdef PCE_CD_OGG

/* Contiguous decode arena, reserved BEFORE CD-RAM mapping.
 * Ys III / typical PCE-CD Vorbis needs ~162 KiB setup + a few KiB temp. */
#define PCE_CD_OGG_ARENA_SIZE  (176u * 1024u)

/* True if any track in the TOC is OGG. */
bool pce_cd_ogg_toc_needs_decoder(const pce_cd_toc_t *toc);

/* Reserve decode arena (ram_malloc). Call AFTER parse_cue, BEFORE CD-RAM map. */
bool pce_cd_ogg_prepare(const pce_cd_toc_t *toc);

/* Probe an OGG file for total PCM frames (stereo pairs). No decoder needed. */
bool pce_cd_ogg_probe_samples(const char *path, uint32_t *out_samples);

/* Decode up to max_n contiguous Red Book sectors starting at absolute LBA
 * into buf (n * 2352 bytes). Returns sectors written (0 on error). */
int pce_cd_ogg_read_sectors(const pce_cd_toc_t *toc, uint32_t lba,
                            uint8_t *buf, int max_n);

/* Drop open decoder + fclose (mount change / pce_cd_close). Arena kept. */
void pce_cd_ogg_close_stream(void);

/* Invalidate FILE* after FatFs remount (sleep/wake); keep arena. */
void pce_cd_ogg_invalidate_handles(void);

#else /* !PCE_CD_OGG */

static inline bool pce_cd_ogg_toc_needs_decoder(const pce_cd_toc_t *toc)
{
	(void)toc;
	return false;
}
static inline bool pce_cd_ogg_prepare(const pce_cd_toc_t *toc)
{
	(void)toc;
	return true;
}
static inline bool pce_cd_ogg_probe_samples(const char *path, uint32_t *out_samples)
{
	(void)path;
	if (out_samples)
		*out_samples = 0;
	return false;
}
static inline int pce_cd_ogg_read_sectors(const pce_cd_toc_t *toc, uint32_t lba,
                                          uint8_t *buf, int max_n)
{
	(void)toc;
	(void)lba;
	(void)buf;
	(void)max_n;
	return 0;
}
static inline void pce_cd_ogg_close_stream(void) {}
static inline void pce_cd_ogg_invalidate_handles(void) {}

#endif /* PCE_CD_OGG */
