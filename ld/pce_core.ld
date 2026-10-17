/*
 * PC Engine / PC Engine CD core linker script.
 *
 *   RAM_EMU  — entry + main_pce (whole) + hot text/data/bss
 *   ITCM     — hot CODE: gfx, h6280, pce, sound, cd, scsi, adpcm
 *   PCE_CODE — XIP sidecar (pce.xip @ sentinel 0xDEC00000):
 *              cold text (OGG, LZMA encoder helpers) + leftover .rodata
 *
 * CHD decompress (flac/lzma/zlib) stays in RAM_EMU — CD-DA is hot.
 * main_pce.o owns PCE_CODE_BASE and the sentinel patch pass; it must not
 * be walked by that pass (see src/main_pce.c).
 *
 * Object paths use *basename.o so BUILD_DIR nesting resolves.
 */

INCLUDE gnw_ram_emu.ld
INCLUDE gnw_itcm_core.ld

MEMORY
{
  RAM_EMU  (xrw) : ORIGIN = __RAM_EMU_START__,  LENGTH = __RAM_EMU_LENGTH__
  ITCM     (xrw) : ORIGIN = __ITCM_CORE_START__, LENGTH = __ITCM_CORE_LENGTH__
  /* Sentinel only — payload ships as pce.xip, not packed into the CORE bin. */
  PCE_CODE (x)   : ORIGIN = 0xDEC00000,         LENGTH = 512K
}

SECTIONS
{
  /* ------------------------------------------------------------------ */
  /* Segment 0: RAM_EMU                                                 */
  /* ------------------------------------------------------------------ */
  .core_entry __RAM_EMU_START__ :
  {
    KEEP(*(.core_entry))
    KEEP(*(.gw_core_bridge_probe))
    . = ALIGN(4);

    /* main_pce.o whole — holds PCE_CODE_BASE; patch range starts after it. */
    _PCE_MAIN_CODE_START = .;
    *main_pce.o (.data .data* .text .text* .rodata .rodata*)
    . = ALIGN(4);
    _PCE_MAIN_CODE_END = .;

    /* Remaining .data (main_pce already claimed). */
    *(.data .data*)

    /* Hot .text: everything except ITCM hot + XIP cold. */
    EXCLUDE_FILE(
      *gfx.o
      *h6280.o
      *pce.o
      *sound_pce.o
      *pce_cd.o
      *pce_scsi.o
      *pce_adpcm.o
      *pce_cd_ogg.o
      *LzmaEnc.o
      *LzFind.o
      *Sort.o
      *Alloc.o
    ) *(.text .text*)

    /* ITCM objects are code-only; keep their rodata in RAM_EMU. */
    *gfx.o (.rodata .rodata*)
    *h6280.o (.rodata .rodata*)
    *pce.o (.rodata .rodata*)
    *sound_pce.o (.rodata .rodata*)
    *pce_cd.o (.rodata .rodata*)
    *pce_scsi.o (.rodata .rodata*)
    *pce_adpcm.o (.rodata .rodata*)

    /* Hot CHD codecs: keep their rodata in RAM (table lookups every hunk). */
    *libchdr_flac.o (.rodata .rodata*)
    *libchdr_chd.o (.rodata .rodata*)
    *LzmaDec.o (.rodata .rodata*)
    *inflate.o (.rodata .rodata*)
    *inffast.o (.rodata .rodata*)
    *inftrees.o (.rodata .rodata*)
    *zutil.o (.rodata .rodata*)
    *adler32.o (.rodata .rodata*)
    *crc32.o (.rodata .rodata*)

    *(.glue_7)
    *(.glue_7t)

    . = ALIGN(4);
    __CORE_INIT_ARRAY_START__ = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    __CORE_INIT_ARRAY_END__ = .;

    . = ALIGN(8);
  } >RAM_EMU

  .data :
  {
    *(.data*)
  } >RAM_EMU

  __CORE_CODE_END__ = .;
  _OVERLAY_PCE_BSS_START = __CORE_CODE_END__;
  _PCE_SENTINEL_PATCH_END = __CORE_CODE_END__;

  .bss _OVERLAY_PCE_BSS_START (NOLOAD) :
  {
    . = ALIGN(8);
    *(.bss*)
    *(COMMON)
  } >RAM_EMU

  __CORE_BSS_END__ = .;

  ASSERT(__CORE_BSS_END__ <= __RAM_EMU_END__,
         "Error: PCE core RAM_EMU segment (code+data+bss) overflows RAM_EMU")

  /* ------------------------------------------------------------------ */
  /* ITCM: hot CODE only                                                */
  /* ------------------------------------------------------------------ */
  .core_itcm __ITCM_CORE_START__ :
  {
    . = ALIGN(4);
    __CORE_ITCM_CODE_START__ = .;
    *gfx.o(.text*)
    *h6280.o(.text*)
    *pce.o(.text*)
    *sound_pce.o(.text*)
    *pce_cd.o(.text*)
    *pce_scsi.o(.text*)
    *pce_adpcm.o(.text*)
    . = ALIGN(4);
    __CORE_ITCM_CODE_END__ = .;
  } >ITCM

  __CORE_ITCM_BSS_END__ = __CORE_ITCM_CODE_END__;

  ASSERT(__CORE_ITCM_BSS_END__ <= __ITCM_CORE_START__ + __ITCM_CORE_LENGTH__,
         "Error: PCE core ITCM segment (code) overflows the 64K ITCM region")

  /* ------------------------------------------------------------------ */
  /* XIP sidecar (NOT a pack segment) → pce.xip                         */
  /* ------------------------------------------------------------------ */
  .xip_pce :
  {
    . = ALIGN(8);
    __xip_pce_start__ = .;
    /* OGG/Vorbis — unused during CHD; OK from QSPI when playing cue. */
    *pce_cd_ogg.o (.text .text*)
    /* LZMA encoder stack — CHD only needs LzmaDec. */
    *LzmaEnc.o (.text .text*)
    *LzFind.o (.text .text*)
    *Sort.o (.text .text*)
    *Alloc.o (.text .text*)
  } >PCE_CODE

  .rodata_pce :
  {
    . = ALIGN(8);
    __rodata_pce_start__ = .;
    /* Everything not claimed above (libm tables, cold consts, …). */
    *(.rodata .rodata*)
    . = ALIGN(4);
    __rodata_pce_end__ = .;
    __xip_pce_end__ = .;
  } >PCE_CODE

  ASSERT(__xip_pce_end__ <= ORIGIN(PCE_CODE) + LENGTH(PCE_CODE),
         "Error: PCE XIP (.xip_pce + .rodata_pce) overflows 512K PCE_CODE")

  __exidx_start = .;
  __exidx_end = .;

  /DISCARD/ :
  {
    *(.fini_array*)
    *(.preinit_array*)
    *(.ARM.exidx*)
    *(.ARM.extab*)
    *(.comment)
    *(.ARM.attributes)
  }
}
