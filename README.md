# PC Engine / PC Engine CD — Retro-Go SD core

Standalone dynamic core for **PC Engine (HuCard)**, **SuperGrafx**, and
**PC Engine CD-ROM²** (cue/OGG and CHD), ported from
[game-and-watch-retro-go-sd `main`](https://github.com/sylverb/game-and-watch-retro-go-sd/tree/main/Core/Src/porting/pce)
onto this template (ABI `gw_firmware_abi_t`). One packed binary.

| | |
|--|--|
| Packer | `sdk/tools/pack_core.py` (`CORE`) |
| SD path | `/cores/pce.bin` + `/cores/pce.xip` |
| HuCard | dirname `pce`, ext `.pce`, cheats `pceplus` |
| SuperGrafx | dirname `sgx`, ext `.sgx` / `.pce` |
| CD cue | dirname `pcecd`, ext `.cue`, parse=cdrom |
| CD CHD | dirname `pcecd`, ext `.chd`, parse=rom |
| BIOS (CD) | `/bios/pce/syscard3.pce` or `.bin` |

Sources: `src/pce-go/` (HuExpress-GO submodule pin from firmware) +
`src/porting/` + `src/main_pce.c` from firmware `Core/Src/porting/pce/`.

## Build

```bash
make          # → pce.bin + pce.xip
make host     # → ./pce_host
make docker
```

Copy **both** `pce.bin` and `pce.xip` to `/cores/` on the SD card. The `.xip`
sidecar holds cold OGG/LZMA-encoder code and leftover `.rodata` (QSPI XIP,
same idea as `gba.xip`) so RAM_EMU keeps room for CHD heap + Super CD-RAM.

| Launcher tab | ROM folder / extensions |
|--------------|-------------------------|
| PC Engine | `/roms/pce/*.pce` |
| SuperGrafx | `/roms/sgx/*` |
| PC Engine CD | `/roms/pcecd/**/*.cue` (+ `.bin` / `.ogg`) |
| PC Engine CD CHD | `/roms/pcecd/**/*.chd` (+ optional `.map` sidecar) |

Cue uses `parse=cdrom` (firmware collapses folders to a `.cue` row). CHD uses
`parse=rom` + `ext=chd` — `parse=cdrom` ignores extensions and always looks
for `.cue`. Both tabs share `/roms/pcecd/`.

Disc backends are linked together but **runtime heaps never coexist**: a `.chd`
opens libchdr (240 KiB heap + LUT8 bonus for CD-RAM); a `.cue` uses the OGG
arena only.

### PC Engine CD audio tracks (cue)

Data tracks stay raw `.bin` (MODE1/2352 or /2048). Audio tracks may be:

| Cue `FILE` type | Format |
|-----------------|--------|
| `BINARY` (default) | Red Book PCM, 2352 B/sector |
| `OGG` | Vorbis, decoded on the fly |

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

### CHD discs

Place `.chd` under `/roms/pcecd/<game>/` (same layout as cue games).

On first open the core expands the V5 hunk map to a sidecar `<game>.chd.map`
(~12 B × hunks — Dracula ~330 KiB, Ys ~490 KiB on SD), then mirrors it into
QSPI flash for play. Later launches reuse the sidecar. **Copy the `.map` next
to the `.chd`** when possible. Codec scratch is a dedicated RAM_EMU heap (not
AHB).

Host CD preview needs the System Card mirrored like the SD layout:

```bash
export HOST_SD=/path/to/sd-root   # contains bios/pce/syscard3.pce
./pce_host /path/to/game.cue
# or
./pce_host /path/to/game.chd
```

## Memory

- Hot `.text` in **ITCM** (gfx, h6280, pce, sound, cd, scsi, adpcm) — see `ld/pce_core.ld`
- ADPCM 64 KiB prefers **DTCM**; CD-RAM prefers `ram_malloc` then EXRAM/save_buffer
- AHB only as last resort (scarcer than on older firmwares)
- CHD heap 240 KiB + LUT8 bonus ~150 KiB for Super CD-RAM when playing `.chd`
