# MD/Lynx

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://neilrackett.com/atarist)

## Introduction

MD/Lynx turns your SidecarT into an Atari Lynx running on your Atari ST!

The Lynx and the ST both show 16 colours at once, so the Lynx palette goes straight to the ST shifter — nothing is quantised, and on an STE you get all 4,096 Lynx colours. The 160x102 Lynx screen is doubled to 320x204 with the top and bottom scanlines cropped, so it fills the ST display exactly.

Sound comes out of the STE's DMA chip at 25 kHz where the machine has it, and falls back to the YM2149 on a plain ST. It's detected at boot, there's nothing to set up.

Any cartridge image in `.lnx`, `.lyx`, `.o` or `.bin` format can be played using the Atari ST keyboard, a joystick, or a modern gamepad via [Xpad](https://github.com/neilrackett/atarist-xpad), just drop them into the `/lynx` folder of your SD card and they'll appear on the menu. No Lynx BIOS ROM is needed. There's a growing library of homebrew titles at [AtariAge](https://atariage.com/software_list.php?SystemID=LYNX&currentPage=1&maxPages=1) to get you started; commercial titles you'll need to dump from your own cartridges, although you may be able to find ready made backups in the [Internet Archive](https://archive.org/details/atari-lynx-roms). (Games flagged as rotated, like Gauntlet — The Third Encounter, play upright rather than sideways.)

Ported from K. Wilkins' [Handy](https://sourceforge.net/projects/handy/), via xrip's [pico-lynx](https://github.com/xrip/pico-lynx), so a massive thank you to both of them for their fantastic work!

## Controls

The Lynx pad is four directions plus **A**, **B**, **Option 1**, **Option 2**
and **Pause**. The ST cursor cluster and a joystick in port 1 both drive the
d-pad, and the two fire buttons sit under the keys nearest the cursors.

| Context | Key                                | Action                        |
| ------- | ---------------------------------- | ----------------------------- |
| Play    | ↑ ↓ ← →                            | D-pad                         |
|         | Alternate / Space / Insert         | Button A                      |
|         | Control / Shift / Clr Home         | Button B                      |
|         | F1 / F2                            | Option 1 / Option 2           |
|         | F3 or P                            | Pause                         |
|         | Esc                                | Open the menu                 |
| Menu    | ↑ ↓                                | Choose a cartridge or setting |
|         | ← →                                | Change the selected setting   |
|         | Return / Space / Insert / Clr Home | Select                        |
|         | Esc                                | Close the menu                |

Pause is on **F3** as well as **P**, because several titles want it held together with an Option button — screen flip for left-handed play being the familiar one — and F1/F2/F3 fall under one hand where F1+P doesn't.

A real ST joystick works in port 1, in both play and the menu — the stick is the d-pad and fire is Button A. It's only one button, though, so Option 1, Option 2 and Pause stay on the keyboard.

### Gamepads, via Xpad

For anything with more than one button, MD/Lynx reads [Xpad](https://github.com/neilrackett/atarist-xpad) — the open standard where a provider publishes controller state and installs the `XPAD` cookie, and software like this polls it. When a provider is running, pad 0 maps like this:

| Gamepad                     | Lynx                    |
| --------------------------- | ----------------------- |
| D-pad or left stick         | D-pad                   |
| South (A on an Xbox pad)    | Button A                |
| East (B)                    | Button B                |
| West (X) or left shoulder   | Option 1                |
| North (Y) or right shoulder | Option 2                |
| Start                       | Pause                   |
| Select / Back, or Guide     | Open and close the menu |

In the menu, the d-pad moves and adjusts and South or Start selects. Providers fold the left stick into the d-pad bits, so an analogue stick works without MD/Lynx doing anything special.

Xpad is an enhancement, never a requirement: with no provider present, the joystick and keyboard behave exactly as above. One caveat worth knowing — MD/Lynx boots from the cartridge _before_ the AUTO folder runs, so a provider installed as a TSR won't be resident yet. It has to come from something already present when the cartridge takes over.

The menu is a box in the middle of the screen, laid out like a MiSTer's on-screen display with the title running up a bar down its left-hand side; with a cartridge running it sits over the game, which carries on playing around it. Its two colours are borrowed from the cartridge's own palette — the shifter has only sixteen and they belong to the game — so while the menu is open the picture loses the distinction between two of its colours. They're chosen as the closest pair going, so it's usually not noticeable.

The settings sit above the cartridge list: **volume**, **size**, **speed**, a **timings** overlay, an **about** item (version, credits and the detected sound chip) and an **exit** item to quit back to GEM.

**size** is `2X FULL` (the default), which fills the ST screen, or `1X CENTRED`, which shows the Lynx's 160x102 pixel-for-pixel in the middle of the screen with a border. 1x is smaller but exact — no doubling.

**speed** is the one worth knowing about. The Lynx runs at 60 Hz and the ST's screen at 50, so `60HZ TRUE` emulates six frames for every five it shows — game speed and sound pitch stay right, at the cost of one repeated frame every tenth of a second. `50HZ SMOOTH` shows every frame instead, which is perfectly even but runs everything about 17% slow. Take your pick.

Loading a cartridge writes it into the SidecarT's flash, so it takes a couple of seconds; the screen holds on `LOADING` until it's done. A cartridge with a save EEPROM gets a `.eep` file alongside it on the microSD card.

## Installation

1. Download the latest `.uf2` and `.json` from the [releases page](https://github.com/neilrackett/md-lynx/releases).
2. Copy both files to the `/apps` folder of your SidecarT's microSD card.
3. Copy your Lynx cartridge images into a `/lynx` folder on the same microSD card.
4. On the Booster screen, press ESC for the app list and select MD/Lynx.
5. To return to Booster, power on your ST while holding the SELECT button on your SidecarT.

## Hardware requirements

- [SidecarTridge Multi-device](https://sidecartridge.com) (RP2040-based ROM cartridge emulator)
- Atari ST, STE, MegaST, or MegaSTE (low or medium resolution — high-res falls back to GEM). An STE-class machine gets much better sound.
- A microSD card for your cartridge images
- Raspberry Pi Debug Probe or Picoprobe for flashing/debugging (optional, for development)

## How it works

The RP2040 runs the whole Lynx on Core 0, with Mikey drawing 2x-scaled pixels straight into the framebuffer; the m68k blits that to the ST screen every VBL. Cartridge images are far too big for RAM, so the one you pick is programmed into the RP2040's flash and read in place. Input, sound and the palette ride the cartridge bus in both directions.

```
IKBD keys + joystick       ──$FB82xx──►  demux → Lynx pad
Xpad gamepad buttons       ──$FB88xx──►  hi byte, then lo at $FB8Axx
Sound chip report (_SND)   ──$FB86xx──►  picks STE DMA or YM2149
                                         Core 1: chunky → ST planar (c2p)
Screen: blit + page flip   ◄──$FA8300──  Mikey, 2x-scaled to 320x204
Palette: 16 shifter words  ◄──$FA4040──  Lynx palette, 1:1
Sound: DMA refill or YM    ◄──$FA4100──  Lynx audio, resampled per VBL
```

MD/Lynx runs almost entirely on the SidecarT, with the ST providing the screen, keyboard and sound.

## Building

Clone with `--recursive`, or run `git submodule update --init --recursive`
before the first build — the Xpad ABI comes from `lib/xpad` and both halves of
the build need it.

```bash
# Production build (pico_w) — uses uuid.txt / APP_UUID_KEY
make build

# Debug build (bumps the patch version)
make debug

# Open a UART console on the debug probe
make uart
```

RAM is the binding constraint here — the emulated Lynx's own 64K has to share the RP2040's 192K with the framebuffer path — so check the `__bss_end__` .. `__StackLimit` window in `rp/build-*/rp.elf.map` after anything that adds state. `AGENTS.md` has the full architecture notes.

For more on coding for the SidecarT, [the docs are here](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/).

## License

Source code is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for the full text. The vendored emulator core (`rp/src/lynx/`) retains its original zlib licence, and the files altered for this port are marked as such, as that licence requires. [Xpad](https://github.com/neilrackett/atarist-xpad), included as the `lib/xpad` submodule, is BSD-2-Clause.

No cartridge images are included or distributed with this project. Atari and Lynx are trademarks of their respective owners.
