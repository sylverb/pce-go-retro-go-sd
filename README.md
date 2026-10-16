# PC Engine / PC Engine CD — Retro-Go SD core

Standalone dynamic core for **PC Engine (HuCard)** and **PC Engine CD-ROM²**,
ported from
[game-and-watch-retro-go-sd `main`](https://github.com/sylverb/game-and-watch-retro-go-sd/tree/main/Core/Src/porting/pce)
onto this template (ABI `gw_firmware_abi_t`). One packed binary, two launcher tabs.

| | |
|--|--|
| Packer | `sdk/tools/pack_core.py` (`CORE`) |
| SD path | `/cores/pce.bin` |
| HuCard | dirname `pce`, ext `.pce`, cheats `pceplus` |
| CD | dirname `pcecd`, ext `.cue`, parse=cdrom |
| BIOS (CD) | `/bios/pce/syscard3.pce` or `.bin` |

Sources: `src/pce-go/` (HuExpress-GO submodule pin from firmware) +
`src/porting/` + `src/main_pce.c` from firmware `Core/Src/porting/pce/`.

## Build

```bash
make          # → pce.bin
make host     # → ./pce_host
make docker
```

Copy `pce.bin` to `/cores/` on the SD card. HuCard ROMs under `/roms/pce/`,
CD under `/roms/pcecd/<game>/<game>.cue` (+ track files).

### PC Engine CD audio tracks

Data tracks stay raw `.bin` (MODE1/2352 or /2048). Audio tracks may be:

| Cue `FILE` type | Format |
|-----------------|--------|
| `BINARY` (default) | Red Book PCM, 2352 B/sector |
| `OGG` | Vorbis, decoded on the fly to Red Book sectors |

Example:

```cue
FILE "Game (Track 01).bin" BINARY
  TRACK 01 MODE1/2352
    INDEX 01 00:00:00
FILE "Game (Track 02).ogg" OGG
  TRACK 02 AUDIO
    INDEX 01 00:00:00
```

OGG should be **44.1 kHz** (mono or stereo). A decode arena (~168–176 KiB,
codebook-dependent) is taken from RAM_EMU before CD-RAM is mapped; ADPCM RAM
lives in DTCM so the bump still covers Super CD’s 256 KiB.

Host CD preview needs the System Card mirrored like the SD layout:

```bash
export HOST_SD=/path/to/sd-root   # contains bios/pce/syscard3.pce
./pce_host /path/to/game.cue
```

## Memory

- Hot `.text` in **ITCM** (gfx, h6280, pce, sound, cd, scsi, adpcm) — see `ld/pce_core.ld`
- ADPCM 64 KiB prefers **DTCM**; CD-RAM prefers `ram_malloc` then EXRAM/save_buffer
- AHB only as last resort (scarcer than on older firmwares)
