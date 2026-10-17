/* PC Engine CD — MAME CHD disc images (libchdr).
 *
 * Enabled with -DPCE_CD_CHD=1 (always on in the unified pce.bin). Runtime is
 * mutually exclusive with the OGG path: only one disc backend is armed per
 * game (.chd vs .cue).
 *
 * The V5 hunk map (~12 B × hunks, often >300 KiB) is expanded to a sidecar
 * "<path>.map" on SD so RAM only holds codec + one hunk workspace. */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pce_cd.h"

#ifdef PCE_CD_CHD

/* Open CHD, build TOC from CHT2 metadata, prepare map sidecar + workspace.
 * Call instead of pce_cd_parse_cue when ACTIVE_FILE is a .chd. */
bool pce_cd_chd_open(const char *chd_path, pce_cd_toc_t *toc);

/* Compat wrappers → pce_cd_lut8 / pce_cd_ram_bonus (shared CD LUT8 path). */
bool pce_cd_chd_lut8(void);
void pce_cd_chd_cd_ram_bonus(uint8_t **ptr, size_t *size);

/* Close CHD + map file handles (keep sidecar on SD). */
void pce_cd_chd_close(void);

/* After FatFs remount: drop FILE* without fclose of torn-down volume. */
void pce_cd_chd_invalidate_handles(void);

/* True while a CHD disc is mounted. */
bool pce_cd_chd_active(void);

/* Read up to max_n Red Book (2352 B) sectors at absolute LBA into buf. */
int pce_cd_chd_read_sectors(const pce_cd_toc_t *toc, uint32_t lba,
                            uint8_t *buf, int max_n);

#else /* !PCE_CD_CHD */

static inline bool pce_cd_chd_open(const char *chd_path, pce_cd_toc_t *toc)
{
	(void)chd_path;
	(void)toc;
	return false;
}
static inline bool pce_cd_chd_lut8(void) { return false; }
static inline void pce_cd_chd_cd_ram_bonus(uint8_t **ptr, size_t *size)
{
	if (ptr) *ptr = NULL;
	if (size) *size = 0;
}
static inline void pce_cd_chd_close(void) {}
static inline void pce_cd_chd_invalidate_handles(void) {}
static inline bool pce_cd_chd_active(void) { return false; }
static inline int pce_cd_chd_read_sectors(const pce_cd_toc_t *toc, uint32_t lba,
                                          uint8_t *buf, int max_n)
{
	(void)toc;
	(void)lba;
	(void)buf;
	(void)max_n;
	return 0;
}

#endif /* PCE_CD_CHD */
