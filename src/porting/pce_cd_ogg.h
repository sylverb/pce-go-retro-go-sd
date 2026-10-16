/* PC Engine CD — OGG Vorbis CD-DA track support (stb_vorbis).
 *
 * Audio tracks referenced as FILE "….ogg" OGG in the cue sheet are decoded to
 * synthetic Red Book sectors (2352 B = 588 stereo frames @ 44.1 kHz) so the
 * existing SCSI CD-DA path stays unchanged. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pce_cd.h"

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
