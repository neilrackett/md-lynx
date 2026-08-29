# AGENTS.md — md-lynx

This file is the playbook for coding agents working in this repository. It is
the single source of truth; `CLAUDE.md` is a one-line `@AGENTS.md` import, so
Claude Code loads this file too. Edit this file, not `CLAUDE.md`.

MD/Lynx is built on the **md-framebuffer-template**; most of what follows
describes that platform, and the app-specific parts are under "Atari Lynx
emulator" below.

See also: `README.md` (the user-facing guide: controls, installation, how it works), `CHANGELOG.md` (release history), `programming.md` (the **upstream** `md-microfirmware-template` reference — useful for background, but its shared-region table describes the upstream layout, *not* this template's: the table below and `cart_shared.h` are authoritative here), and the private `docs/epics/` folder (internal planning notes — one Markdown file per epic, each split into stories — **not committed to the repo**; never link or cite its content, see the rule below).

## ⛔ Never reference the epic docs in shipped material

The epic planning docs live in the private `docs/epics/` folder —
**internal planning notes that are not committed to the repo** (one
Markdown file per epic, each split into stories). Do NOT reference them —
no links to `docs/epics/*.md`, and no "Epic N" / "Story X.Y" citations —
anywhere, including **this file** and **anything that ships or is
user-facing**: `README.md`, `SKILL.md` / any skill, the `examples/` apps,
the public docs, and code/header comments. Describe the behaviour or the
code directly instead (e.g. "the dual-core split in `fb_core1_dispatch`",
not "the dual-core split from Story N.M"). When you touch a comment that
cites an epic/story, rephrase it. Cross-references *between* the epic docs
themselves are fine — they're not committed. This is a hard rule.

## What this repo is

**MD/Lynx — an Atari Lynx emulator for the Atari ST / STE / MegaST(E)**,
built on the `md-framebuffer-template` for sub-20-millisecond audiovisual
SidecarTridge Multi-device microfirmware apps. It is a UF2 image that runs
on a Raspberry Pi Pico (RP2040) plugged into the Multi-device cartridge
slot: the Pico emulates a ROM cartridge for the Atari, runs the Lynx
emulation, and hands the ST a finished 320×200 16-colour screen every VBL
while also handling SD card I/O, keyboard/joystick input and audio.

- **Emulation core**: Handy (K. Wilkins, zlib licence), vendored from
  [xrip/pico-lynx](https://github.com/xrip/pico-lynx) under `rp/src/lynx/`.
  It stays C++ and keeps its licence text; every file altered for this port
  carries an `ALTERED SOURCE VERSION` banner (zlib clause 2) and its changes
  are tagged `MD/Lynx:`.
- **Display**: the Lynx's 160×102 4-bit screen is doubled to 320×204 and the
  top and bottom Lynx scanlines are dropped, so 320×200 fills the ST screen
  exactly. The Lynx's 16 on-screen colours go straight to the ST/STE shifter
  palette — no colour quantisation.
- **Sound**: STE/MegaSTE DMA sound (mono 8-bit PCM at 25,033 Hz) when the
  machine has it, with a YM2149 fallback on a plain ST; detected at boot by
  the m68k and reported to the RP over the cart bus.
- **Input**: Atari ST keyboard and the port-1 joystick, decoded on the RP.
- **Storage**: cartridge images (`.lnx` / `.lyx` / `.o` / `.bin`) live in the
  `lynx` folder on the microSD card.

The framebuffer platform underneath provides:
- **Dual (page-flipped) framebuffer on the Atari ST side** — tear-free display, fully managed for you.
- **Real 50 Hz**, locked to the ST's vertical blank.
- **~19 ms of compute every VBL** for the app to draw its frame.
- **Chunked drawing on the RP2040** — you write one byte per pixel; the framework does the chunked → Atari ST planar conversion for you.
- **~1 ms per VBL** for that chunky→planar conversion (split across both cores).
- **Atari ST keyboard and joystick handled on the RP2040** — decoded scancodes and stick state delivered straight to your app.

Network plumbing (WiFi / lwIP / mbedTLS / httpc) was deliberately stripped — apps that need it bring it back from `md-microfirmware-template` upstream.

## Build

Top-level build is driven by `build.sh` in the repo root:

```bash
# <board_type> = pico | pico_w | sidecartos_16mb
# <build_type> = debug | release   (note: always compiled as MinSizeRel — see below)
# <app_uuid_key> = UUID4 identifying this app, must match desc/app.json
./build.sh pico_w release 123e4567-e89b-12d3-a456-426614174000
```

Required host environment:
- ARM GNU Toolchain 14.2 or newer — export `PICO_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` dir.
- `atarist-toolkit-docker` (`stcmd`) — needed for the m68k target. `stcmd` requires a PTY (`pty=true`).
- SDK paths (auto-set from the repo if unset): `PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`.
- Git + GNU Make. VS Code users: the C/C++ Extension Pack, CMake Tools and Cortex-Debug.

For on-hardware debugging:
- A Raspberry Pi Debug Probe / Picoprobe wired to the Multi-device header — TX, RX **and both GND pins** must be connected.
- Optional helpers: `ARM_GDB_PATH` (the `arm-none-eabi/bin` dir) and `PICO_OPENOCD_PATH` (OpenOCD's `tcl` dir).

Build flow (orchestrated by `build.sh`):
1. Runs `tools/bump_version.sh`, which increments the patch in `version.txt` and syncs it to `rp/version.txt` and `target/version.txt`. Set `SKIP_VERSION_BUMP=1` to release `version.txt` as-is (this is what `make build` does).
2. Builds the Atari ST target (`target/atarist/build.sh`) via `stcmd make`. Enforces a **16 KB hard limit** on `BOOT.BIN` (the cartridge code budget — `CART_CARTRIDGE_CODE_SIZE` in `rp/src/include/cart_shared.h`, mirrored as `CARTRIDGE_CODE_SIZE` in `target/atarist/src/main.s`); a build that exceeds it aborts with `ERROR: cartridge code is N bytes; limit is 16384`. A separate copy (`FIRMWARE.IMG`) is then padded to 64 KB to fill the entire shared region, and `firmware.py` converts it into `rp/src/include/target_firmware.h` (a C byte array embedded in the RP firmware).
3. Builds the RP firmware (`rp/build.sh`): pins submodule versions (pico-sdk 2.2.0, pico-extras sdk-2.2.0, fatfs-sdk at a specific commit), runs CMake, produces `rp/dist/rp-<board>.uf2`. The FatFs configuration lives at `rp/src/ff/ffconf.h` and shadows the submodule's default via `target_include_directories(... BEFORE PRIVATE)` in `rp/src/CMakeLists.txt`, so the `fatfs-sdk` submodule stays pristine.
4. Computes MD5, renames to `dist/<APP_UUID>-<VERSION>.uf2`, and substitutes UUID/MD5/version into `dist/<APP_UUID>.json` from the `desc/app.json` template.

### Build gotchas
- **CMake always builds with `-DCMAKE_BUILD_TYPE=MinSizeRel`** regardless of the `<build_type>` argument. A full `Release` previously caused breakage (memory/over-optimization). The legacy line is left commented in `rp/build.sh`. `<build_type>` only controls the `DEBUG_MODE` macro and the dist filename.
- Harmless VASM warnings during the m68k build (`target data type overflow`, `trailing garbage after option -D`) can be ignored.
- VASM/`stcmd` errors like `the input device is not a TTY` mean `stcmd` was invoked without a PTY. `target/atarist/build.sh` already exports `STCMD_NO_TTY=1` for every `stcmd` call it makes; you only need to export it yourself if invoking `stcmd` directly from a non-TTY context (CI, sub-shells, build wrappers). Without it the m68k build can fail silently and the previous `BOOT.BIN` survives — leading to a working RP firmware that displays garbage on the ST because `target_firmware.h` is stale.

### Troubleshooting

| Symptom | Fix |
| --- | --- |
| `the input device is not a TTY` when using `stcmd` | `target/atarist/build.sh` already sets `STCMD_NO_TTY=1` for every stcmd call. If you invoke `stcmd` directly from a non-TTY context, export `STCMD_NO_TTY=1` first. |
| `arm-none-eabi-gcc not found` | Ensure `PICO_TOOLCHAIN_PATH` points to the Arm GNU toolchain bin dir. |
| `ERROR: cartridge code is N bytes; limit is 16384` | The m68k cartridge grew past 16 KB. Trim `target/atarist/src/` (`main.s` / `userfw.s` and their includes), or move data into `APP_FREE` / `SHARED_VARIABLES` rather than embedding it in the cartridge image. |
| Final steps fail copying the UF2 | An upstream compile failed — scroll back for the first error before the copy step. |
| `region RAM overflowed` at link, or the app bails to Booster at boot | RAM ran out. Check the `__bss_end__` .. `__StackLimit` window in `rp/build-*/rp.elf.map`; the emulator needs ~75 KB of heap left over. See "Memory layout". |
| The menu lists no cartridges | The SD folder from `ACONFIG_PARAM_FOLDER` (default `/lynx`) is missing, or it holds nothing with a `.lnx` / `.lyx` / `.o` / `.bin` extension under 512 KB. |
| The Atari ST display shows garbage | Almost always a stale `target_firmware.h`. Confirm `target/atarist/dist/BOOT.BIN` was regenerated by the current build (compare its timestamp to the rest of `dist/`); if `stcmd make` failed silently the previous `BOOT.BIN` survives and the m68k boots with the wrong shared-region addresses. |

### CI / release
- `.github/workflows/build.yml` builds `pico_w` Release on PR.
- `.github/workflows/release.yml` triggers on `v*` tags: builds, attaches UF2 + JSON to the GitHub Release, uploads to `s3://atarist.sidecartridge.com/`.
- `make tag` tags HEAD with the contents of `version.txt` and pushes the tag (which triggers release).
- `upload_s3.sh <file>` is a manual one-off uploader; needs `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY`.

### Tests
There is no test suite. "Verification" is: build succeeds, UF2 boots on hardware, and a cartridge from the SD folder runs — plus manual interaction over the serial debug console. The menu's **TIMINGS** setting overlays the per-frame emulation cost and the achieved frame rate, and the **ABOUT** page shows the same figures along with which audio back-end the m68k detected; both are the quickest way to tell whether a change helped or hurt.

## Architecture

The firmware is a **two-target build**: m68k assembly that runs on the Atari ST is compiled into a ROM image, embedded as a C array inside the RP2040 firmware, and served back to the Atari over the cartridge bus that the RP2040 emulates via PIO + DMA.

### Framebuffer pipeline (the headline feature)

End-to-end, every visible pixel on the ST goes through this path each VBL:

1. **RP draws into a chunked off-screen buffer** at RP `0x20000000+` (`fb_chunked_buffer`, 320×200 bytes = 64 KB, one byte per pixel, palette index in low nibble). `fb_font.c` writes text; `fb_blit.c` writes rectangles / bitmaps / sprites with color-key.
2. **RP publishes via chunky-to-planar conversion**. The chunked buffer is bit-transposed to ST planar 4 bpp by a hand-written Thumb assembly worker (`fb_chunked_asm.S` — `fb_c2p_half`). For each 4-pixel uint32 group, plane K's 4-bit nibble = `((q >> K) & 0x01010101) * 0x80402010 >> 28`. Work is split between **Core 0 (top 100 rows) and Core 1 (bottom 100 rows)**, synced via the inter-core FIFO. Both cores write into a 32 KB RP-RAM scratch (`fb_planar_scratch`) in natural row-major order; `fb_chunky_to_planar` then publishes the result into the cart FB at `$FA8300` (RP `0x20030000 + 0x8300`) with the 48-byte m68k MOVEM chunks **pre-reversed** (so the m68k's predec store in step 3 lands each chunk at its natural image position — see "Framebuffer chunk layout for the m68k MOVEM blit" in `cart_shared.h`). Total conversion time: ~1 ms c2p + ~60-120 µs chunk-reversed memcpy.
3. **m68k blits cart FB → ST screen page** inside `userfw.s`'s VBL loop via the `FBDRV_INLINE` macro — a fully unrolled `movem.l (a6)+, d0-d7/a1-a4` load + `movem.l d0-d7/a1-a4, -(a5)` predec store (**12 longwords / 48 bytes per iter, 666 iterations** for the full 200-line low-res FB, plus a `d16(a5)` MOVEM tail copying the last 32 bytes). A0 and A7 are intentionally omitted from the MOVEM list: A0 is the dedicated Timer-B audio cursor (kept stable across the macro so the audio IRQ handler doesn't have to save/restore it), and A7 keeps the supervisor SP valid so IRQs can fire safely during the macro. Predec mode is 4 cyc/iter faster than `d16(a5)` (8+8n vs 12+8n on 68000), at the cost of writing each chunk in reverse memory order — which is exactly what the RP-side chunk-reversed cart FB layout from step 2 compensates for. A5 is initialised to `UFW_SCREEN_PAGE + 31968` (page end of chunked region) and decrements back to page start over the 666 iters. Pure 68000 CPU, no STE blitter, no `_MCH` cookie dispatch — same code runs on plain ST / STE / MegaSTE / TT / Falcon.
4. **m68k flips video base** to the page it just wrote. `userfw.s` toggles between `$70000` and `$78000` every frame (`UFW_SCREEN_PAGE` XOR `UFW_SCREEN_XOR`), so the display always reads from a stable page while the next blit fills the other.
5. **PALETTE_IDX0 (`$FFFF8240`) doubles as a timing tape-measure** — `userfw.s` writes it at three points (post-vsync, blit-start, blit-end) so the ST border shows the blit cost as colored bands. White band = `FBDRV_INLINE` in flight (~17 ms at FB_COPY_LINES=200). Green band = slack until next vsync.

The number of lines copied by `FBDRV_INLINE` is parameterized by `FB_COPY_LINES` (default 200). Leaving rows untouched lets an app keep a static status bar in the destination ST page that the FB blit never overwrites. `FBDRV_TOTAL_BYTES = FB_COPY_LINES * FB_ROW_BYTES` is divided by `FBDRV_ITER_BYTES = 48` to derive `FBDRV_MAIN_ITERS`; trailing bytes (at most 47) are picked up by a small `d16(a5)` MOVEM tail block emitted via `ifne FBDRV_TAIL_BYTES`. The tail's MOVEM register list must hold exactly `FBDRV_TAIL_BYTES / 4` longwords — at FB_COPY_LINES=200 the tail is 32 B (`d0-d7`); at 199 it's 16 B (`d0-d3`); at 198 it's 0 B (block skipped). Adjust the register list manually when changing FB_COPY_LINES.

### IKBD pipeline (keyboard + joystick input, ESC exit)

`userfw.s` owns the Atari ST IKBD ACIA at `$FFFFFC00`/`02` end-to-end,
replacing TOS's handlers so the m68k VBL blit runs interrupt-free apart from
the two IRQs the app actually needs:

1. **m68k boot stubs 4 IRQ vectors** — HBL (`$68`), Timer-A (`$134`), Timer-C
   (`$114`) and Timer-D (`$110`) all point at a 1-instruction
   `userfw_dummy_irq` (just `rte`). MFP Timer A/C/D are disabled+masked at
   IERA/IERB so they never fire; HBL is masked by SR=$2300 (IPL=3 blocks
   IPL≤3). Timer-B (`$120`) is owned by the YM audio path (see "Audio
   pipeline"); it is left idle when STE DMA sound is in use.
2. **The ACIA vector (`$118`) gets a real handler**, `userfw_acia_irq`,
   enabled at IERB/IMRB bit 6. It reads each IKBD byte the instant it
   arrives and forwards it to the RP with a cart-bus read at
   `IKBD_WINDOW_BASE + byte` (`$FB8200..$FB82FF`, md-devops single-byte ABI).
   Interrupt-driven rather than polled inside the blit: the MC6850 has a
   one-byte receive buffer, and polling drops bytes, which desynchronises the
   multi-byte joystick packets. The MIDI ACIA shares the same MFP interrupt,
   so the handler drains it too (and discards it) or its level-asserted IRQ
   line would re-trigger forever. MFP is in auto-EOI mode (VR S=0), so the
   handler needs no in-service acknowledge; it saves only D0/A1, which makes
   it safe to fire in the middle of `FBDRV_INLINE`.
3. **m68k boot configures the IKBD** — `$80 $01` reset → ~110 ms busy-wait for
   the self-test → `$12` (disable mouse reporting) → `$14` (joystick event
   reporting). The byte stream is then keyboard scancodes plus `$FE`/`$FF`/
   `$FD` joystick packets.
4. **RP captures via commemul** — the 1 KB ROM3 DMA ring (`commemul.{c,pio}`)
   records every read in `$FB0000`–`$FBFFFF`. The main loop drains it with
   `fb_pump_rom3()`, whose callback routes each sample to the IKBD demux, the
   VBL frame-sync detector and the sound-capability decoder.
5. **RP demux** (`ikbd_pump()`) classifies each raw byte: `$00..$7F` = key
   press, `$80..$F1` = key release (scancode = `byte & $7F`), `$FD/$FE/$FF` =
   joystick packet headers whose following state byte(s) are framed into
   `s_joy_state[]`. Only port 1 is reported by `ikbd_get_joystick()` (bit0 up,
   1 down, 2 left, 3 right, 7 fire) — with joystick reporting on, a mouse in
   port 0 reports as joystick 0 and its stationary quadrature pattern would
   otherwise latch a phantom direction forever. Apps drain key events via
   `ikbd_pop_key()`; scancode `$00` is suppressed.
6. **ESC** — `ikbd.c` can post `CART_CMD_BOOT_GEM` to the sentinel slot on an
   ESC press+release within 200 ms, but MD/Lynx turns that off
   (`ikbd_set_esc_auto_exit(false)`) because ESC toggles the emulator menu.
   Leaving to GEM is the menu's own **Exit** item, which calls
   `ikbd_request_boot_gem()`; the m68k VBL loop polls the sentinel each frame
   and exits cleanly. `ikbd_clear_command()` at the top of the main loop
   re-arms the slot to `CMD_NOP` so the exit is a one-shot and does not
   re-trigger after an ST reset.

The earlier ROM4 + DMA-IRQ-per-read capture route (a first attempt) saturated
the Cortex-M0+ during fbdrv blits (~8M cart reads/sec) and silently dropped
IKBD bytes; switching to the dedicated commemul DMA ring eliminated the loss.

### Audio pipeline (STE DMA sound, YM2149 fallback)

The back-end is chosen at **runtime** from what the machine actually has, and
both paths are compiled in. The RP produces whichever sample format the m68k
asks for; the app-side fill callback never has to know which machine it is on
beyond calling `audio_get_mode()`.

1. **Detection.** At boot `userfw.s` walks the cookie jar (`_p_cookies` at
   `$5A0`) for `_SND` and tests bit 1 (DMA / PCM sound), storing the result in
   `UFW_HAS_DMA`. A null jar or a missing `_SND` (plain ST / early TOS) means
   no DMA. Building with `FORCE_NO_DMA=1` forces the fallback so the YM path
   can be exercised on an STE.
2. **Reporting.** Every VBL the m68k does one cart-bus read at
   `SNDCAP_WINDOW_BASE + has_dma` (`$FB8600`). `fb.c`'s ROM3 dispatch hands every
   sample to `audio_consume_rom3_sample()`, which owns that window and
   calls `audio_set_mode(AUDIO_MODE_DMA | AUDIO_MODE_YM)` to set the
   per-VBL refill size. Before the first report the mode is
   `AUDIO_MODE_SILENT` and the buffer is filled with zeros — silence for both
   readers.
3. **STE DMA path (preferred).** Mono, 8-bit **signed** PCM at 25,033 Hz =
   **500 bytes per PAL VBL**. The DMA runs in loop mode over a double buffer
   parked in the unused tails of the two ST screen pages (`$77D00` / `$7FD00`)
   — past the 32,000-byte framebuffer, below the userfw scratch slots, never
   displayed and never touched by the blit, so it costs no separately
   allocated ST RAM. Each VBL the m68k reads bit 7 of the DMA frame-counter
   mid byte (`$FFFF890B`) to see which buffer the DMA is playing, copies the
   fresh 500 bytes from the cart audio buffer into the *other* one, and points
   the loop at it. No Timer-B, no YM, no CPU per sample.
4. **YM2149 fallback.** MFP Timer-B in /4 delay mode with TBDR=110 →
   5,585.45 Hz, ~112 fires per PAL VBL = **224 bytes per VBL** of (vA, vB)
   volume pairs. Each fire writes channel A's volume, latches register 9,
   writes channel B's volume and re-latches register 8 — ~112 cycles per fire,
   ~8% of the VBL. A0 is the dedicated Timer-B read cursor, kept out of
   `FBDRV_INLINE`'s MOVEM list and reset to the buffer base by `userfw_vbl`
   every vsync; that VBL-edge resync is what keeps the m68k and the RP in step
   without a chunk counter. Pairing two channels through the 1988 Ghostbusters
   demo's 64-entry LUT extracts ~6 effective bits from the YM's logarithmic
   volume curve where one channel would give 4.
5. **RP side.** `audio_render_frame()` paces to ~50 Hz via `time_us_32` and
   asks the installed fill callback for exactly the current mode's byte count,
   writing into the 1 KB cart buffer at `CART_AUDIO_BUFFER_OFFSET` (`$FA4100`).
   MD/Lynx installs `lynxemu_audio_fill`, which resamples the emulator's
   25 kHz mono output onto the requested count (see "Audio" under the Lynx
   section).

Audio uses no Atari RAM in the YM path and only the two screen-page tails in
the DMA path; the cart buffer at `$FA4100` is the whole I/O channel.

Tuning knobs:
- `TIMERB_COUNT` in `userfw.s` sets the YM rate. It must stay in step with
  `AUDIO_FILL_BYTES_YM` in `rp/src/audio.c` (= 2 × fires-per-VBL), or the m68k
  and the RP drift apart.
- `STE_SND_BYTES` in `userfw.s` and `AUDIO_FILL_BYTES_DMA` in `audio.c` must
  match (500 = one PAL VBL at 25,033 Hz, and a multiple of 4 for the copy
  loop).

### Atari ST side (`target/atarist/`)
- `src/main.s` — m68k cartridge header + boot dispatcher. Lives at `$FA0000` (ROM4 cartridge region). `pre_auto` (CA_INIT bit 27) runs in supervisor mode, copies `start_rom_code` below ST screen memory, jumps there, checks resolution (high-res falls to GEM with a warning), then **`jmp USERFW` directly** — no terminal command dispatch in the default path. The old `check_commands` / `rom_function` indirection is still defined (for apps that want CMD_START-style handoff from the RP) but unused.
- **No Atari RAM**: the cartridge code deliberately uses none of the ST's RAM (no `.bss`, no heap). Data lives in registers or the shared region. Adding naive BSS to `userfw.s` will silently corrupt system variables — keep state in `APP_FREE` / `SHARED_VARIABLES`, the cart code itself (which is **read-only** from m68k — writes bus-error), or registers. Key live registers across the `userfw.s` VBL loop: A0 = Timer-B audio read cursor (reset to `AUDIO_BUFFER_ADDR` by `userfw_vbl` each VBL, never in any MOVEM list), A5 = per-VBL FB destination scratch (predec target for `FBDRV_INLINE`), A6 = per-VBL FB source scratch (postinc reader of the cart FB), A7 = SP (also kept out of MOVEM so IRQs stay safe). D0-D7 / A1-A4 are scratch and are clobbered by the MOVEM macro.
- `src/userfw.s` — **the primary extension point for app-specific m68k code.** `src/userfw.ld` places `main.s` at offset `0x0000` (≤ 2 KB) and `userfw.s` at offset `0x0800` (code + the inline `FBDRV_INLINE` macro expansion). The previously-separate `fbdrv.s` module at offset `0x2000` was eliminated: the unrolled MOVEM block is now emitted inline at the userfw entry via the `FBDRV_INLINE` macro, so there's no jsr/rts overhead and no separate hand-written file to maintain. Total cart code ≤ 16 KB (`CARTRIDGE_CODE_SIZE`); the current build is ~8.3 KB. `main.s` exposes the userfw entry as `USERFW equ (ROM4_ADDR + $800)`. MD/Lynx's `userfw.s` runs the FB blit + audio (STE DMA or YM) + interrupt-driven IKBD VBL loop and exits on the RP's `CMD_BOOT_GEM`.
- Adding more m68k modules: add a new `.text_<name>` section in `userfw.ld`, mirror the offset with an `equ (ROM4_ADDR + $????)` in `main.s`, and add the `.o` target to `target/atarist/Makefile`.
- Built via `stcmd make release` (m68k assembler in Docker). A 64 KB padded copy of the BOOT.BIN is then converted to `target_firmware.h` for inclusion in the RP build.

### Shared 64 KB cartridge region
The Atari ST sees a 64 KB window at `$FA0000`–`$FAFFFF` (mirrored RP-side at `0x20030000`). This is the **single source of truth** for any cross-target data layout — both sides derive every offset symbolically from constants in `rp/src/include/cart_shared.h` (RP-side) and `target/atarist/src/main.s` (m68k side). **Apps must never hard-code an address inside this region** — always reference the named offset/symbol.

| Offset | Symbol | Size | Purpose |
| --- | --- | --- | --- |
| `$FA0000` | cartridge image | 16 KB | m68k header + main.s (≤ 2 KB) + userfw.s at `$800` (code + the inline `FBDRV_INLINE` expansion). Read-only from m68k. |
| `$FA4000` | `CMD_MAGIC_SENTINEL_ADDR` | 4 B | RP→m68k command word (`CMD_NOP`, `CMD_RESET`, `CMD_BOOT_GEM`, `CMD_START`). |
| `$FA4004` | (reserved) | 8 B | Former RANDOM_TOKEN / RANDOM_TOKEN_SEED slots from the TPROTOCOL handshake; unused since that handshake was removed. |
| `$FA400C` | `FB_FRAME_COUNTER_ADDR` | 4 B | RP-incremented dirty-frame counter; `userfw.s` only blits when this changes. |
| `$FA4010` | `SHARED_VARIABLES` | 240 B | 60 × 4 B indexed slots. **Slots 12–19 are the palette below — not free.** The other 52 slots are free for apps. |
| `$FA4040` | `PALETTE` (`CART_PALETTE_OFFSET` / `PALETTE_ADDR`) | 32 B | 16 ST colour words (`0000.0RRR.0GGG.0BBB`) inside `SHARED_VARIABLES` slots 12–19. The m68k VBL handler copies these to `$FFFF8240..$FFFF825E` every frame, so the RP owns the palette. Leave it zeroed (= all-black) and write `$FFFF8240` from your own m68k code if you'd rather drive it yourself. |
| `$FA4100` | `AUDIO_BUFFER_ADDR` | 1024 B | Audio buffer, refilled by the RP once per VBL with whichever format the detected back-end wants: 500 signed PCM bytes for STE DMA sound (the m68k copies them into the screen-page-tail double buffer the DMA is not playing), or ~224 B of (vA, vB) YM volume pairs consumed by the Timer-B IRQ (A0 is reset to the buffer base every vsync, and the remaining ~800 B is drift headroom). |
| `$FA4500` | `APP_FREE_ADDR` | ~15.5 KB | Contiguous arena, ends at FRAMEBUFFER. The ST never reads it, so MD/Lynx overlays it with the `CART_APP_FREE` linker region and parks RP-side buffers there (`__cart_app_free`) — see "Memory layout". |
| `$FA8300` | `FRAMEBUFFER_ADDR` | 32000 B | 320×200×4bpp color framebuffer. Top of the region so an overrun walks off the end of the 64 KB window instead of corrupting the metadata block. Read by `FBDRV_INLINE` every VBL. |

### RP2040 side (`rp/src/`)
- `main.c` — only sets clock/voltage, calls `gconfig_init` (global config) then `aconfig_init` (per-app config), and hands off to `emul_start()`. If config init fails it jumps to the **Booster** app via `reset_jump_to_booster()` to bootstrap. **Don't add features to `main.c`** — put them in `emul.c` or a new module.
- `emul.c` / `emul.h` — the application's main loop and entry point. ~100 lines: erase + copy firmware to RAM, init IKBD ring, init romemul, init fb, init commemul, mount SD, configure SELECT, bring up the emulator front-end (`lynxemu_init()`), install `lynxemu_audio_fill`, then loop `{ikbd_clear_command(); fb_pump_rom3(); ikbd_pump(); drain key events → lynxemu_handle_key(); lynxemu_render_frame(); audio_render_frame();}`. `fb_pump_rom3()` (in `fb.c`) is the public wrapper that drains the ROM3 ring via `commemul_poll(fb_rom3_dispatch)` for IKBD demux, VBL frame-sync and the sound-capability report.
- `fb.c` / `fb.h` — **owns the 32 KB planar framebuffer at `$FA8300` + the boot-time framebuffer setup.** `fb_init(&fb_mode_320x200)` populates `fb_screen` (pointer + width + height + bpp), calls `fb_chunked_init()` (which launches Core 1) and paints one boot splash frame. `fb_render_static()` / `fb_render_frame()` only exist for that splash — every frame after it comes from `lynxemu_render_frame()`. `fb_publish()` (the VBL-synced transpose + cart-FB publish) is the call that hands a finished frame to the ST. `fb_rom3_dispatch` also decodes the m68k's sound-capability report and drives `audio_set_mode()`.
- `fb_chunked.c` / `fb_chunked.h` — **chunked off-screen buffer + dual-core publish dispatcher.** `fb_chunked_buffer` is the 320×200 byte-per-pixel render target apps draw into. It is a macro for `fb_chunked_store.pixels` — the pixels sit inside a struct with guard bands either side so the Lynx renderer can write scaled scanlines straight into it (see the Lynx section); it still behaves as a `uint8_t[FB_CHUNKED_SIZE]` for indexing, `&` and `sizeof`. `fb_chunked_init()` launches Core 1 with `fb_core1_loop`, a **generic job dispatcher**: `fb_core1_dispatch(fn, arg)` hands a function+arg to the parked Core 1 over the inter-core FIFO and `fb_core1_wait()` joins — the c2p bottom half and the flash-programming park job both ride this one path. `fb_chunky_to_planar(planar)` runs c2p into `fb_planar_scratch` (32 KB in RP RAM, natural row-major planar layout) using both cores, then publishes scratch → cart FB via the `fb_chunk_reverse_copy48` Thumb asm worker (chunk-reversed `ldmia/stmia`) so the m68k's predec MOVEM blit lands each 48-byte chunk at its natural image position. The chunk-reversal mapping is specified in `cart_shared.h`.
- `fb_chunked_asm.S` — **hand-written Thumb assembly worker.** `fb_c2p_half(dst, src, src_end)` processes a chunked range using a multiplication-based bit transpose (`((q & 0x01010101) * 0x80402010) >> 28` packs a 4-pixel plane bit into a nibble in one MUL). Writes natural row-major planar output to `dst` — chunk reversal happens in the dispatcher (`fb_chunky_to_planar`), not the worker. Both cores execute this same code on their respective halves. No LUTs in scratch; only two 32-bit constants in registers.
- `fb_font.c` / `fb_font.h` — text primitives. `font_set_font(&font8x8)`, `font_set_color(idx)`, `font_move(x, y)`, `font_print(str)`. The 8×8 cell gives 40×25 characters over the ST screen. `render_text` writes single bytes into `fb_chunked_buffer` (no LUT, no plane math) so the cart-bus byte-swap is irrelevant at the font layer.
- `fb_blit.c` / `fb_blit.h` / `fb_blit_asm.S` — bitmap / sprite primitives. `fb_fill_rect(x, y, w, h, color)`, `fb_blit(bm, dst_x, dst_y)` (opaque memcpy per row), `fb_blit_key(bm, dst_x, dst_y, key)` (color-key transparent sprite, inner row in the `fb_blit_key_row` Thumb asm). `_band` variants (`fb_blit_band` / `fb_blit_key_band`) clip vertically to `[y0, y1)` so two cores can render disjoint screen bands. All target the chunked buffer.
- `font8x8.h` — 8×8 ASCII font data + the `FB_FONT font8x8` instance definition. Include from exactly one .c file (currently `fb.c`) to avoid multiple-definition link errors; other files use `extern const struct FB_FONT font8x8;`.
- `lynxemu.cpp` / `include/lynxemu.h` — **the Atari Lynx front-end and the app's primary source file.** Owns the cartridge menu, the SD→flash cartridge loader, the display scaling and palette publish, the ST-keyboard/joystick → Lynx pad mapping, the 60 Hz → 50 Hz frame pacing and the audio resampler. `emul.c` drives it with `lynxemu_init()` / `lynxemu_handle_key()` / `lynxemu_render_frame()` / `lynxemu_audio_fill()`. See "Atari Lynx emulator" below.
- `lynx/` — **vendored Handy emulation core** (C++). Copyright (c) K. Wilkins, zlib licence (`lynx/license.txt`), taken from [xrip/pico-lynx](https://github.com/xrip/pico-lynx). Files altered for this port carry an `ALTERED SOURCE VERSION` banner and mark their changes `MD/Lynx:`; keep it that way, and keep changes to the minimum that the port actually needs.
- `romemul.c` / `romemul.pio` — PIO programs and the runtime that emulates the cartridge ROM/RAM bus to the Atari (driven by `READ_*` / `WRITE_*` GPIOs defined in `include/constants.h`).
- `commemul.c` / `commemul.pio` — **ROM3 cart-bus capture ring; the primary extension point for reading anything the m68k forwards via cart reads in `$FB0000`–`$FBFFFF`.** `commemul_init()` starts a PIO+DMA chain that records every ROM3 read into a ring buffer with no CPU/IRQ involvement. The ring is 1 KB (512 samples) and parked in the cart-region hole — ROM3 carries only IKBD bytes plus two one-word reports per frame, so that is ample, and the RAM it used to take is needed by the emulator. The main loop drains it via `commemul_poll(callback)`; each captured 16-bit sample is the cart-bus address that was read (low byte typically carries the payload, high byte the discriminator window — see `ikbd.c` for the canonical example at `$FB8200..$FB82FF`). The upstream `chandler.c` + `tprotocol.h` command-dispatch layer that previously sat on top of commemul was removed — apps that need a multi-byte command channel can wrap `commemul_poll` with their own callback.
- `gconfig.c` / `aconfig.c` — global vs per-app configuration stored in dedicated flash sectors, on top of `settings/` (a key-value store). The global-config defaults in `gconfig.c` mirror the Booster app and include WiFi parameters even though this template no longer brings up networking — **do not trim them**.
- `sdcard.c`, `hw_config.c` — FatFs over SPI/SDIO via the bundled `fatfs-sdk`.
- `select.c`, `reset.c` — SELECT-button handling, soft reset/jump-to-booster.
- `xpadin.c` / `xpadin.h` — receiver for the m68k's per-VBL Xpad controller report (see "Xpad" below). The ABI it speaks comes from the `lib/xpad` submodule, not from a copy in this tree.
- `ikbd.c` / `ikbd.h` — IKBD keyboard ingest + demux. Chained off `commemul_poll` in the main loop; filters the `$FB8200..$FB82FF` window and decodes scancodes into a key-event ring (`ikbd_pop_key`). ESC press+release within 200 ms signals m68k exit via `CART_CMD_BOOT_GEM`. See `cart_shared.h` for the shared-region constants.

The `display.c` / `term.c` / `u8g2/` subsystem from upstream `md-microfirmware-template` was deleted — the framebuffer template's UI is just "draw into `fb_screen`".

### Memory layout (`rp/src/memmap_rp.ld`)
The RP2040's 2 MB flash is sliced into named regions, and code is responsible for not stomping on them:

| Region | Origin | Length | Purpose |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 640 K | App code (~200 K used) |
| `ROM_TEMP` | `0x100A0000` | 512 K | The loaded Lynx cartridge image (see below) |
| `BOOSTER_APP_FLASH` | `0x10120000` | 768 K | Reserved for the Booster app (do not write from this app) |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | 30 sectors of per-app config |
| `GLOBAL_LOOKUP_FLASH` | `0x101FE000` | 4 K | UUID → config-sector lookup |
| `GLOBAL_CONFIG_FLASH` | `0x101FF000` | 4 K | Global config |
| `RAM` | `0x20000000` | 192 K | Normal RAM |
| `ROM_IN_RAM` | `0x20030000` | 64 K | The shared cartridge region the m68k reads |
| `CART_APP_FREE` | `0x20034800` | 15,104 B | Overlay on the unused hole inside `ROM_IN_RAM` |

`FLASH` was cut from the template's 1024 K to 640 K so `ROM_TEMP` can hold a
512 K Lynx cartridge; the two together must stay below `BOOSTER_APP_FLASH` at
`0x10120000`. `ROM_TEMP_SIZE_BYTES` in `constants.h` mirrors the length —
keep them in step.

**RAM is the binding constraint and there is not much of it left.** The 192 K
region has to hold the 64,000-byte chunked framebuffer, the 32,000-byte planar
scratch, ~14 K of RAM-resident code, and a heap big enough for the emulated
Lynx's own 64 K of system RAM plus Mikey/Suzy/EEPROM (~70 K in total). Three
things buy the room:

- **`CART_APP_FREE`** overlays the 15,104-byte hole between `CART_APP_FREE_OFFSET`
  (`$4500`) and `CART_FRAMEBUFFER_OFFSET` (`$8300`) inside the shared region —
  ordinary SRAM that neither the ST nor the framebuffer path reads. Buffers
  tagged `__cart_app_free("name")` (`cart_shared.h`) live there: the commemul
  DMA ring, the cartridge list, and the two audio buffers. It is `NOLOAD`, so
  only park things first touched after `emul_start()` has called
  `ERASE_FIRMWARE_IN_RAM()`. `emul_start()` re-checks the window against
  `cart_shared.h` at boot, and the linker script asserts the section starts on
  `ORIGIN`.
- **Mikey renders straight into the chunked framebuffer** rather than into a
  160×102 intermediate — see the Lynx display path below. That saves 16 K at
  the cost of 1,024 bytes of guard band (`FB_CHUNKED_GUARD_LO` /
  `FB_CHUNKED_GUARD_HI` in `fb_chunked.h`).
- **`C65C02::Update` is deliberately NOT `__time_critical_func`.** The
  generated opcode switch is ~27 K and simply does not fit in RAM here; it runs
  from XIP. `CMikie::Update` stays RAM-resident.

If you add a `static` array, check the link: `__bss_end__` .. `__StackLimit`
in `rp/build-*/rp.elf.map` is the whole heap window, and it needs to stay above
~75 K or the emulator's allocations start failing.

The build assumes Core 0 owns flash writes (`PICO_FLASH_ASSUME_CORE0_SAFE=1`). **Core 1 is parked in `fb_core1_loop`** (`fb_chunked.c`), a generic job dispatcher; the chunky-to-planar bottom half rides it, and so does the RAM-resident park job `lynxemu.cpp` uses to hold Core 1 still while it programs a cartridge into flash. Each `fb_core1_dispatch` must be joined with `fb_core1_wait` before the next one.

Core 0 runs at **400 MHz at `VREG_VOLTAGE_1_30`** (`RP2040_CLOCK_FREQ_KHZ` in `constants.h`) so the Lynx core has enough headroom; the cart-bus PIO programs keep their proven 225 MHz cycle timing via the `SAMPLE_DIV_FREQ` clock divider, and the QSPI flash divider is raised to /4 (`PICO_FLASH_SPI_CLKDIV=4`, set on the SDK's `bs2_default` boot-stage-2 target) so flash SCK stays at 100 MHz. If a board fails to boot at 400 MHz (silicon lottery), step down: 360000 @ `VREG_VOLTAGE_1_25`, then 300000 @ `VREG_VOLTAGE_1_15`, then 225000 @ `VREG_VOLTAGE_1_10` (a /2 flash divider is only safe at 225).

### Atari Lynx emulator (`rp/src/lynxemu.cpp`, `rp/src/lynx/`)

**Cartridge storage.** A Lynx cartridge is up to 512 KB, which is far more
than the RAM budget allows, so the selected image is programmed into the
`ROM_TEMP` flash window and `CCart` reads it in place over XIP (it never
copies the ROM — it just keeps pointers into it). `flash_cartridge()` reads
32 KB at a time from SD into the chunked framebuffer, then erases/programs
that chunk. Because SD access needs XIP live, the erase/program calls are
bracketed by `save_and_disable_interrupts()` and by parking Core 1 in a
RAM-resident spin (`core1_park_job`, dispatched through `fb_core1_dispatch`)
— Core 1's normal FIFO wait lives in flash and would fault the moment XIP
goes away. The whole image is erased in one call so the SDK can use 64 KB
block erases; a 256 KB cartridge takes a couple of seconds, during which the
ST keeps displaying the last published frame (the "LOADING" screen).

**Display path.** `CMikie::DisplayRenderLine` writes the raw 4-bit Lynx pen
index, doubled horizontally, straight into `fb_chunked_buffer` at a 640-byte
pitch (`MDLYNX_DISPLAY_PITCH`), so each Lynx scanline lands on an even ST row;
`double_rows()` fills the odd rows afterwards. `gPrimaryFrameBuffer` is set one
scanline *below* the buffer, so Lynx line 1 lands on ST row 0 and Lynx lines 0
and 101 land in the guard bands and are never seen — that is the "320×204 with
two pixels cropped top and bottom" the port targets, and it needs no
intermediate buffer. Rotated cartridges (Gauntlet and friends) render upright;
nothing calls `SetRotation()`, exactly as upstream pico-lynx ships.

**Palette.** The Lynx and the ST both show 16 colours at once, so
`publish_lynx_palette()` copies Mikey's `mPalette[]` to the shifter every frame
instead of quantising pixels. `lynx_to_st()` packs each 4-bit channel into the
STE layout (LSB in the top bit of the nibble), which a plain ST reads as the
correct 3-bit approximation — one palette word serves both machines. Palette
index 0 also drives the ST border. This is why the vendored core's 8 KB
`mColourMap[4096]` lookup was removed.

**Frame pacing.** The Lynx runs at 60 Hz and the ST VBL is 50 Hz, so
`lynxemu_render_frame()` advances the emulator six frames for every five
displayed ones (`s_frame_acc`); the extra frame is emulated with `draw = false`
so it costs no pixel work. That keeps game speed and audio pitch correct at the
price of one doubled frame per 100 ms. The menu's **SPEED** item switches to
"50HZ SMOOTH": one emulated frame per VBL, perfectly even but ~17% slow.

**Audio.** Mikey is configured to generate at exactly the STE DMA rate
(`HANDY_AUDIO_SAMPLE_FREQ` 25,033), so nothing is resampled on the STE path.
After each emulated frame the stereo output is folded to mono signed 8-bit and
pushed onto a 2,048-sample ring; `lynxemu_audio_fill()` box-filters that ring
onto exactly the count the detected back-end wants — decimating slightly at
60 Hz (500.4 produced vs 500 consumed), stretching in the 50 Hz mode, and
holding the last level on an underrun. The YM fallback maps each resampled
value through the Ghostbusters `(vA, vB)` LUT.

**Controls.**

| Atari ST | Lynx |
| --- | --- |
| Cursor keys, or joystick in port 1 | D-pad |
| Alt, Space, Insert, joystick fire | Button A |
| Control, either Shift, Clr/Home | Button B |
| F1 / F2 | Option 1 / Option 2 |
| F3 or P | Pause |
| ESC | Toggle the menu |

Pause is doubled on F3 so the Option+Pause chords several titles use (screen
flip for left-handed play among them) fall under one hand; F1+P does not.

An Xpad gamepad reaches all of it (see "Xpad" below): d-pad to d-pad, South to
A, East to B, West or left shoulder to Option 1, North or right shoulder to
Option 2, Start to Pause, Select/Guide to toggle the menu. `pad_buttons()` ORs
the ST joystick and the Xpad into one Lynx pad state, and `menu_nav_bits()`
does the same for menu navigation so a single edge detector covers both.

In the menu the cursor keys navigate and adjust, Return/Space select, and ESC
goes back to the game. The menu is drawn full-screen with its own palette (the
running cartridge owns the palette otherwise) and highlights the cartridge
currently in flash. It is one flat list with the settings block on top and
the cartridges below, the same layout as MD/Speccy: `sel_to_row()` is a plain
`+ UI_ITEM_COUNT` offset, so the settings land in reverse declaration order
with EXIT heading the list and VOLUME immediately above the first cartridge,
and it opens on that first cartridge with the settings scrolled above it.

**Xpad.** [Xpad](https://github.com/neilrackett/atarist-xpad) is an open
standard for describing a modern gamepad to ST software: a provider publishes
a shared block and installs the `XPAD` cookie, and consumers poll it. MD/Lynx
implements the consumer half, which is how a pad reaches the Lynx's Option 1,
Option 2 and Pause -- inputs a one-button ST joystick simply cannot express.

The block lives in ST RAM, so the consumer is split across both targets.
`userfw.s` walks the cookie jar at boot, validates the block the way
`xpad_valid()` does (magic, major version, and a pad area that fits inside
`hdr_size`), and precomputes the address of pad 0 in each of the two buffers
so the per-VBL path needs no multiply. Each VBL it reads pad 0 tear-free --
sample `seq` and `active`, copy, recheck both, since the provider bumps `seq`
before flipping `active` -- and forwards the button mask as two cart-bus
reads: the high byte at `XPAD_HI_WINDOW_BASE` (`$FB8800`) then the low byte at
`XPAD_LO_WINDOW_BASE` (`$FB8A00`). `xpadin.c` latches the high byte and
commits the pair only when the low byte lands, so a ring drain falling between
the two reads cannot tear the value. A slot reporting `type == 0` is skipped,
which is also what tells the RP to fall back.

**Where the ABI comes from.** `lib/xpad` is a submodule of
[atarist-xpad](https://github.com/neilrackett/atarist-xpad), pinned to a
commit, and it is the single source of truth for both halves of the port: the
RP compiles against its `src/xpad.h` (added to the include path in
`rp/src/CMakeLists.txt`) and `userfw.s` assembles against its `src/xpad.inc`,
which upstream generates from that same header with a staleness check in its
own build. So the button bit values and field offsets the two targets use are
the same pinned revision by construction, not by anyone remembering to
re-copy.

There is one wrinkle worth knowing before you move anything. `stcmd` runs
`docker run -v $ST_WORKING_FOLDER:/tmp`, mounting **only** `target/atarist`,
so `lib/xpad` at the repository root is invisible to vasm inside the container
and no `-I` can reach it. `target/atarist/build.sh` therefore copies
`xpad.inc` into `src/inc/` before invoking the m68k make. That copy is a build
artifact, regenerated every build and gitignored -- it cannot go stale against
the submodule the way a committed vendored copy would. If the submodule is
missing, the build fails with a message telling you to run `git submodule
update --init --recursive` rather than assembling against a stale include.

Note also that `XPAD_COOKIE` (the jar tag a discovery walk matches) and
`XPAD_MAGIC` (the sentinel inside the block) hold the same value today but are
deliberately separate constants and are not promised to stay equal. Match each
with its own name; `userfw.s` does.

Only bits 0..15 of the mask are carried over the cart bus; bit 16, the right
stick click, is dropped. `xpadin_get()` reports absence after 250 ms without a
report, so an unplugged pad falls back within a quarter of a second.

Two things to keep in mind. Pad 0 is the only slot read -- MD/Lynx is
single-player, and merging slots would let a phantom direction from a mouse
reporting as joystick 0 leak in, the same hazard `ikbd.c` avoids by reporting
only port 1. And MD/Lynx boots at CA_INIT bit 27, *before* the AUTO folder, so
a provider installed as a TSR is not resident when the cookie walk runs; it
has to come from something already present when the cartridge takes over.

**SD card layout.** Cartridges live in the folder named by the per-app config
key `ACONFIG_PARAM_FOLDER`, default `/lynx` — editable from Booster without
recompiling. `.lnx`, `.lyx`, `.o` and `.bin` are listed (headered images,
raw dumps and BS93 homebrew); files over 512 KB and dot files are skipped.
A cartridge with an EEPROM gets a `<name>.eep` save file alongside it, written
when the cartridge is swapped out.

### App identity
`CURRENT_APP_UUID_KEY` (set from the `APP_UUID_KEY` env var at CMake time, with a placeholder default) is the app's UUID4. It must match the `uuid` field in `desc/app.json` and is used as the key into `GLOBAL_LOOKUP_FLASH` to find this app's config sector. Mismatch → app jumps to Booster.

## Editing guardrails

- **`rp/src/lynx/` is vendored upstream code** (Handy, zlib licence). Change it only when the port genuinely requires it, keep the licence text, keep the `ALTERED SOURCE VERSION` banner at the top of every file you touch, and tag each change with an `MD/Lynx:` comment explaining why — that banner is what zlib clause 2 requires of an altered redistribution.
- **Never modify** `pico-sdk/`, `pico-extras/`, or `fatfs-sdk/` — they are git submodules pinned to specific upstream revisions, and the build re-pins them on every run. To change FatFs configuration, edit `rp/src/ff/ffconf.h` (project-owned override); the include path is set up so this file wins over the submodule's default.
- Don't touch `main.c` for feature work — start in `emul.c`, or `lynxemu.cpp` for anything the emulator front-end owns.
- Match the existing C style (clang-format config in `.clang-format`, clang-tidy in `.clang-tidy` — both wired up via CMake when the binaries are on `PATH`).

---

## Working style

These behavioral guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think before coding

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity first

Minimum code that solves the problem. Nothing speculative.
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical changes

Touch only what you must. Clean up only your own mess.
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- When your changes orphan an import/variable/function, remove it. Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

### 4. Goal-driven execution

Define success criteria. Loop until verified.
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan with a verification check per step.

### 5. No AI attribution

Never add AI-tool attribution to commits, PR descriptions, code comments,
docs, or any other artifact. This means **no**:
- "Generated with Claude Code", "Co-authored by Claude", "Made with ChatGPT",
  or any similar phrasing.
- `Co-Authored-By: Claude …`, `Co-Authored-By: ChatGPT …`, or any other
  AI co-author trailer.
- "AI-assisted", "written with the help of an LLM", etc., as comments or
  changelog entries.

Write the message as the human author. Do not mention AI tools used to
produce the work.

### 6. Copyright headers

Always include the appropriate copyright notice at the top of every source file
that is created or modified in the following format, where `[AUTHOR_NAME]` is 
the name of the human author:

```c
/*
 * Copyright (C) 2026 [AUTHOR_NAME]
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

```
