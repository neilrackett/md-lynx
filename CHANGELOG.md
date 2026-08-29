# Changelog

## Unreleased

First release of MD/Lynx — an Atari Lynx emulator for the Atari ST / STE /
MegaST(E), built on md-framebuffer-template v1.0.0beta.

- Handy emulation core (K. Wilkins, zlib) vendored from xrip/pico-lynx under
  `rp/src/lynx/`, kept in C++ with its licence text.
- 320×200 display: the Lynx's 160×102 screen doubled to 320×204 with the top
  and bottom scanlines cropped. Mikey renders 4-bit pen indices straight into
  the chunked framebuffer and the Lynx palette is published to the ST/STE
  shifter each frame, so nothing is colour-quantised.
- STE/MegaSTE DMA sound at 25 kHz with a YM2149 fallback on a plain ST,
  detected at boot from the `_SND` cookie and reported to the RP over the
  cart bus.
- Interrupt-driven IKBD: ST keyboard and port-1 joystick.
- Xpad ABI comes from the `lib/xpad` submodule rather than a vendored copy, so
  the header the RP compiles against and the equates the m68k assembles
  against are the same pinned revision.
- Xpad consumer, so a gamepad with more than one button can reach the Lynx's
  Option 1, Option 2 and Pause. The m68k locates and validates the published
  block at boot and forwards pad 0's buttons each VBL; falls back to the IKBD
  joystick when no provider is present.
- Cartridge menu with a games list from the SD `lynx` folder, volume, a
  60 Hz / 50 Hz frame-pacing choice, a timings overlay, an about page and an
  exit to GEM.
- Cartridge images up to 512 KB are programmed into the `ROM_TEMP` flash
  window at load time; EEPROM saves are written back next to the image.
- Core 0 runs at 400 MHz (cart-bus PIO timing preserved by a clock divider).

## v1.0.0beta (2026-06-04)

First release of md-framebuffer-template.
