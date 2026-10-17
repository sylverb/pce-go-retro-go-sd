/* Minimal stubs — CD CHDs use cdlz/cdzl/cdfl; zstd codecs are never selected. */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct ZSTD_DStream_s ZSTD_DStream;
typedef size_t ZSTD_ErrorCode;
static inline ZSTD_DStream *ZSTD_createDStream(void) { return NULL; }
static inline size_t ZSTD_freeDStream(ZSTD_DStream *d) { (void)d; return 0; }
static inline size_t ZSTD_initDStream(ZSTD_DStream *d) { (void)d; return (size_t)-1; }
static inline size_t ZSTD_decompressStream(ZSTD_DStream *d, void *out, void *in) { (void)d;(void)out;(void)in; return (size_t)-1; }
static inline unsigned ZSTD_isError(size_t c) { return c != 0; }
typedef struct { void *dst; size_t dstCapacity; size_t pos; } ZSTD_outBuffer;
typedef struct { const void *src; size_t srcSize; size_t pos; } ZSTD_inBuffer;
