#pragma once

#include <stdbool.h>
#include <stdint.h>

int gfx_init(void);
void gfx_run(void);
void gfx_term(void);
void gfx_irq(int chip, int type);
void gfx_reset(bool hard);
void gfx_latch_context(int chip, int force);
void gfx_sgx_alloc(void);
/* Keep DTCM palette mirror in sync with PCE.Palette (VCE write / loadstate). */
void gfx_palette_write(uint16_t n, uint8_t c);
void gfx_palette_reload(void);
