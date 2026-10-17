/* First-fit free-list heap for libchdr in RAM_EMU (AHB is too small). */
#define PCE_CD_CHD_ALLOC_NO_REMAP 1
#include "pce_cd_chd_alloc.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "gw_malloc.h"

typedef struct chd_blk {
	size_t size;           /* payload bytes */
	uint32_t magic;
	struct chd_blk *next;  /* freelist when free */
} chd_blk_t;

#define MAGIC_USED  0x48454150u /* 'HEAP' */
#define MAGIC_FREE  0x46524545u /* 'FREE' */
/* Match lzma/zlib fast_alloc secondary alignment requirements. */
#define ALIGN       64u
#define HDR         ((sizeof(chd_blk_t) + (ALIGN - 1u)) & ~(ALIGN - 1u))

static uint8_t *s_base;
static size_t   s_size;
static size_t   s_used_peak;
static chd_blk_t *s_free;

static void *(*const s_sys_malloc)(size_t) = malloc;
static void *(*const s_sys_realloc)(void *, size_t) = realloc;
static void (*const s_sys_free)(void *) = free;

static size_t align_up(size_t n)
{
	return (n + (ALIGN - 1u)) & ~(size_t)(ALIGN - 1u);
}

static chd_blk_t *payload_to_blk(void *p)
{
	return (chd_blk_t *)((uint8_t *)p - HDR);
}

static void *blk_to_payload(chd_blk_t *b)
{
	return (uint8_t *)b + HDR;
}

static int blk_in_heap(const chd_blk_t *b)
{
	const uint8_t *p = (const uint8_t *)b;
	return s_base && p >= s_base && p < s_base + s_size;
}

static void coalesce(void)
{
	/* Sort-free: repeatedly merge physically adjacent free blocks. */
	int merged;
	do {
		merged = 0;
		for (chd_blk_t *a = s_free; a; a = a->next) {
			uint8_t *a_end = (uint8_t *)blk_to_payload(a) + a->size;
			chd_blk_t *prev = NULL;
			for (chd_blk_t *b = s_free; b; prev = b, b = b->next) {
				if (b == a)
					continue;
				if ((uint8_t *)b == a_end) {
					/* a then b */
					a->size += HDR + b->size;
					if (prev)
						prev->next = b->next;
					else
						s_free = b->next;
					merged = 1;
					break;
				}
			}
			if (merged)
				break;
		}
	} while (merged);
}

int chd_heap_init(size_t bytes)
{
	bytes = align_up(bytes);
	if (bytes < 4096)
		return 0;
	if (!(s_base && s_size >= bytes)) {
		uint8_t *raw = (uint8_t *)ram_malloc(bytes + ALIGN);
		if (!raw)
			raw = (uint8_t *)ahb_malloc(bytes + ALIGN);
		if (!raw)
			return 0;
		uintptr_t addr = (uintptr_t)raw;
		addr = (addr + (ALIGN - 1u)) & ~(uintptr_t)(ALIGN - 1u);
		s_base = (uint8_t *)addr;
		s_size = bytes;
	}
	s_free = (chd_blk_t *)s_base;
	s_free->size = s_size - HDR;
	s_free->magic = MAGIC_FREE;
	s_free->next = NULL;
	s_used_peak = 0;
	return 1;
}

void chd_heap_shutdown(void)
{
	if (!s_base) {
		s_free = NULL;
		return;
	}
	s_free = (chd_blk_t *)s_base;
	s_free->size = s_size - HDR;
	s_free->magic = MAGIC_FREE;
	s_free->next = NULL;
	s_used_peak = 0;
}

size_t chd_heap_used(void) { return s_used_peak; }
size_t chd_heap_size(void) { return s_size; }
size_t chd_heap_mark(void) { return 0; }          /* unused with freelist */
void chd_heap_release_to(size_t mark) { (void)mark; }

void *chd_arena_malloc(size_t n)
{
	chd_blk_t *prev = NULL, *cur, *best_prev = NULL, *best = NULL;
	size_t need;

	if (n == 0)
		n = 1;
	need = align_up(n);
	if (!s_base)
		return s_sys_malloc(need);

	for (cur = s_free; cur; prev = cur, cur = cur->next) {
		if (cur->magic != MAGIC_FREE || !blk_in_heap(cur))
			continue;
		if (cur->size >= need && (!best || cur->size < best->size)) {
			best = cur;
			best_prev = prev;
		}
	}
	if (!best) {
		printf("CHD heap OOM need=%u peak=%u size=%u\n",
		       (unsigned)need, (unsigned)s_used_peak, (unsigned)s_size);
		return NULL;
	}

	/* Unlink */
	if (best_prev)
		best_prev->next = best->next;
	else
		s_free = best->next;

	/* Split */
	if (best->size >= need + HDR + 32u) {
		chd_blk_t *rest = (chd_blk_t *)((uint8_t *)blk_to_payload(best) + need);
		rest->size = best->size - need - HDR;
		rest->magic = MAGIC_FREE;
		rest->next = s_free;
		s_free = rest;
		best->size = need;
	}

	best->next = NULL;
	best->magic = MAGIC_USED;
	{
		size_t end = (size_t)(((uint8_t *)blk_to_payload(best) + best->size) - s_base);
		if (end > s_used_peak)
			s_used_peak = end;
	}
	return blk_to_payload(best);
}

void *chd_arena_calloc(size_t n, size_t sz)
{
	size_t total;
	void *p;
	if (n && sz > (size_t)-1 / n)
		return NULL;
	total = n * sz;
	p = chd_arena_malloc(total);
	if (p)
		memset(p, 0, total);
	return p;
}

void chd_arena_free(void *p)
{
	chd_blk_t *blk;
	if (!p)
		return;
	if (!s_base) {
		s_sys_free(p);
		return;
	}
	blk = payload_to_blk(p);
	if (!blk_in_heap(blk) || blk->magic != MAGIC_USED)
		return;
	blk->magic = MAGIC_FREE;
	blk->next = s_free;
	s_free = blk;
	coalesce();
}

void *chd_arena_realloc(void *p, size_t n)
{
	chd_blk_t *blk;
	void *q;
	size_t copy;
	if (!p)
		return chd_arena_malloc(n);
	if (n == 0) {
		chd_arena_free(p);
		return NULL;
	}
	if (!s_base)
		return s_sys_realloc(p, n);

	blk = payload_to_blk(p);
	if (!blk_in_heap(blk) || blk->magic != MAGIC_USED)
		return NULL;
	if (blk->size >= align_up(n))
		return p;

	q = chd_arena_malloc(n);
	if (!q)
		return NULL;
	copy = blk->size < n ? blk->size : n;
	memcpy(q, p, copy);
	chd_arena_free(p);
	return q;
}
