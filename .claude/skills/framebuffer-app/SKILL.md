---
name: framebuffer-app
description: >-
  Use when building, modifying, or debugging MD/Lynx -- an Atari Lynx
  emulator built on the md-framebuffer-template, the platform for
  sub-20-ms audiovisual SidecarTridge Multi-device microfirmware apps for
  the Atari ST; you draw a 320x200 16-colour framebuffer on the RP2040 in
  the cartridge and the firmware blits it to the ST each VBL. Covers the
  chunked framebuffer, the fb_* / palette / audio / ikbd APIs, the
  main-loop pattern, and the build / RAM constraints. Triggers on
  tasks like "draw X on the Atari screen", "add audio/input", "why is the
  emulator slow", or "why does my app show garbage".
---

# MD/Lynx on the md-framebuffer-template

This repo is **MD/Lynx**, an Atari Lynx emulator for the Atari ST / STE /
MegaST(E), built on the **md-framebuffer-template** platform for
sub-20-millisecond audiovisual SidecarTridge Multi-device microfirmware
apps. The app runs on the Pico in the cartridge: you draw a 320×200
16-colour framebuffer in the Pico's RAM and the firmware blits it to the
ST each VBL (50 Hz), with keyboard, joystick and audio handled for you.

**Read `AGENTS.md` first** — it is the full playbook, and the "Atari Lynx
emulator" section covers the parts specific to this app (cartridge
flashing, the display path, the palette, frame pacing, audio, controls).
This skill is the quick reference for the platform underneath it.

**Platform capabilities:** a dual (page-flipped) framebuffer on the ST
side (tear-free, managed for you); real 50 Hz locked to the ST's vertical
blank; **~19 ms of compute every VBL**; chunked drawing on the RP2040 (one
byte per pixel) with the chunked → ST planar conversion done for you in
**~1 ms per VBL** across both cores; **STE DMA sound with a YM2149
fallback**, chosen at runtime from what the machine has; and ST keyboard
plus port-1 joystick decoded on the RP2040.

## The model (read this first)

The app runs on the RP2040 in the cartridge. You **draw into one
byte-per-pixel buffer** (`fb_chunked_buffer`, 320×200, low nibble =
palette index 0–15) and call **`fb_publish()` once per frame**. The
firmware does the chunky→planar conversion and a tear-free, VBL-synced
hand-off to the ST at 50 Hz. No m68k assembly, no bus timing, no
double-buffering. `fb_publish()` blocks on the ST's VBL, so one call per
loop paces the app to 50 Hz.

`README.md` is the human guide; `AGENTS.md` is the architecture
deep-dive; `examples/hello_text/` is a minimal working app.

## Where the code lives

- `rp/src/lynxemu.cpp` — the emulator front-end. Menu, SD→flash cartridge
  loader, display scaling, palette publish, input mapping, 60→50 Hz frame
  pacing, audio resampling. **Start here for anything app-level.**
- `rp/src/lynx/` — the vendored Handy core (C++, zlib licence). Change it
  only when the port needs it; keep the `ALTERED SOURCE VERSION` banner and
  tag changes `MD/Lynx:`.
- `rp/src/emul.c` — boot sequence and the main loop.
- `target/atarist/src/userfw.s` — the m68k side (per-VBL blit, palette,
  audio, IKBD). Only touch this if you need new m68k behaviour, and
  remember it forces a Docker `stcmd` build.
- `rp/src/main.c` — clocks and config bring-up only. **Don't add features
  here.**

`examples/hello_text/` and `tools/reset_template.sh` are leftovers from the
template and describe an app this repo no longer is — `reset_template.sh`
would delete the emulator, so don't run it.

## The API (keep these modules; they are your API)

```c
// pixels: fb_chunked.h
extern uint8_t fb_chunked_buffer[FB_CHUNKED_W * FB_CHUNKED_H];  // 320*200
fb_chunked_clear(color);                       // fill whole buffer
fb_chunked_plot(x, y, color);                  // bounds-checked pixel
// rects + sprites: fb_blit.h
fb_fill_rect(x, y, w, h, color);
fb_blit(&bitmap, x, y);                        // opaque (FB_BITMAP{w,h,data})
fb_blit_key(&bitmap, x, y, key);               // key index = transparent
// text: fb_font.h  (font8x8 defined in fb.c; no printf -- format numbers
// yourself, see fmt_uint/str_append in lynxemu.cpp). 8x8 cell = 40x25 chars.
font_set_font(&font8x8); font_set_color(0); font_align(FONT_ALIGN_LEFT);
font_move(x, y); font_print("TEXT");
// palette: palette.h  (16 entries, channels 0..7; idx 0 white, 15 black by default)
palette_set_entry(2, PALETTE_RGB(7, 0, 0));    // or palette_set(entries[16])
// publish: fb.h
fb_publish();                                  // once per frame, after drawing
// input: ikbd.h
ikbd_key_event_t k; while (ikbd_pop_key(&k)) { if (k.is_press) ... }  // k.scancode
ikbd_get_joystick();           // port 1: bit0 up,1 down,2 left,3 right,7 fire
ikbd_set_esc_auto_exit(false); // take ESC over (lynxemu does)
ikbd_request_boot_gem();       // exit to GEM on your own trigger
// audio: audio.h  (back-end picked at runtime from the m68k's _SND probe)
audio_set_fill_callback(cb);   // cb(buf,bytes) per VBL; bytes is 500 signed
                               // PCM for STE DMA, 224 for YM (vA,vB) pairs
audio_get_mode();              // AUDIO_MODE_DMA | _YM | _SILENT
audio_render_frame();          // call every loop iteration
```

**SD card:** the microSD is already mounted at boot
(`sdcard_initFilesystem()` in `emul_start()`). Read/write with standard
FatFs — `#include "ff.h"`, then `f_open` / `f_read` / `f_write` /
`f_close` on absolute paths. Helpers in `sdcard.h`: `sdcard_isMounted()`,
`sdcard_ensureFolder()`, `sdcard_dirExist()`. Keep file I/O **out of the
per-frame loop** (SPI reads cost ms and blow the ~19 ms budget) — load at
startup or stream small chunks like `audio.c` does for `.YMS`.

**Per-app config** (persistent settings, editable from Booster without
recompiling): defaults live in `rp/src/aconfig.c` `defaultEntries[]` as
`{"KEY", SETTINGS_TYPE_STRING|INT|BOOL, "default"}`, with the key macro in
`aconfig.h`. Read: `settings_find_entry(aconfig_getContext(), "KEY")` →
`->value` (always a string; `atoi()` for ints). Write:
`settings_put_string` / `_integer` / `_bool(aconfig_getContext(), "KEY",
v)` then `settings_save(aconfig_getContext(), true)`. (This is how the SD
folder name `ACONFIG_PARAM_FOLDER` is supplied.)

Main loop shape (in `emul_start()`):

```c
while (true) {
    ikbd_clear_command();                 // keep: makes an exit a one-shot
    fb_pump_rom3(); ikbd_pump();          // keep: input + VBL sync + _SND
    ikbd_key_event_t k; while (ikbd_pop_key(&k)) { lynxemu_handle_key(&k); }
    lynxemu_render_frame();               // draws + fb_publish: tear-free 50 Hz
    audio_render_frame();                 // keep: audio
}
```

## Critical constraints (get these wrong and the ST shows garbage)

- **RAM is the binding limit, and it is nearly all spoken for.** `.bss` +
  heap must not grow past `0x20030000` (the m68k cart-ROM mirror). The
  framebuffer buffers take ~96 KB and the emulator's heap ~70 KB more,
  leaving roughly 8 KB of slack — a large `static` array will not fit.
  Check `__bss_end__` .. `__StackLimit` in `rp/build-*/rp.elf.map` after
  any change that adds state. Buffers the ST never reads can be parked in
  the cart-region hole with `__cart_app_free("name")` (`cart_shared.h`),
  which has a few KB free. Big lookup tables / textures stay `const`
  (flash), never RAM.
- **Build:** `./build.sh <board> <build_type> <uuid>`. Build with the
  dev UUID `44444444-4444-4444-8444-444444444444` (the placeholder in
  AGENTS.md keys the app to the wrong identity → it jumps to Booster).
  The user usually runs the build themselves; only build the **Atari/m68k
  target** if you change `target/atarist/` asm. RP-only C changes don't
  need it.
- **Optimization:** the global build is `MinSizeRel` (`-Os`). For hot
  per-pixel loops add `#pragma GCC optimize("O3")` at the top of that
  *compute-only* file and `__not_in_flash_func()` on the function — never
  on bus/PIO/timing code, and watch the RAM cost: `C65C02::Update` is
  deliberately left in flash because its ~27 KB does not fit.
  `fb_core1_dispatch` / `fb_core1_wait` give you Core 1 for parallel work;
  each dispatch must be joined before the next.
- **Never** edit the `pico-sdk/`, `pico-extras/`, `fatfs-sdk/`
  submodules — the build re-pins them. Don't add features to `main.c`
  (use `emul.c`).
- **Palette:** index 0 also drives the ST border. `palette_set()` takes all 16 words; the port publishes STE-format words (4 bits per channel, LSB in the top bit of the nibble) so a plain ST reads the correct 3-bit approximation.
- **Default palette:** index 0 = white (text/border), 15 = black (background);
  `PALETTE_RGB(r,g,b)` channels are 0..7. Re-publishing the palette each
  frame is cheap (colour-cycling).
- **Commits:** no AI attribution / `Co-Authored-By` trailers.

## Verifying without hardware

You usually can't flash. What you can do offline: build (that catches the
RAM ceiling, which is the failure that actually bites here), read the
`.map` for the heap window, and validate addressing / fixed-point maths by
mirroring the exact code in a small script and diffing it against a
reference. Then the user flashes and confirms. In-game, the menu's
**TIMINGS** setting and the **ABOUT** page report per-frame emulation cost,
frame rate and the detected audio back-end.
