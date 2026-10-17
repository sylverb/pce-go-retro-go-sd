#ifndef __CHDCONFIG_H__
#define __CHDCONFIG_H__

/* PCE-CD / Game & Watch: CD hunks are 2448 B (2352+96) in the CHD. */
#define WANT_RAW_DATA_SECTOR    1
/* Skip subcode zlib windows (~3×32 KiB). Dest frames stay 2448 B; we only
 * copy the 2352 B user/audio area into the SCSI path. */
#define WANT_SUBCODE            0
/* No second hunk cache — we keep one workspace buffer in the wrapper. */
/* #define NEED_CACHE_HUNK */
/* #define VERIFY_BLOCK_CRC */
/* This tree's CD CHDs use cdlz/cdzl/cdfl only — skip zstd to save .text. */
/* #define HAVE_ZSTD */

#endif
