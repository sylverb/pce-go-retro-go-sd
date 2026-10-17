/* Stub zstd API for libchdr when CD images only need cdlz/cdzl/cdfl.
 * Real ZSTD is not linked; these codecs must never be selected. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct ZSTD_DStream_s ZSTD_DStream;

typedef struct {
	void *dst;
	size_t size;       /* libchdr uses .size (older zstd naming) */
	size_t pos;
	size_t dstCapacity; /* unused in stub */
} ZSTD_outBuffer;

typedef struct {
	const void *src;
	size_t size;
	size_t pos;
	size_t srcSize;
} ZSTD_inBuffer;

static inline ZSTD_DStream *ZSTD_createDStream(void) { return NULL; }
static inline size_t ZSTD_freeDStream(ZSTD_DStream *d) { (void)d; return 0; }
static inline size_t ZSTD_initDStream(ZSTD_DStream *d) { (void)d; return (size_t)-1; }
static inline size_t ZSTD_decompressStream(ZSTD_DStream *d, ZSTD_outBuffer *out, ZSTD_inBuffer *in)
{
	(void)d; (void)out; (void)in;
	return (size_t)-1;
}
static inline unsigned ZSTD_isError(size_t code) { return code != 0; }
