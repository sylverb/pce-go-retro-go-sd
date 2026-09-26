// pce.c - Machine emulation (Memory/IO/Timer)
//
#include <stdlib.h>
#include <string.h>
#include "pce-go.h"
#include "utils.h"
#include "pce.h"
#include "gfx.h"

#ifdef PCE_ENABLE_ARCADE_CARD
#include "arcade_card.h"
#endif

#ifdef TARGET_GNW
#include "gw_malloc.h"
#else
/* Host stubs provide the same symbols via host_emu.c */
#include "gw_malloc.h"
#endif

// Global struct containing our emulated hardware status
PCE_t PCE;

// Memory Mapping
uint8_t *PageR[8];
uint8_t *PageW[8];

/* PC Engine CD-ROM2 ($1800-$180F) SCSI target — Core/Src/porting/pce/pce_scsi.c */
extern uint8_t pce_scsi_read(uint8_t reg);
extern void    pce_scsi_write(uint8_t reg, uint8_t val);

static bool running = false;

void
pce_vdc_bind(void)
{
	PCE.vdc[0].vram_mem = PCE.VRAM;
	PCE.vdc[0].spram = PCE.SPRAM;
	if (PCE.IsSGX && PCE.VRAM2) {
		PCE.vdc[1].vram_mem = PCE.VRAM2;
		PCE.vdc[1].spram = PCE.SPRAM2;
	} else {
		PCE.vdc[1].vram_mem = PCE.VRAM;
		PCE.vdc[1].spram = PCE.SPRAM2;
	}
}

void
pce_sgx_enable(void)
{
	if (PCE.IsSGX)
		return;

	PCE.IsSGX = true;

	if (!PCE.VRAM2) {
		PCE.VRAM2 = (uint16_t *)ram_malloc(0x8000 * sizeof(uint16_t));
		if (PCE.VRAM2)
			memset(PCE.VRAM2, 0, 0x8000 * sizeof(uint16_t));
		else
			MESSAGE_INFO("SGX: VRAM2 alloc failed\n");
	}
	if (!PCE.RAM_SGX) {
		PCE.RAM_SGX = (uint8_t *)ram_malloc(0x6000);
		if (PCE.RAM_SGX)
			memset(PCE.RAM_SGX, 0, 0x6000);
		else
			MESSAGE_INFO("SGX: RAM_SGX alloc failed\n");
	}

	/* Banks $F8–$FB = 32 KiB work RAM */
	PCE.MemoryMapR[0xF8] = PCE.MemoryMapW[0xF8] = PCE.RAM;
	if (PCE.RAM_SGX) {
		PCE.MemoryMapR[0xF9] = PCE.MemoryMapW[0xF9] = PCE.RAM_SGX;
		PCE.MemoryMapR[0xFA] = PCE.MemoryMapW[0xFA] = PCE.RAM_SGX + 0x2000;
		PCE.MemoryMapR[0xFB] = PCE.MemoryMapW[0xFB] = PCE.RAM_SGX + 0x4000;
	}

	PCE.vpc.priority[0] = PCE.vpc.priority[1] = 0x11;
	PCE.vpc.winwidths[0] = PCE.vpc.winwidths[1] = 0;
	PCE.vpc.st_mode = 0;

	pce_vdc_bind();
	gfx_sgx_alloc();
	MESSAGE_INFO("SuperGrafx mode enabled\n");
}

#define SGX_STATE_MAGIC 0x31584753u /* 'SGX1' */

void
pce_sgx_state_write(FILE *file)
{
	if (!file || !PCE.IsSGX || !PCE.VRAM2)
		return;
	uint32_t magic = SGX_STATE_MAGIC;
	fwrite(&magic, sizeof(magic), 1, file);
	fwrite(PCE.VRAM2, 0x8000 * sizeof(uint16_t), 1, file);
	if (PCE.RAM_SGX)
		fwrite(PCE.RAM_SGX, 0x6000, 1, file);
	else {
		static const uint8_t z[512] = {0};
		for (size_t left = 0x6000; left; ) {
			size_t n = left > sizeof(z) ? sizeof(z) : left;
			fwrite(z, 1, n, file);
			left -= n;
		}
	}
	fwrite(PCE.SPRAM2, sizeof(PCE.SPRAM2), 1, file);
	fwrite(PCE.vdc[1].regs, sizeof(PCE.vdc[1].regs), 1, file);
	fwrite(&PCE.vdc[1].reg, 1, 1, file);
	fwrite(&PCE.vdc[1].status, 1, 1, file);
	fwrite(&PCE.vdc[1].vram, 1, 1, file);
	fwrite(&PCE.vdc[1].satb, 1, 1, file);
	fwrite(&PCE.vdc[1].pending_irqs, sizeof(PCE.vdc[1].pending_irqs), 1, file);
	fwrite(&PCE.vdc[1].scroll_y_diff, sizeof(PCE.vdc[1].scroll_y_diff), 1, file);
	fwrite(&PCE.vpc, sizeof(PCE.vpc), 1, file);
}

bool
pce_sgx_state_read(FILE *file)
{
	if (!file || !PCE.IsSGX || !PCE.VRAM2)
		return false;
	uint32_t magic = 0;
	if (fread(&magic, sizeof(magic), 1, file) != 1 || magic != SGX_STATE_MAGIC)
		return false;
	fread(PCE.VRAM2, 0x8000 * sizeof(uint16_t), 1, file);
	if (PCE.RAM_SGX)
		fread(PCE.RAM_SGX, 0x6000, 1, file);
	else
		fseek(file, 0x6000, SEEK_CUR);
	fread(PCE.SPRAM2, sizeof(PCE.SPRAM2), 1, file);
	fread(PCE.vdc[1].regs, sizeof(PCE.vdc[1].regs), 1, file);
	fread(&PCE.vdc[1].reg, 1, 1, file);
	fread(&PCE.vdc[1].status, 1, 1, file);
	fread(&PCE.vdc[1].vram, 1, 1, file);
	fread(&PCE.vdc[1].satb, 1, 1, file);
	fread(&PCE.vdc[1].pending_irqs, sizeof(PCE.vdc[1].pending_irqs), 1, file);
	fread(&PCE.vdc[1].scroll_y_diff, sizeof(PCE.vdc[1].scroll_y_diff), 1, file);
	fread(&PCE.vpc, sizeof(PCE.vpc), 1, file);
	pce_vdc_bind();
	return true;
}

/**
  * Reset the hardware
  **/
void
pce_reset(bool hard)
{
    memset(&PCE.VCE, 0, sizeof(PCE.VCE));
    memset(&PCE.vdc[0], 0, sizeof(vdc_t));
    memset(&PCE.vdc[1], 0, sizeof(vdc_t));
    memset(&PCE.PSG, 0, sizeof(PCE.PSG));
    memset(&PCE.Timer, 0, sizeof(PCE.Timer));
    pce_vdc_bind();

    IO_VDC_REG[VPR].B.h=0x0f;
    IO_VDC_REG[VPR].B.l=0x02;

    IO_VDC_REG[HSR].W = IO_VDC_REG[HDR].W = IO_VDC_REG[VPR].W = IO_VDC_REG[VDW].W = IO_VDC_REG[VCR].W = 0xFF;

    if (PCE.IsSGX) {
        PCE.vdc[1].regs[VPR].B.h = 0x0f;
        PCE.vdc[1].regs[VPR].B.l = 0x02;
        PCE.vdc[1].regs[HSR].W = PCE.vdc[1].regs[HDR].W =
            PCE.vdc[1].regs[VPR].W = PCE.vdc[1].regs[VDW].W =
            PCE.vdc[1].regs[VCR].W = 0xFF;
        PCE.vpc.priority[0] = PCE.vpc.priority[1] = 0x11;
        PCE.vpc.winwidths[0] = PCE.vpc.winwidths[1] = 0;
        PCE.vpc.st_mode = 0;
    }

    if (hard) {
        memset(&PCE.RAM, 0, sizeof(PCE.RAM));
        memset(&PCE.VRAM, 0, sizeof(PCE.VRAM));
        memset(&PCE.SPRAM, 0, sizeof(PCE.SPRAM));
        memset(&PCE.SPRAM2, 0, sizeof(PCE.SPRAM2));
        memset(&PCE.Palette, 0, sizeof(PCE.Palette));
        memset(&PCE.NULLRAM, 0xFF, sizeof(PCE.NULLRAM));
        if (PCE.VRAM2)
            memset(PCE.VRAM2, 0, 0x8000 * sizeof(uint16_t));
        if (PCE.RAM_SGX)
            memset(PCE.RAM_SGX, 0, 0x6000);
        gfx_palette_reload();
    }

    PCE.SF2 = 0;
    PCE.ScrollYDiff = 0;
    PCE.Timer.cycles_counter=CYCLES_PER_TIMER_TICK;
    PCE.Timer.cycles_per_line = 113;
    Cycles = 0;

    // Reset sound generator values
    for (int i = 0; i < PSG_CHANNELS; i++) {
        PCE.PSG.chan[i].control = 0x80;
    }

    // Reset memory banking
    pce_bank_set(7, 0x00);
    pce_bank_set(6, 0x05);
    pce_bank_set(5, 0x04);
    pce_bank_set(4, 0x03);
    pce_bank_set(3, 0x02);
    pce_bank_set(2, 0x01);
    pce_bank_set(1, 0xF8);
    pce_bank_set(0, 0xFF);

    // Reset CPU_PCE
    h6280_reset();

#ifdef PCE_ENABLE_ARCADE_CARD
    pce_arcade_card_reset();
#endif
}


/**
  * Initialize the hardware
  **/
int
pce_init(void)
{
    for (int i = 0; i < 0xFF; i++) {
        PCE.MemoryMapR[i] = PCE.NULLRAM;
        PCE.MemoryMapW[i] = PCE.NULLRAM;
    }

    PCE.MemoryMapR[0xF8] = PCE.RAM;
    PCE.MemoryMapW[0xF8] = PCE.RAM;
    PCE.MemoryMapR[0xFF] = PCE.IOAREA;
    PCE.MemoryMapW[0xFF] = PCE.IOAREA;
    PCE.rp_count = 0;
    PCE.patchs = NULL;
    PCE.IsSGX = false;
    PCE.VRAM2 = NULL;
    PCE.RAM_SGX = NULL;
    pce_vdc_bind();

    // pce_reset();

    return 0;
}

/* CD-ROM2 backup RAM, empty-but-formatted image the System Card accepts as valid
 * backup memory: "HUBM" magic + LE16 0x8800 (2048-byte capacity) + LE16 0x8010
 * (first free byte). Byte-identical to Mednafen huc.cpp BRAM_Init_String. */
static const uint8_t PCE_BRAM_MAGIC[8] = { 0x48, 0x55, 0x42, 0x4D, 0x00, 0x88, 0x10, 0x80 };

/* Map BRAM into bank $F7 (read+write, no hardware lock — the System Card always
 * unlocks before writing, so honouring the lock could only remove a write-protect,
 * never block a legit save). Fill the non-mirrored 0x800-0x1FFF tail with 0xFF.
 * Call once from the CD load path, after pce_init(), before reset/run. */
void pce_bram_init(void)
{
    PCE.MemoryMapR[0xF7] = PCE.bram;
    PCE.MemoryMapW[0xF7] = PCE.bram;
    memset(PCE.bram + 0x800, 0xFF, 0x2000 - 0x800);
}

/* If the first 8 bytes are not the HUBM signature (fresh boot, or a missing/corrupt
 * .bram file), write a valid empty-formatted cabinet so the System Card does not show
 * "backup memory not initialized". Touches only the low 2KB. */
void pce_bram_format_if_needed(void)
{
    if (memcmp(PCE.bram, PCE_BRAM_MAGIC, 8) != 0) {
        memset(PCE.bram, 0x00, 0x800);
        memcpy(PCE.bram, PCE_BRAM_MAGIC, 8);
    }
}


/**
  * Terminate the emulation loop
  **/
void
pce_pause(void)
{
    running = false;
}


/**
  * Terminate the hardware
  **/
void
pce_term(void)
{
    if (PCE.ExRAM) free(PCE.ExRAM);
    if (PCE.ROM) free(PCE.ROM);
}


/**
  * Main emulation loop
  **/
void
pce_run(void)
{
    running = true;

    while (running) {
        osd_input_read(PCE.Joypad.regs);

        for (PCE.Scanline = 0; PCE.Scanline < 263; ++PCE.Scanline) {
            gfx_run();
        }

        osd_gfx_blit();
        osd_vsync();

        // Prevent Overflowing
        PCE.Timer.cycles_counter -= Cycles;
        PCE.MaxCycles -= Cycles;
        Cycles = 0;
    }
}


static inline void
cart_write(uint16_t A, uint8_t V)
{
    TRACE_IO("Cart Write %02x at %04x\n", V, A);

    // SF2 Mapper
    if (A >= 0xFFF0 && PCE.ROM_SIZE >= 0xC0)
    {
        if (PCE.SF2 != (A & 3))
        {
            PCE.SF2 = A & 3;
            uint8_t *base = PCE.ROM_DATA + PCE.SF2 * (512 * 1024);
            for (int i = 0x40; i < 0x80; i++)
            {
                PCE.MemoryMapR[i] = base + i * 0x2000;
            }
            for (int i = 0; i < 8; i++)
            {
                if (PCE.MMR[i] >= 0x40 && PCE.MMR[i] < 0x80)
                    pce_bank_set(i, PCE.MMR[i]);
            }
        }
    }
}


/* ---- HuC6270 register access (one chip) ---- */

static uint8_t
vdc_read_port(vdc_t *vdc, unsigned A)
{
	uint8_t ret = 0;
	uint16_t *vram = vdc->vram_mem ? vdc->vram_mem : PCE.VRAM;

	switch (A & 3) {
	case 0:
		ret = vdc->status;
		vdc->status = 0;
		if (PCE.IsSGX) {
			if (!(PCE.vdc[0].status & 0x3F) && !(PCE.vdc[1].status & 0x3F))
				CPU_PCE.irq_lines &= ~INT_IRQ1;
		} else {
			CPU_PCE.irq_lines &= ~INT_IRQ1;
		}
		break;
	case 1:
		ret = 0;
		if (PCE.VCE.dot_clock > 0)
			ret = 0x40;
		break;
	case 2:
		if (vdc->reg == VRR)
			ret = vram[vdc->regs[MARR].W & 0x7FFF] & 0xFF;
		else
			ret = vdc->regs[vdc->reg].B.l;
		break;
	case 3:
		if (vdc->reg == VRR) {
			ret = vram[vdc->regs[MARR].W & 0x7FFF] >> 8;
			VDC_REG_INC(vdc, MARR);
			PCE.io_buffer = vram[vdc->regs[MARR].W & 0x7FFF];
		} else {
			ret = vdc->regs[vdc->reg].B.h;
		}
		break;
	}
	return ret;
}

static void
vdc_write_port(vdc_t *vdc, int chip, unsigned A, uint8_t V)
{
	uint16_t *vram = vdc->vram_mem ? vdc->vram_mem : PCE.VRAM;

	switch (A & 3) {
	case 0:
		vdc->reg = V & 31;
		return;
	case 1:
		return;
	case 2: /* LSB */
		switch (vdc->reg & 31) {
		case CR:
			if (vdc->regs[vdc->reg].B.l != V)
				gfx_latch_context(chip, 0);
			break;
		case BXR:
			if (vdc->regs[vdc->reg].B.l != V)
				gfx_latch_context(chip, 0);
			break;
		case BYR:
			gfx_latch_context(chip, 0);
			vdc->scroll_y_diff = (int)PCE.Scanline - 1 - (int)VDC_MINLINE(vdc);
			if (vdc->scroll_y_diff < 0)
				vdc->scroll_y_diff = 0;
			if (chip == 0)
				PCE.ScrollYDiff = vdc->scroll_y_diff;
			break;
		case HSR:
			V = 0x1F;
			vdc->mode_chg = 1;
			break;
		case HDR:
			V &= 0x7F;
			if ((V + 1) * 8 != (int)VDC_SCREEN_WIDTH(vdc))
				vdc->mode_chg = 1;
			break;
		case VPR:
			V &= 0x1F;
			vdc->mode_chg = 1;
			break;
		case VDW:
		case VCR:
			vdc->mode_chg = 1;
			break;
		default:
			break;
		}
		vdc->regs[vdc->reg].B.l = V;
		return;

	case 3: /* MSB */
		switch (vdc->reg & 31) {
		case VWR:
			if (vdc->regs[MAWR].W < 0x8000) {
				if (vdc->vram == DMA_TRANSFER_PENDING) {
					int src_inc = (vdc->regs[DCR].W & 8) ? -1 : 1;
					int dst_inc = (vdc->regs[DCR].W & 4) ? -1 : 1;
					while (vdc->regs[LENR].W != 0xFFFF) {
						if (vdc->regs[DISTR].W < 0x8000)
							vram[vdc->regs[DISTR].W] = vram[vdc->regs[SOUR].W];
						vdc->regs[SOUR].W += src_inc;
						vdc->regs[DISTR].W += dst_inc;
						vdc->regs[LENR].W -= 1;
					}
					vdc->vram = 0;
					if (vdc->regs[DCR].W & 0x02)
						gfx_irq(chip, VDC_STAT_DV);
				}
				vram[vdc->regs[MAWR].W] = (V << 8) | vdc->regs[vdc->reg].B.l;
			}
			VDC_REG_INC(vdc, MAWR);
			break;
		case CR:
			if (vdc->regs[vdc->reg].B.h != V)
				gfx_latch_context(chip, 0);
			break;
		case RCR:
			V &= 0x3;
			break;
		case BXR:
			V &= 0x3;
			if (vdc->regs[vdc->reg].B.h != V)
				gfx_latch_context(chip, 0);
			break;
		case BYR:
			gfx_latch_context(chip, 0);
			V &= 0x1;
			vdc->scroll_y_diff = (int)PCE.Scanline - 1 - (int)VDC_MINLINE(vdc);
			if (vdc->scroll_y_diff < 0)
				vdc->scroll_y_diff = 0;
			if (chip == 0)
				PCE.ScrollYDiff = vdc->scroll_y_diff;
			break;
		case HSR:
			V &= 0x7F;
			vdc->mode_chg = 1;
			break;
		case HDR:
			V &= 0x7F;
			break;
		case VPR:
			V &= 0x7F;
			vdc->mode_chg = 1;
			break;
		case VDW:
			V &= 0x1;
			vdc->mode_chg = 1;
			break;
		case VCR:
			vdc->mode_chg = 1;
			break;
		case LENR:
			vdc->regs[LENR].B.h = V;
			vdc->vram = DMA_TRANSFER_PENDING;
			return;
		case SATB:
			vdc->satb = DMA_TRANSFER_PENDING;
			break;
		default:
			break;
		}
		vdc->regs[vdc->reg].B.h = V;
		return;
	}
}

static uint8_t
vdc_space_read(uint16_t A)
{
	if (!PCE.IsSGX)
		return vdc_read_port(&PCE.vdc[0], A);

	A &= 0x1F;
	switch (A) {
	case 0x8: return PCE.vpc.priority[0];
	case 0x9: return PCE.vpc.priority[1];
	case 0xA: return (uint8_t)PCE.vpc.winwidths[0];
	case 0xB: return (uint8_t)(PCE.vpc.winwidths[0] >> 8);
	case 0xC: return (uint8_t)PCE.vpc.winwidths[1];
	case 0xD: return (uint8_t)(PCE.vpc.winwidths[1] >> 8);
	case 0xE: return 0;
	default:
		break;
	}
	if (A & 0x8)
		return 0;
	return vdc_read_port(&PCE.vdc[(A & 0x10) >> 4], A);
}

static void
vdc_space_write(uint16_t A, uint8_t V)
{
	if (!PCE.IsSGX) {
		vdc_write_port(&PCE.vdc[0], 0, A, V);
		return;
	}

	A &= 0x1F;
	switch (A) {
	case 0x8: PCE.vpc.priority[0] = V; return;
	case 0x9: PCE.vpc.priority[1] = V; return;
	case 0xA:
		PCE.vpc.winwidths[0] = (PCE.vpc.winwidths[0] & 0x300) | V;
		return;
	case 0xB:
		PCE.vpc.winwidths[0] = (PCE.vpc.winwidths[0] & 0x0FF) | ((V & 3) << 8);
		return;
	case 0xC:
		PCE.vpc.winwidths[1] = (PCE.vpc.winwidths[1] & 0x300) | V;
		return;
	case 0xD:
		PCE.vpc.winwidths[1] = (PCE.vpc.winwidths[1] & 0x0FF) | ((V & 3) << 8);
		return;
	case 0xE:
		PCE.vpc.st_mode = V & 1;
		return;
	default:
		break;
	}
	if (A & 0x8)
		return;
	vdc_write_port(&PCE.vdc[(A & 0x10) >> 4], (A & 0x10) >> 4, A, V);
}


inline uint8_t
pce_readIO(uint16_t A)
{
    uint8_t ret = 0xFF; // Open Bus

#ifdef PCE_ENABLE_ARCADE_CARD
    if ((A & 0x1E00) == 0x1A00)
        return pce_arcade_card_read(A);
#endif

    // The last read value in 0800-017FF is read from the io buffer
    if (A >= 0x800 && A < 0x1800)
        ret = PCE.io_buffer;

    switch (A & 0x1F00) {
    case 0x0000:                /* VDC / VPC / VDC2 (SGX via A&0x1F) */
        ret = vdc_space_read(A);
        break;

    case 0x0400:                /* VCE */
        switch (A & 7) {
        case 0: ret = 0xFF; break; // Write only
        case 1: ret = 0xFF; break; // Unused
        case 2: ret = 0xFF; break; // Write only
        case 3: ret = 0xFF; break; // Write only
        case 4: ret = PCE.VCE.regs[PCE.VCE.reg.W].B.l; break; // Color LSB (8 bit)
        case 5: {            
                //printf("READ VCE Color MSB\n");
                ret = (PCE.VCE.regs[PCE.VCE.reg.W].B.h & 1) | 0xFE; // Color MSB (1 bit)
                PCE.VCE.reg.W++;
                PCE.VCE.reg.W &= 0x1FF;
                break; 
        }
        case 6: ret = 0xFF; break; // Unused
        }
        break;

    case 0x0800:                /* PSG */
        switch (A & 15) {
        case 0: ret = PCE.PSG.ch; break;
        case 1: ret = PCE.PSG.volume; break;
        case 2: ret = PCE.PSG.chan[PCE.PSG.ch].freq_lsb; break;
        case 3: ret = PCE.PSG.chan[PCE.PSG.ch].freq_msb; break;
        case 4: ret = PCE.PSG.chan[PCE.PSG.ch].control; break;
        case 5: ret = PCE.PSG.chan[PCE.PSG.ch].balance; break;
        case 6: ret = PCE.PSG.chan[PCE.PSG.ch].wave_index; break;
        case 7: ret = PCE.PSG.chan[PCE.PSG.ch].noise_ctrl; break;
        case 8: ret = PCE.PSG.lfo_freq; break;
        case 9: ret = PCE.PSG.lfo_ctrl; break;
        }
        break;

    case 0x0C00:                /* Timer */
        {
            uint8_t tmp = PCE.Timer.counter;
            if(PCE.Timer.cycles_counter == Cycles)
                tmp = (tmp - 1) & 0x7F;
            ret = (tmp | (PCE.io_buffer & 0x80)); 
        }
        break;

    case 0x1000:                /* Joypad */
        ret = PCE.Joypad.regs[PCE.Joypad.counter] ^ 0xff;
        if (PCE.Joypad.nibble & 1)
            ret >>= 4;
        else {
            ret &= 15;
            PCE.Joypad.counter = ((PCE.Joypad.counter + 1) % 5);
        }
        ret |= 0x30; // those 2 bits are always on, bit 6 = 0 (Jap), bit 7 = 0 (Attached cd)
        break;

    case 0x1400:                /* IRQ */
        switch (A & 3) {
        case 0:    
        case 1:
            ret = PCE.io_buffer;
            break;
        case 2:
            ret = CPU_PCE.irq_mask | (PCE.io_buffer & ~INT_MASK);
            break;
        case 3:
            ret = (CPU_PCE.irq_lines & INT_MASK) | (PCE.io_buffer & ~INT_MASK);
            //CPU_PCE.irq_lines &= ~INT_TIMER;
            break;
        }
        break;

    case 0x1A00:                // Arcade Card
#ifndef PCE_ENABLE_ARCADE_CARD
        MESSAGE_INFO("Arcade Card not supported : 0x%04X\n", A);
#endif
        break;

    case 0x1800:                // CD-ROM2 / Super System Card
    case 0x18C0:
        /* $18C0-$18C7 = Super CD identification (EX_MEMOPEN). BIOS checks $18C5/$18C6
         * first for a Duo ($18C7=version); else $18C1/$18C2 for PCE+System Card.
         * Report Duo (Mednafen 2019-08-24) for US Super CD-ROM2 compatibility. */
        if ((A & 0xF8) == 0xC0) {
            switch (A & 0x07) {
            case 1: ret = 0xAA; break;
            case 2: ret = 0x55; break;
            case 3: ret = 0x00; break;
            case 5: ret = 0xAA; break;
            case 6: ret = 0x55; break;
            case 7: ret = 0x03; break;
            default: ret = 0x00; break;
            }
        } else {
            ret = pce_scsi_read(A & 0x0F);
        }
        break;
    }

    TRACE_IO("IO Read %02x at %04x\n", ret, A);

    // The last read value in 0800-017FF is saved in the io buffer
    if (A >= 0x800 && A < 0x1800)
        PCE.io_buffer = ret;

    return ret;
}


inline void
pce_writeIO(uint16_t A, uint8_t V)
{
    TRACE_IO("IO Write %02x at %04x\n", V, A);

#ifdef PCE_ENABLE_ARCADE_CARD
    if ((A & 0x1E00) == 0x1A00) {
        pce_arcade_card_write(A, V);
        return;
    }
#endif

    // The last write value in 0800-017FF is saved in the io buffer
    if (A >= 0x800 && A < 0x1800)
        PCE.io_buffer = V;

    switch (A & 0x1F00) {
    case 0x0000:                /* VDC / VPC / VDC2 */
        vdc_space_write(A, V);
        return;

    case 0x0400:                /* VCE */
        switch (A & 7) {
        case 0:                                 // VCE control
            PCE.VCE.CR = V;
            PCE.VCE.dot_clock = V & 1;
            if(V & 2)
               PCE.VCE.dot_clock = 2;
            return;

        case 1:                                 // Not used
            return;

        case 2:                                 // Color table address (LSB)
            PCE.VCE.reg.B.l = V;
            return;

        case 3:                                 // Color table address (MSB)
            PCE.VCE.reg.B.h = V & 1;
            return;

        case 4:                                 // Color table data (LSB)
            PCE.VCE.regs[PCE.VCE.reg.W].B.l = V;
            {
                uint16_t n = PCE.VCE.reg.W;
                uint8_t c = PCE.VCE.regs[n].W >> 1;
                gfx_palette_write(n, c);
            }
            return;

        case 5:                                 // Color table data (MSB)
            PCE.VCE.regs[PCE.VCE.reg.W].B.h = V;
            {
                uint16_t n = PCE.VCE.reg.W;
                uint8_t c = PCE.VCE.regs[n].W >> 1;
                gfx_palette_write(n, c);
            }
            PCE.VCE.reg.W = (PCE.VCE.reg.W + 1) & 0x1FF;
            return;

        case 6:                                 // Not used
            return;

        case 7:                                 // Not used
            return;
        }
        break;

    case 0x0800:                /* PSG */
        switch (A & 15) {
        case 0:                                 // Select PSG channel
            PCE.PSG.ch = MIN(V & 7, 5);
            return;

        case 1:                                 // Select global volume
            PCE.PSG.volume = V;
            return;

        case 2:                                 // Frequency setting, 8 lower bits
            PCE.PSG.chan[PCE.PSG.ch].freq_lsb = V;
            return;

        case 3:                                 // Frequency setting, 4 upper bits
            PCE.PSG.chan[PCE.PSG.ch].freq_msb = V & 0xF;
            return;

        case 4:
            if ((V & 0xC0) == (PSG_DDA_ENABLE)) {
                PCE.PSG.chan[PCE.PSG.ch].wave_index = 0; // Reset wave index pointer
            }

            PCE.PSG.chan[PCE.PSG.ch].control = V;
            return;

        case 5:                                 // Set channel specific volume
            PCE.PSG.chan[PCE.PSG.ch].balance = V;
            return;

        case 6:                                 // Put a value into the waveform or direct audio buffers
            switch (PCE.PSG.chan[PCE.PSG.ch].control & 0xC0)
            {
            case 0: // Write to the wave buffer and increment the counter
                PCE.PSG.chan[PCE.PSG.ch].wave_data[PCE.PSG.chan[PCE.PSG.ch].wave_index] = V & 0x1F;
                PCE.PSG.chan[PCE.PSG.ch].wave_index++; // Inc pointer
                PCE.PSG.chan[PCE.PSG.ch].wave_index &= 0x1F; // Wrap at 32
                break;
            case PSG_CHAN_ENABLE|PSG_DDA_ENABLE: // Update DDA sample
                PCE.PSG.chan[PCE.PSG.ch].dda_data[PCE.PSG.chan[PCE.PSG.ch].dda_index] = V & 0x1F;
                PCE.PSG.chan[PCE.PSG.ch].dda_count = MIN(PCE.PSG.chan[PCE.PSG.ch].dda_count+1, 0x100);
                PCE.PSG.chan[PCE.PSG.ch].dda_index = (PCE.PSG.chan[PCE.PSG.ch].dda_index+1) & 0xFF;
                break;
            }
            return;

        case 7:
            PCE.PSG.chan[PCE.PSG.ch].noise_ctrl = V;
            return;

        case 8:
            PCE.PSG.lfo_freq = V;
            return;

        case 9:
            PCE.PSG.lfo_ctrl = V;
            return;
        }
        break;

    case 0x0C00:                /* Timer */
        switch (A & 1) {
        case 0:
            PCE.Timer.reload = (V & 0x7F); // + 1;
            return;
        case 1:
            V &= 1;
            if (V && !PCE.Timer.running){
                PCE.Timer.cycles_counter = Cycles + CYCLES_PER_TIMER_TICK;
                PCE.Timer.counter = PCE.Timer.reload;
            }
            PCE.Timer.running = V;
            return;
        }
        break;

    case 0x1000:                /* Joypad */
        PCE.Joypad.nibble = V & 1;
        if (V & 2)
            PCE.Joypad.counter = 0;
        return;

    case 0x1400:                /* IRQ */
        switch (A & 3) {
        case 2:
            CPU_PCE.irq_mask = V & INT_MASK;
            return;
        case 3:
            CPU_PCE.irq_lines &= ~INT_TIMER;
            return;
        }
        break;

    case 0x1A00:                /* Arcade Card */
#ifndef PCE_ENABLE_ARCADE_CARD
        MESSAGE_INFO("Arcade Card not supported : %d into 0x%04X\n", V, A);
#endif
        return;

    case 0x1800:                /* CD-ROM2 / Super System Card */
        /* $18C0-$18C7 = Super System Card id/unlock block (BIOS writes $AA/$55 to
         * $18C0 after a successful EX_MEMOPEN). Keep it OUT of the SCSI block so a
         * write here can't fire a spurious SCSI SEL/ACK. */
        if ((A & 0xF8) == 0xC0)
            return;
        pce_scsi_write(A & 0x0F, V);
        return;

    case 0x1F00:                /* Street Fighter 2 Mapper */
        cart_write(A, V);
        return;
    }

    MESSAGE_DEBUG("ignored I/O write: %04x,%02x at PC = %04X\n", A, V, CPU_PCE.PC);
}
