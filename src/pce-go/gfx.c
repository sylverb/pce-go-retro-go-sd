// gfx.c - VDC/VCE Emulation (+ SuperGrafx VPC mix)
//
#include <stdlib.h>
#include <string.h>
#include "utils.h"
#include "pce.h"
#include "gfx.h"
#include "gw_malloc.h"

typedef struct
{
	uint16_t y;
	uint16_t x;
	uint16_t no;
	uint16_t attr;
} sprite_t;

#define PAL(nibble) (PAL[(L >> ((nibble) * 4)) & 15])

#define V_FLIP  0x8000
#define H_FLIP  0x0800

#define VPC_ALPHA   0x100
#define VPC_SPRITE  0x400

#define SPR_SLICE_H  16
#define SPR_BUF_W    (XBUF_WIDTH + 64)
#define SPR_XOFS     32
/* Fine-scroll can write up to 7px left of the strip origin. */
#define STRIP_XPAD   8

static int last_line_counter = 0;
static int line_counter = 0;

static struct {
	int scroll_x;
	int scroll_y;
	int control;
	int latched;
} gfx_context[2];

static uint16_t *spr_buf_mem;
static uint16_t (*spr_buf)[SPR_BUF_W];
static uint16_t spr_rmin[SPR_SLICE_H];
static uint16_t spr_rmax[SPR_SLICE_H];

static uint16_t *vpc_lb[2];
/* SuperGrafx strip buffers (SPR_SLICE_H lines) — RAM_EMU, frees DTCM */
static uint16_t *vpc_strip_mem[2];
static uint16_t *vpc_strip[2];
static int vpc_strip_pitch;

/* DTCM hot mirrors — Palette lookups dominate tile/sprite/mix loops. */
static uint8_t *gfx_pal;
/* Plane-byte → packed-nibble expand (same layout as the old bit dance). */
static uint32_t *tile_exp;

static const int prio_select[4] = {1, 1, 0, 0};
static const int prio_shift[4] = {4, 0, 4, 0};

static inline void
fill_u16(uint16_t *p, uint16_t v, int n)
{
	/* Unroll a bit for clear-to-ALPHA which dominates SGX strip setup */
	int i = 0;
	for (; i + 4 <= n; i += 4) {
		p[i] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
	}
	for (; i < n; i++)
		p[i] = v;
}

static inline void
fill_strip_rows(uint16_t *strip, int pitch, int rows, int width, uint16_t v)
{
	for (int r = 0; r < rows; r++)
		fill_u16(strip + r * pitch, v, width);
}

static inline uint8_t *
gfx_pal_base(void)
{
	return gfx_pal ? gfx_pal : PCE.Palette;
}

void
gfx_palette_write(uint16_t n, uint8_t c)
{
	if (n == 0) {
		for (int i = 0; i < 256; i += 16) {
			PCE.Palette[i] = c;
			if (gfx_pal) gfx_pal[i] = c;
		}
	} else if (n & 15) {
		PCE.Palette[n] = c;
		if (gfx_pal) gfx_pal[n] = c;
	}
}

void
gfx_palette_reload(void)
{
	if (gfx_pal)
		memcpy(gfx_pal, PCE.Palette, 512);
}

static inline uint32_t
gfx_screen_width_v(vdc_t *vdc)
{
	uint32_t w = VDC_SCREEN_WIDTH(vdc);
	return (w > XBUF_WIDTH) ? XBUF_WIDTH : w;
}

static inline uint32_t
gfx_screen_width(void)
{
	return gfx_screen_width_v(&PCE.vdc[0]);
}

static void
draw_tiles_u8(vdc_t *vdc, uint8_t *screen_buffer, int Y1, int Y2, int scroll_x, int scroll_y)
{
	const uint8_t _bg_w[] = { 32, 64, 128, 128 };
	const uint8_t _bg_h[] = { 32, 64 };
	uint16_t *vram = vdc->vram_mem;
	uint32_t bg_w = _bg_w[(vdc->regs[MWR].W >> 4) & 3];
	uint32_t bg_h = _bg_h[(vdc->regs[MWR].W >> 6) & 1];
	uint8_t *PALBASE = gfx_pal_base();
	const uint32_t *EXP = tile_exp;
	int XW, no, x, y, h, offset;
	uint8_t *PP, *PAL, *P, *C;

	y = Y1 + scroll_y;
	offset = y & 7;
	h = 8 - offset;
	if (h > Y2 - Y1)
		h = Y2 - Y1;
	y >>= 3;
	PP = (screen_buffer + XBUF_WIDTH * Y1) - (scroll_x & 7);
	XW = (int)gfx_screen_width_v(vdc) / 8 + 1;

	for (int Line = Y1; Line < Y2; y++) {
		x = scroll_x / 8;
		y &= bg_h - 1;
		for (int X1 = 0; X1 < XW; X1++, x++, PP += 8) {
			x &= bg_w - 1;
			no = vram[x + y * bg_w];
			PAL = &PALBASE[(no >> 8) & 0x1F0];
			no &= 0x7FF;
			C = (uint8_t *)(vram + no * 16 + offset);
			P = PP;
			for (int i = 0; i < h; i++, P += XBUF_WIDTH, C += 2) {
				uint32_t J, L;
				J = C[0] | C[1] | C[16] | C[17];
				if (!J) continue;
				if (EXP) {
					L = EXP[C[0]] | (EXP[C[1]] << 1) | (EXP[C[16]] << 2) | (EXP[C[17]] << 3);
				} else {
					uint32_t M;
					M = C[0];
					L = ((M & 0x88) >> 3) | ((M & 0x44) << 6) | ((M & 0x22) << 15) | ((M & 0x11) << 24);
					M = C[1];
					L |= ((M & 0x88) >> 2) | ((M & 0x44) << 7) | ((M & 0x22) << 16) | ((M & 0x11) << 25);
					M = C[16];
					L |= ((M & 0x88) >> 1) | ((M & 0x44) << 8) | ((M & 0x22) << 17) | ((M & 0x11) << 26);
					M = C[17];
					L |= ((M & 0x88) >> 0) | ((M & 0x44) << 9) | ((M & 0x22) << 18) | ((M & 0x11) << 27);
				}
				if (J == 0xFF) {
					P[0] = PAL(1); P[1] = PAL(3); P[2] = PAL(5); P[3] = PAL(7);
					P[4] = PAL(0); P[5] = PAL(2); P[6] = PAL(4); P[7] = PAL(6);
				} else {
					if (J & 0x80) P[0] = PAL(1);
					if (J & 0x40) P[1] = PAL(3);
					if (J & 0x20) P[2] = PAL(5);
					if (J & 0x10) P[3] = PAL(7);
					if (J & 0x08) P[4] = PAL(0);
					if (J & 0x04) P[5] = PAL(2);
					if (J & 0x02) P[6] = PAL(4);
					if (J & 0x01) P[7] = PAL(6);
				}
			}
		}
		Line += h;
		PP += XBUF_WIDTH * h - XW * 8;
		offset = 0;
		h = Y2 - Line;
		if (h > 8) h = 8;
	}
}

static void
draw_tiles_u16(vdc_t *vdc, uint16_t *line_base, int pitch, int Y1, int Y2, int scroll_x, int scroll_y)
{
	const uint8_t _bg_w[] = { 32, 64, 128, 128 };
	const uint8_t _bg_h[] = { 32, 64 };
	uint16_t *vram = vdc->vram_mem;
	uint32_t bg_w = _bg_w[(vdc->regs[MWR].W >> 4) & 3];
	uint32_t bg_h = _bg_h[(vdc->regs[MWR].W >> 6) & 1];
	uint8_t *PALBASE = gfx_pal_base();
	const uint32_t *EXP = tile_exp;
	int XW, no, x, y, h, offset;
	uint16_t *PP, *P;
	uint8_t *PAL, *C;

	y = Y1 + scroll_y;
	offset = y & 7;
	h = 8 - offset;
	if (h > Y2 - Y1) h = Y2 - Y1;
	y >>= 3;
	PP = line_base + Y1 * pitch - (scroll_x & 7);
	XW = (int)gfx_screen_width_v(vdc) / 8 + 1;

	for (int Line = Y1; Line < Y2; y++) {
		x = scroll_x / 8;
		y &= bg_h - 1;
		for (int X1 = 0; X1 < XW; X1++, x++, PP += 8) {
			x &= bg_w - 1;
			no = vram[x + y * bg_w];
			PAL = &PALBASE[(no >> 8) & 0x1F0];
			no &= 0x7FF;
			C = (uint8_t *)(vram + no * 16 + offset);
			P = PP;
			for (int i = 0; i < h; i++, P += pitch, C += 2) {
				uint32_t J, L;
				J = C[0] | C[1] | C[16] | C[17];
				if (!J) continue;
				if (EXP) {
					L = EXP[C[0]] | (EXP[C[1]] << 1) | (EXP[C[16]] << 2) | (EXP[C[17]] << 3);
				} else {
					uint32_t M;
					M = C[0];
					L = ((M & 0x88) >> 3) | ((M & 0x44) << 6) | ((M & 0x22) << 15) | ((M & 0x11) << 24);
					M = C[1];
					L |= ((M & 0x88) >> 2) | ((M & 0x44) << 7) | ((M & 0x22) << 16) | ((M & 0x11) << 25);
					M = C[16];
					L |= ((M & 0x88) >> 1) | ((M & 0x44) << 8) | ((M & 0x22) << 17) | ((M & 0x11) << 26);
					M = C[17];
					L |= ((M & 0x88) >> 0) | ((M & 0x44) << 9) | ((M & 0x22) << 18) | ((M & 0x11) << 27);
				}
				if (J == 0xFF) {
					P[0] = PAL(1); P[1] = PAL(3); P[2] = PAL(5); P[3] = PAL(7);
					P[4] = PAL(0); P[5] = PAL(2); P[6] = PAL(4); P[7] = PAL(6);
				} else {
					if (J & 0x80) P[0] = PAL(1);
					if (J & 0x40) P[1] = PAL(3);
					if (J & 0x20) P[2] = PAL(5);
					if (J & 0x10) P[3] = PAL(7);
					if (J & 0x08) P[4] = PAL(0);
					if (J & 0x04) P[5] = PAL(2);
					if (J & 0x02) P[6] = PAL(4);
					if (J & 0x01) P[7] = PAL(6);
				}
			}
		}
		Line += h;
		PP += pitch * h - XW * 8;
		offset = 0;
		h = Y2 - Line;
		if (h > 8) h = 8;
	}
}

static void
draw_sprite_row(uint16_t *B, const uint16_t *C, uint16_t flags, uint16_t attr)
{
	uint8_t *PAL = &gfx_pal_base()[256 + ((attr & 0xF) << 4)];
	uint16_t J = C[0] | C[16] | C[32] | C[48];
	uint32_t L1, L2, L;
	if (!J) return;
	if (tile_exp) {
		const uint32_t *EXP = tile_exp;
		uint16_t p0 = C[0], p1 = C[16], p2 = C[32], p3 = C[48];
		L1 = EXP[p0 & 0xFF] | (EXP[p1 & 0xFF] << 1) | (EXP[p2 & 0xFF] << 2) | (EXP[p3 & 0xFF] << 3);
		L2 = EXP[p0 >> 8] | (EXP[p1 >> 8] << 1) | (EXP[p2 >> 8] << 2) | (EXP[p3 >> 8] << 3);
	} else {
		uint32_t M;
		M = C[0];
		L1 = ((M & 0x88) >> 3) | ((M & 0x44) << 6) | ((M & 0x22) << 15) | ((M & 0x11) << 24);
		L2 = ((M & 0x8800) >> 11) | ((M & 0x4400) >> 2) | ((M & 0x2200) << 7) | ((M & 0x1100) << 16);
		M = C[16];
		L1 |= ((M & 0x88) >> 2) | ((M & 0x44) << 7) | ((M & 0x22) << 16) | ((M & 0x11) << 25);
		L2 |= ((M & 0x8800) >> 10) | ((M & 0x4400) >> 1) | ((M & 0x2200) << 8) | ((M & 0x1100) << 17);
		M = C[32];
		L1 |= ((M & 0x88) >> 1) | ((M & 0x44) << 8) | ((M & 0x22) << 17) | ((M & 0x11) << 26);
		L2 |= ((M & 0x8800) >> 9) | ((M & 0x4400) >> 0) | ((M & 0x2200) << 9) | ((M & 0x1100) << 18);
		M = C[48];
		L1 |= ((M & 0x88) >> 0) | ((M & 0x44) << 9) | ((M & 0x22) << 18) | ((M & 0x11) << 27);
		L2 |= ((M & 0x8800) >> 8) | ((M & 0x4400) << 1) | ((M & 0x2200) << 10) | ((M & 0x1100) << 19);
	}
	if (attr & H_FLIP) {
		L = L2;
		if ((J & 0x8000)) B[15] = flags | PAL(1);
		if ((J & 0x4000)) B[14] = flags | PAL(3);
		if ((J & 0x2000)) B[13] = flags | PAL(5);
		if ((J & 0x1000)) B[12] = flags | PAL(7);
		if ((J & 0x0800)) B[11] = flags | PAL(0);
		if ((J & 0x0400)) B[10] = flags | PAL(2);
		if ((J & 0x0200)) B[9]  = flags | PAL(4);
		if ((J & 0x0100)) B[8]  = flags | PAL(6);
		L = L1;
		if ((J & 0x80)) B[7] = flags | PAL(1);
		if ((J & 0x40)) B[6] = flags | PAL(3);
		if ((J & 0x20)) B[5] = flags | PAL(5);
		if ((J & 0x10)) B[4] = flags | PAL(7);
		if ((J & 0x08)) B[3] = flags | PAL(0);
		if ((J & 0x04)) B[2] = flags | PAL(2);
		if ((J & 0x02)) B[1] = flags | PAL(4);
		if ((J & 0x01)) B[0] = flags | PAL(6);
	} else {
		L = L2;
		if ((J & 0x8000)) B[0] = flags | PAL(1);
		if ((J & 0x4000)) B[1] = flags | PAL(3);
		if ((J & 0x2000)) B[2] = flags | PAL(5);
		if ((J & 0x1000)) B[3] = flags | PAL(7);
		if ((J & 0x0800)) B[4] = flags | PAL(0);
		if ((J & 0x0400)) B[5] = flags | PAL(2);
		if ((J & 0x0200)) B[6] = flags | PAL(4);
		if ((J & 0x0100)) B[7] = flags | PAL(6);
		L = L1;
		if ((J & 0x80)) B[8]  = flags | PAL(1);
		if ((J & 0x40)) B[9]  = flags | PAL(3);
		if ((J & 0x20)) B[10] = flags | PAL(5);
		if ((J & 0x10)) B[11] = flags | PAL(7);
		if ((J & 0x08)) B[12] = flags | PAL(0);
		if ((J & 0x04)) B[13] = flags | PAL(2);
		if ((J & 0x02)) B[14] = flags | PAL(4);
		if ((J & 0x01)) B[15] = flags | PAL(6);
	}
}

static bool
sprites_decode_slice(vdc_t *vdc, int Y1, int Y2)
{
	bool has_prio1 = false;
	uint16_t *vram = vdc->vram_mem;
	sprite_t *spram = (sprite_t *)vdc->spram;
	int width = (int)gfx_screen_width_v(vdc);

	for (int i = 0; i < SPR_SLICE_H; i++) {
		if (spr_rmax[i] > spr_rmin[i])
			memset(&spr_buf[i][spr_rmin[i]], 0, (spr_rmax[i] - spr_rmin[i]) * sizeof(uint16_t));
		spr_rmin[i] = SPR_BUF_W;
		spr_rmax[i] = 0;
	}

	for (int n = 63; n >= 0; n--) {
		sprite_t *spr = spram + n;
		/* Empty SATB slots are typically y=0 (display y = -64). */
		if ((spr->y & 0x3FF) == 0)
			continue;
		uint16_t attr = spr->attr;
		int y = (spr->y & 0x3FF) - 64;
		int x = (spr->x & 0x3FF) - 32;
		int cgx = (attr >> 8) & 1;
		int cgy = (attr >> 12) & 3;
		int no = (spr->no & 0x7FF);
		cgy |= cgy >> 1;
		no = (no >> 1) & ~(cgy * 2 + cgx);
		no &= 0x1FF;
		int height = (cgy + 1) * 16;
		if (y >= Y2 || y + height <= Y1 || x >= width || x + (cgx + 1) * 16 < 0)
			continue;
		uint16_t flags = 0x100 | ((attr & 0x80) ? 0x8000 : 0);
		if (flags & 0x8000) has_prio1 = true;
		const uint16_t *C = vram + (no * 64);
		int r0 = (y < Y1) ? Y1 : y;
		int r1 = (y + height > Y2) ? Y2 : (y + height);
		for (int r = r0; r < r1; r++) {
			int yo = r - y;
			if (attr & V_FLIP) yo = height - 1 - yo;
			const uint16_t *Crow = C + ((yo >> 4) * 128) + (yo & 15);
			uint16_t *B = &spr_buf[r - Y1][SPR_XOFS + x];
			for (int j = 0; j <= cgx; j++) {
				const uint16_t *cell = Crow + (((attr & H_FLIP) ? (cgx - j) : j) * 64);
				draw_sprite_row(B + j * 16, cell, flags, attr);
			}
			int bx0 = SPR_XOFS + x;
			int bx1 = bx0 + (cgx + 1) * 16;
			if (bx0 < spr_rmin[r - Y1]) spr_rmin[r - Y1] = bx0;
			if (bx1 > spr_rmax[r - Y1]) spr_rmax[r - Y1] = bx1;
		}
	}
	return has_prio1;
}

static void
sprites_apply_u8(uint8_t *screen_buffer, int Y1, int Y2, int prio)
{
	int width = (int)gfx_screen_width();
	for (int r = 0; r < Y2 - Y1; r++) {
		int bmin = spr_rmin[r], bmax = spr_rmax[r];
		if (bmin >= bmax) continue;
		int x0 = bmin - SPR_XOFS;
		int x1 = bmax - SPR_XOFS;
		if (x0 < 0) x0 = 0;
		if (x1 > width) x1 = width;
		uint8_t *fb = screen_buffer + (Y1 + r) * XBUF_WIDTH;
		const uint16_t *B = &spr_buf[r][SPR_XOFS];
		if (prio) {
			for (int x = x0; x < x1; x++) {
				uint16_t e = B[x];
				if (e & 0x8000) fb[x] = (uint8_t)e;
			}
		} else {
			for (int x = x0; x < x1; x++) {
				uint16_t e = B[x];
				if (e && !(e & 0x8000)) fb[x] = (uint8_t)e;
			}
		}
	}
}

static void
sprites_apply_u16(uint16_t *strip, int pitch, int Y1, int Y2, int prio, int width)
{
	for (int r = 0; r < Y2 - Y1; r++) {
		int bmin = spr_rmin[r], bmax = spr_rmax[r];
		if (bmin >= bmax)
			continue;
		int x0 = bmin - SPR_XOFS;
		int x1 = bmax - SPR_XOFS;
		if (x0 < 0) x0 = 0;
		if (x1 > width) x1 = width;
		uint16_t *lb = strip + r * pitch;
		const uint16_t *B = &spr_buf[r][SPR_XOFS];
		if (prio) {
			for (int x = x0; x < x1; x++) {
				uint16_t e = B[x];
				if (e & 0x8000)
					lb[x] = ((uint8_t)e) | VPC_SPRITE;
			}
		} else {
			for (int x = x0; x < x1; x++) {
				uint16_t e = B[x];
				if (e && !(e & 0x8000))
					lb[x] = ((uint8_t)e) | VPC_SPRITE;
			}
		}
	}
}

static inline bool
sprite_hit_check(vdc_t *vdc)
{
	sprite_t *spr = (sprite_t *)vdc->spram;
	int x0 = spr->x, y0 = spr->y;
	int w0 = (((spr->attr >> 8) & 1) + 1) * 16;
	int h0 = (((spr->attr >> 12) & 3) + 1) * 16;
	spr++;
	for (int i = 1; i < 64; i++, spr++) {
		int x = spr->x, y = spr->y;
		int w = (((spr->attr >> 8) & 1) + 1) * 16;
		int h = (((spr->attr >> 12) & 3) + 1) * 16;
		if ((x < x0 + w0) && (x + w > x0) && (y < y0 + h0) && (y + h > y0))
			return 1;
	}
	return 0;
}

void gfx_latch_context(int chip, int force)
{
	vdc_t *vdc = &PCE.vdc[chip & 1];
	if (!gfx_context[chip].latched || force) {
		gfx_context[chip].scroll_x = vdc->regs[BXR].W;
		int sy = (int)vdc->regs[BYR].W - vdc->scroll_y_diff;
		if (sy > 0) sy -= 1;
		gfx_context[chip].scroll_y = sy;
		gfx_context[chip].control = vdc->regs[CR].W;
		gfx_context[chip].latched = 1;
	}
}

/* Compose one Mednafen vpc_mix_inner step; bg is Palette[0]. */
static inline uint8_t
vpc_compose_pixel(uint16_t vdc1_pixel, uint16_t vdc2_pixel, uint8_t pb, uint8_t bg)
{
	uint16_t a = VPC_ALPHA, b = VPC_ALPHA;
	if (pb & 1) a = vdc1_pixel;
	if (pb & 2) b = vdc2_pixel;
	switch (pb >> 2) {
	case 1:
		a |= (((b ^ a) & b) >> 2) & VPC_ALPHA;
		break;
	case 2: {
		const uint16_t intermediate = ((a ^ b) & a) >> 2;
		a |= (intermediate ^ b) & intermediate & VPC_ALPHA;
		break;
	}
	default:
		break;
	}
	if (a & VPC_ALPHA)
		return (b & VPC_ALPHA) ? bg : (uint8_t)b;
	return (uint8_t)a;
}

static void
mix_vpc_line(uint8_t *dst, const uint16_t *lb0, const uint16_t *lb1, int count)
{
	const uint8_t bg = gfx_pal_base()[0];
	const int win0 = (int)PCE.vpc.winwidths[0] - 0x40;
	const int win1 = (int)PCE.vpc.winwidths[1] - 0x40;
	const int windows = (PCE.vpc.winwidths[0] > 0x40) || (PCE.vpc.winwidths[1] > 0x40);

	if (!windows) {
		const uint8_t pb = (PCE.vpc.priority[prio_select[0]] >> prio_shift[0]) & 0xF;
		/* Hot common cases: single layer or simple OR without sprite priority. */
		if ((pb >> 2) == 0) {
			if (pb == 0x1) {
				for (int x = 0; x < count; x++) {
					uint16_t p = lb0[x];
					dst[x] = (p & VPC_ALPHA) ? bg : (uint8_t)p;
				}
				return;
			}
			if (pb == 0x2) {
				for (int x = 0; x < count; x++) {
					uint16_t p = lb1[x];
					dst[x] = (p & VPC_ALPHA) ? bg : (uint8_t)p;
				}
				return;
			}
			if (pb == 0x3) {
				/* VDC1 in front of VDC2, no sprite-priority fiddling */
				for (int x = 0; x < count; x++) {
					uint16_t a = lb0[x], b = lb1[x];
					dst[x] = (a & VPC_ALPHA) ? ((b & VPC_ALPHA) ? bg : (uint8_t)b) : (uint8_t)a;
				}
				return;
			}
		} else if (pb == 0x5) {
			/* Both layers + Mednafen case 1 (VDC2 sprite punches VDC1 BG).
			 * Dai Makai Mura / many SGX titles stay here every pixel. */
			for (int x = 0; x < count; x++) {
				uint16_t a = lb0[x], b = lb1[x];
				a |= (((b ^ a) & b) >> 2) & VPC_ALPHA;
				dst[x] = (a & VPC_ALPHA) ? ((b & VPC_ALPHA) ? bg : (uint8_t)b) : (uint8_t)a;
			}
			return;
		}
		for (int x = 0; x < count; x++)
			dst[x] = vpc_compose_pixel(lb0[x], lb1[x], pb, bg);
		return;
	}

	for (int x = 0; x < count; x++) {
		int in_window = 0;
		if (x < win0) in_window |= 1;
		if (x < win1) in_window |= 2;
		uint8_t pb = (PCE.vpc.priority[prio_select[in_window]] >> prio_shift[in_window]) & 0xF;
		dst[x] = vpc_compose_pixel(lb0[x], lb1[x], pb, bg);
	}
}

static void
render_vdc_u8(vdc_t *vdc, int chip, uint8_t *screen_buffer, int min_line, int max_line)
{
	size_t screen_width = gfx_screen_width_v(vdc);
	const uint8_t bg = gfx_pal_base()[0];
	gfx_context[chip].latched = 0;
	for (int y = min_line; y < max_line; y++)
		memset(screen_buffer + (y * XBUF_WIDTH), bg, screen_width);
	for (int Y1 = min_line; Y1 < max_line; Y1 += SPR_SLICE_H) {
		int Y2 = Y1 + SPR_SLICE_H;
		if (Y2 > max_line) Y2 = max_line;
		bool has_prio1 = false;
		if (gfx_context[chip].control & 0x40) {
			has_prio1 = sprites_decode_slice(vdc, Y1, Y2);
			sprites_apply_u8(screen_buffer, Y1, Y2, 0);
		}
		if (gfx_context[chip].control & 0x80)
			draw_tiles_u8(vdc, screen_buffer, Y1, Y2, gfx_context[chip].scroll_x, gfx_context[chip].scroll_y);
		if (has_prio1)
			sprites_apply_u8(screen_buffer, Y1, Y2, 1);
	}
}

/* Render one VDC into a uint16 strip [0 .. Y2-Y1) at vpc_strip[chip]. */
static void
render_vdc_strip_u16(vdc_t *vdc, int chip, int Y1, int Y2)
{
	int width = (int)gfx_screen_width_v(vdc);
	int pitch = vpc_strip_pitch;
	uint16_t *strip = vpc_strip[chip];
	int rows = Y2 - Y1;
	int ctrl = gfx_context[chip].control;

	fill_strip_rows(strip, pitch, rows, width, VPC_ALPHA);

	if (!(ctrl & 0xC0))
		return;

	bool has_prio1 = false;
	if (ctrl & 0x40) {
		has_prio1 = sprites_decode_slice(vdc, Y1, Y2);
		sprites_apply_u16(strip, pitch, Y1, Y2, 0, width);
	}
	if (ctrl & 0x80)
		draw_tiles_u16(vdc, strip - Y1 * pitch, pitch, Y1, Y2,
			gfx_context[chip].scroll_x, gfx_context[chip].scroll_y);
	if (has_prio1)
		sprites_apply_u16(strip, pitch, Y1, Y2, 1, width);
}

static void
render_lines(int min_line, int max_line)
{
	uint8_t *screen_buffer = osd_gfx_framebuffer();
	if (!screen_buffer) return;

	if (!PCE.IsSGX) {
		render_vdc_u8(&PCE.vdc[0], 0, screen_buffer, min_line, max_line);
		return;
	}

	const int windows = (PCE.vpc.winwidths[0] > 0x40) || (PCE.vpc.winwidths[1] > 0x40);
	const uint8_t pb0 = (PCE.vpc.priority[prio_select[0]] >> prio_shift[0]) & 0xF;
	const int need0 = (gfx_context[0].control & 0xC0) != 0;
	const int need1 = (gfx_context[1].control & 0xC0) != 0;

	/* Idle second VDC + simple VDC1-only priority → skip strip/mix entirely. */
	if (!windows && !need1 && (pb0 == 0x1 || pb0 == 0x0)) {
		render_vdc_u8(&PCE.vdc[0], 0, screen_buffer, min_line, max_line);
		gfx_context[1].latched = 0;
		return;
	}
	if (!windows && !need0 && pb0 == 0x2) {
		render_vdc_u8(&PCE.vdc[1], 1, screen_buffer, min_line, max_line);
		gfx_context[0].latched = 0;
		return;
	}

	/* Slow fallback: per-line into DTCM linebufs if strip alloc failed */
	if (!vpc_strip[0] || !vpc_strip[1]) {
		int width = (int)gfx_screen_width();
		if (!vpc_lb[0] || !vpc_lb[1]) {
			render_vdc_u8(&PCE.vdc[0], 0, screen_buffer, min_line, max_line);
			return;
		}
		for (int y = min_line; y < max_line; y++) {
			fill_u16(vpc_lb[0], VPC_ALPHA, width);
			fill_u16(vpc_lb[1], VPC_ALPHA, width);
			{
				bool has_prio1 = false;
				if (gfx_context[0].control & 0x40) {
					has_prio1 = sprites_decode_slice(&PCE.vdc[0], y, y + 1);
					sprites_apply_u16(vpc_lb[0], width, y, y + 1, 0, width);
				}
				if (gfx_context[0].control & 0x80)
					draw_tiles_u16(&PCE.vdc[0], vpc_lb[0] - y * width, width, y, y + 1,
						gfx_context[0].scroll_x, gfx_context[0].scroll_y);
				if (has_prio1)
					sprites_apply_u16(vpc_lb[0], width, y, y + 1, 1, width);
			}
			{
				bool has_prio1 = false;
				if (gfx_context[1].control & 0x40) {
					has_prio1 = sprites_decode_slice(&PCE.vdc[1], y, y + 1);
					sprites_apply_u16(vpc_lb[1], width, y, y + 1, 0, width);
				}
				if (gfx_context[1].control & 0x80)
					draw_tiles_u16(&PCE.vdc[1], vpc_lb[1] - y * width, width, y, y + 1,
						gfx_context[1].scroll_x, gfx_context[1].scroll_y);
				if (has_prio1)
					sprites_apply_u16(vpc_lb[1], width, y, y + 1, 1, width);
			}
			mix_vpc_line(screen_buffer + y * XBUF_WIDTH, vpc_lb[0], vpc_lb[1], width);
		}
		gfx_context[0].latched = 0;
		gfx_context[1].latched = 0;
		return;
	}

	int width = (int)gfx_screen_width();
	int pitch = vpc_strip_pitch;

	for (int Y1 = min_line; Y1 < max_line; Y1 += SPR_SLICE_H) {
		int Y2 = Y1 + SPR_SLICE_H;
		if (Y2 > max_line) Y2 = max_line;
		int rows = Y2 - Y1;

		if (need0)
			render_vdc_strip_u16(&PCE.vdc[0], 0, Y1, Y2);
		else
			fill_strip_rows(vpc_strip[0], pitch, rows, width, VPC_ALPHA);

		if (need1)
			render_vdc_strip_u16(&PCE.vdc[1], 1, Y1, Y2);
		else
			fill_strip_rows(vpc_strip[1], pitch, rows, width, VPC_ALPHA);

		for (int y = Y1; y < Y2; y++) {
			mix_vpc_line(screen_buffer + y * XBUF_WIDTH,
				vpc_strip[0] + (y - Y1) * pitch,
				vpc_strip[1] + (y - Y1) * pitch,
				width);
		}
	}
	gfx_context[0].latched = 0;
	gfx_context[1].latched = 0;
}

void
gfx_sgx_alloc(void)
{
	vpc_strip_pitch = XBUF_WIDTH;
	size_t row_words = (size_t)vpc_strip_pitch + STRIP_XPAD;
	size_t strip_bytes = (size_t)SPR_SLICE_H * row_words * sizeof(uint16_t);

	for (int i = 0; i < 2; i++) {
		if (vpc_strip[i])
			continue;
		vpc_strip_mem[i] = (uint16_t *)ram_malloc(strip_bytes);
		if (!vpc_strip_mem[i])
			vpc_strip_mem[i] = (uint16_t *)ahb_malloc(strip_bytes);
		if (vpc_strip_mem[i])
			vpc_strip[i] = vpc_strip_mem[i] + STRIP_XPAD;
	}
	/* Keep one-line DTCM buffers as optional scratch (unused if strips ok). */
	if (!vpc_lb[0]) {
		uint16_t *p = (uint16_t *)dtc_malloc((XBUF_WIDTH + STRIP_XPAD) * sizeof(uint16_t));
		if (p) vpc_lb[0] = p + STRIP_XPAD;
	}
	if (!vpc_lb[1]) {
		uint16_t *p = (uint16_t *)dtc_malloc((XBUF_WIDTH + STRIP_XPAD) * sizeof(uint16_t));
		if (p) vpc_lb[1] = p + STRIP_XPAD;
	}

	if (!vpc_strip[0] || !vpc_strip[1])
		MESSAGE_INFO("SGX: strip alloc failed — falling back to slow path\n");
}

static void
gfx_init_luts(void)
{
	if (!gfx_pal) {
		gfx_pal = (uint8_t *)dtc_calloc(512, 1);
		if (!gfx_pal)
			gfx_pal = (uint8_t *)ram_calloc(512, 1);
		if (gfx_pal)
			memcpy(gfx_pal, PCE.Palette, 512);
	}
	if (!tile_exp) {
		tile_exp = (uint32_t *)dtc_malloc(256 * sizeof(uint32_t));
		if (!tile_exp)
			tile_exp = (uint32_t *)ram_malloc(256 * sizeof(uint32_t));
		if (tile_exp) {
			for (int M = 0; M < 256; M++) {
				tile_exp[M] =
					((M & 0x88) >> 3) | ((M & 0x44) << 6) |
					((M & 0x22) << 15) | ((M & 0x11) << 24);
			}
		}
	}
}

int
gfx_init(void)
{
	gfx_init_luts();
	if (!spr_buf) {
		spr_buf_mem = (uint16_t *)dtc_calloc(SPR_SLICE_H * SPR_BUF_W, sizeof(uint16_t));
		if (!spr_buf_mem)
			spr_buf_mem = (uint16_t *)ram_calloc(SPR_SLICE_H * SPR_BUF_W, sizeof(uint16_t));
		spr_buf = (uint16_t (*)[SPR_BUF_W])spr_buf_mem;
	}
	gfx_reset(true);
	return 0;
}

void
gfx_reset(bool hard)
{
	last_line_counter = 0;
	line_counter = 0;
	if (hard) {
		for (int i = 0; i < 2; i++) {
			gfx_context[i].latched = 0;
			gfx_context[i].scroll_x = 0;
			gfx_context[i].scroll_y = 0;
			gfx_context[i].control = 0;
		}
	}
}

void gfx_term(void) {}

void
gfx_irq(int chip, int type)
{
	vdc_t *vdc = &PCE.vdc[chip & 1];
	if (type >= 0) {
		vdc->pending_irqs <<= 4;
		vdc->pending_irqs |= (1 + type) & 0xF;
	}
	int pos = 28;
	while (!(CPU_PCE.irq_lines & INT_IRQ1) && vdc->pending_irqs) {
		if (vdc->pending_irqs >> pos) {
			vdc->status |= 1 << ((vdc->pending_irqs >> pos) - 1);
			vdc->pending_irqs &= ~(0xF << pos);
			CPU_PCE.irq_lines |= INT_IRQ1;
		}
		pos -= 4;
	}
}

static void
vram_dma_run_chunk(vdc_t *vdc, int chip)
{
	uint16_t *vram = vdc->vram_mem;
	int src_inc = (vdc->regs[DCR].W & 8) ? -1 : 1;
	int dst_inc = (vdc->regs[DCR].W & 4) ? -1 : 1;
	for (int i = 0; i < 227; i++) {
		if (vdc->regs[DISTR].W < 0x8000)
			vram[vdc->regs[DISTR].W] = vram[vdc->regs[SOUR].W];
		vdc->regs[SOUR].W += src_inc;
		vdc->regs[DISTR].W += dst_inc;
		vdc->regs[LENR].W -= 1;
		if (vdc->regs[LENR].W == 0xFFFF) {
			vdc->vram = 0;
			if (vdc->regs[DCR].W & 0x02)
				gfx_irq(chip, VDC_STAT_DV);
			break;
		}
	}
}

static void
gfx_run_vdc_timing(vdc_t *vdc, int chip, int scanline, bool *need_vbi)
{
	if (scanline == 0 && chip == 0) {
		PCE.VBlankFL = VDC_MAXLINE(vdc) + 1;
		if (PCE.VBlankFL > 261) PCE.VBlankFL = 261;
		if (vdc->mode_chg) {
			vdc->mode_chg = 0;
			osd_gfx_set_mode(VDC_SCREEN_WIDTH(vdc), VDC_SCREEN_HEIGHT(vdc));
		}
	}

	int vblank_fl = (chip == 0) ? (int)PCE.VBlankFL : (int)(VDC_MAXLINE(vdc) + 1);
	if (vblank_fl > 261) vblank_fl = 261;

	if (scanline == vblank_fl) {
		if (vdc->regs[CR].W & 0x08)
			*need_vbi = true;
		if (vdc->satb == DMA_TRANSFER_PENDING || (vdc->regs[DCR].W & 0x0010)) {
			memcpy(vdc->spram, vdc->vram_mem + vdc->regs[SATB].W, 512);
			vdc->satb = DMA_TRANSFER_COUNTER + 4;
		}
	}

	if (vdc->regs[CR].W & 0x04) {
		if (vdc->regs[RCR].W >= 0x40 && vdc->regs[RCR].W <= 0x146) {
			uint16_t temp_rcr = (uint16_t)(vdc->regs[RCR].W - 0x40);
			if (scanline == (temp_rcr + VDC_MINLINE(vdc)) % 263)
				gfx_irq(chip, VDC_STAT_RR);
		}
	}

	if (vdc->vram == DMA_TRANSFER_PENDING) {
		bool display_active = (scanline >= 14 && scanline <= 255)
			&& (scanline >= (int)VDC_MINLINE(vdc) && scanline <= (int)VDC_MAXLINE(vdc))
			&& (vdc->regs[CR].W & 0xC0);
		if (!display_active)
			vram_dma_run_chunk(vdc, chip);
	}

	if (vdc->satb > DMA_TRANSFER_COUNTER) {
		if (--vdc->satb == DMA_TRANSFER_COUNTER) {
			vdc->satb = 0;
			if (vdc->regs[DCR].W & 0x01)
				gfx_irq(chip, VDC_STAT_DS);
		}
	}
}

void
gfx_run(void)
{
	int scanline = PCE.Scanline;
	bool need_vbi0 = false, need_vbi1 = false;

	gfx_run_vdc_timing(&PCE.vdc[0], 0, scanline, &need_vbi0);
	if (PCE.IsSGX)
		gfx_run_vdc_timing(&PCE.vdc[1], 1, scanline, &need_vbi1);

	int32_t magical = M_vdc_HDS + (M_vdc_HDW + 1) + M_vdc_HDE;
	magical = (magical + 2) & ~1;
	magical -= M_vdc_HDW + 1;
	int32_t cyc_tot = magical * 8;
	cyc_tot -= 2;
	switch (PCE.VCE.dot_clock) {
	case 0: cyc_tot = 4 * cyc_tot / 3; break;
	case 1: break;
	case 2: cyc_tot = 2 * cyc_tot / 3; break;
	}
	if (cyc_tot < 0) cyc_tot = 0;
	h6280_run(cyc_tot);

	if (scanline >= 14 && scanline <= 255) {
		if (scanline == (int)IO_VDC_MINLINE) {
			gfx_latch_context(0, 1);
			if (PCE.IsSGX) gfx_latch_context(1, 1);
		}
		if (scanline >= (int)IO_VDC_MINLINE && scanline <= (int)IO_VDC_MAXLINE) {
			if (gfx_context[0].latched || (PCE.IsSGX && gfx_context[1].latched)) {
				if (PCE.IsSGX && !gfx_context[1].latched)
					gfx_latch_context(1, 1);
				render_lines(last_line_counter, line_counter);
				last_line_counter = line_counter;
			}
			line_counter++;
		}
	} else if (scanline == 256) {
		if (PCE.vdc[0].mode_chg) {
			PCE.vdc[0].mode_chg = 0;
			osd_gfx_set_mode(IO_VDC_SCREEN_WIDTH, IO_VDC_SCREEN_HEIGHT);
		}
		gfx_latch_context(0, 0);
		if (PCE.IsSGX) gfx_latch_context(1, 0);
		render_lines(last_line_counter, line_counter);
		if ((IO_VDC_REG[CR].W & 0x01) && sprite_hit_check(&PCE.vdc[0]))
			gfx_irq(0, VDC_STAT_CR);
		if (PCE.IsSGX && (PCE.vdc[1].regs[CR].W & 0x01) && sprite_hit_check(&PCE.vdc[1]))
			gfx_irq(1, VDC_STAT_CR);
	} else {
		gfx_context[0].latched = 0;
		gfx_context[1].latched = 0;
		last_line_counter = 0;
		line_counter = 0;
		PCE.ScrollYDiff = 0;
		PCE.vdc[0].scroll_y_diff = 0;
		PCE.vdc[1].scroll_y_diff = 0;
	}

	if (need_vbi0) PCE.vdc[0].status |= 1 << VDC_STAT_VD;
	if (need_vbi1) PCE.vdc[1].status |= 1 << VDC_STAT_VD;

	h6280_run(2);
	if ((PCE.vdc[0].status & (1 << VDC_STAT_VD)) ||
	    (PCE.IsSGX && (PCE.vdc[1].status & (1 << VDC_STAT_VD))))
		CPU_PCE.irq_lines |= INT_IRQ1;
	h6280_run(PCE.Timer.cycles_per_line - 82 - 2);
}
