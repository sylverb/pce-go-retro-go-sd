# Changelog

## [v0.0.3]

### Added

- PC Engine CD audio tracks as **OGG Vorbis** (`FILE "track.ogg" OGG` in the cue sheet).
- **CHD** disc images via libchdr (map sidecar on SD, mirrored to QSPI flash at play).

### Changed

- Switched to LUT8 display instead of RGB565 to free some additional ram.

### Fixed

- Nothing.

### Install

**Core**

- Copy `pce.bin` **and** `pce.xip` to `/cores/` on the SD card.
- HuCard ROMs: `/roms/pce/*.pce`
- SuperGrafx: `/roms/sgx/*.sgx` or `*.pce`
- CD-ROM² cue: `/roms/pcecd/<game>/<game>.cue` (+ `.bin`/`.ogg`)
- CD-ROM² CHD: `/roms/pcecd/<game>/<game>.chd`
- System Card (CD): `/bios/pce/syscard3.pce` or `syscard3.bin`

The release archive contains the ready-to-copy SD layout
(`cores/pce.bin` + `cores/pce.xip`).
