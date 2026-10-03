# Changelog

## [v0.0.2]

### Added

- SuperGrafx HuCard support (`.sgx`): dual HuC6270 + VPC mix, 32 KiB RAM, VRAM2; hot `spr_buf`/linebufs in DTCM

### Changed

- Nothing

### Fixed

- Nothing

### Install

**Core**

- Copy `pce.bin` to `/cores/` on the SD card.
- HuCard ROMs: `/roms/pce/*.pce`
- SuperGrafx: `/roms/sgx/*.sgx` or `*.pce` (CRC auto-detect also works under `/roms/pce/`)
- CD-ROM²: `/roms/pcecd/<game>/<game>.cue` (+ sibling `.bin` tracks)
- System Card (CD): `/bios/pce/syscard3.pce` or `syscard3.bin`

The release archive contains the ready-to-copy SD layout (`cores/pce.bin`).
