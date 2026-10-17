/* Dedicated freeable heap for libchdr (AHB newlib heap is too small for
 * cdlz+cdzl+cdfl). Include this BEFORE any malloc use in libchdr .c files. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Carve `bytes` from RAM_EMU (or AHB fallback). Returns false on OOM. */
int  chd_heap_init(size_t bytes);
void chd_heap_shutdown(void);   /* rewind bump to empty */
size_t chd_heap_used(void);
size_t chd_heap_size(void);
/* Nested bump scopes — reclaim temp encoder allocs without a real free(). */
size_t chd_heap_mark(void);
void   chd_heap_release_to(size_t mark);

void *chd_arena_malloc(size_t n);
void *chd_arena_calloc(size_t n, size_t sz);
void *chd_arena_realloc(void *p, size_t n);
void  chd_arena_free(void *p);

#ifdef __cplusplus
}
#endif

/* Remap libc allocators used by vendored libchdr / lzma / zlib / dr_flac. */
#if defined(PCE_CD_CHD) && !defined(PCE_CD_CHD_ALLOC_NO_REMAP)
#undef malloc
#undef calloc
#undef realloc
#undef free
#define malloc(n)       chd_arena_malloc(n)
#define calloc(n, s)    chd_arena_calloc(n, s)
#define realloc(p, n)   chd_arena_realloc(p, n)
#define free(p)         chd_arena_free(p)
#ifndef DRFLAC_MALLOC
#define DRFLAC_MALLOC(sz)          chd_arena_malloc(sz)
#define DRFLAC_REALLOC(p, sz)      chd_arena_realloc(p, sz)
#define DRFLAC_FREE(p)             chd_arena_free(p)
#endif
#endif
