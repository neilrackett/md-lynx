/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: emul.c
 * Description: Boot path + main loop for the Atari Lynx port. Brings up
 *              the cartridge bus emulator, the 320x200 framebuffer, the
 *              ROM3 cart-bus capture ring, the SD card and the audio
 *              buffer, then hands off to the emulator front-end
 *              (lynxemu.cpp): drain IKBD -> feed keys -> render one
 *              frame per VBL -> refill the audio buffer.
 */

#include "emul.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "aconfig.h"
#include "audio.h"
#include "cart_shared.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "fb.h"
#include "ff.h"
#include "ikbd.h"
#include "lynxemu.h"
#include "memfunc.h"
#include "palette.h"
#include "pico/stdlib.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "settings/settings.h"
#include "target_firmware.h"
#include "xpadin.h"

/* Cart-corruption canary: the first 16 bytes of the served cart region
 * (the m68k cartridge header + entry) must stay byte-identical to the
 * embedded image after COPY_FIRMWARE_TO_RAM. Any change means something
 * on the RP wrote into the region the m68k is executing from -- which
 * makes the ST bomb or freeze on the boot frame, with no clue on the RP
 * side that anything is wrong. Logged (debug builds only) with a stage
 * tag so the culprit can be localised over UART. */
static bool cart_check(const char *stage) {
  static bool ok = true;
  if (ok &&
      memcmp((const void *)&__rom_in_ram_start__, target_firmware, 16) != 0) {
    ok = false;
    const volatile uint16_t *c =
        (const volatile uint16_t *)&__rom_in_ram_start__;
    DPRINTF("CART REGION CORRUPTED (%s): %04X %04X %04X %04X\n", stage, c[0],
            c[1], c[2], c[3]);
  }
  return ok;
}

void emul_start() {
  // Cartridges live in the app folder (per-app config
  // ACONFIG_PARAM_FOLDER, default "/lynx" from aconfig.c).
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  const char *folderName = folder ? folder->value : "/lynx";

  // The .cart_app_free buffers live inside the shared region's unused
  // hole. The linker script hard-codes that window (ld cannot read C
  // headers), so verify it here against cart_shared.h's authoritative
  // offsets -- a drifted script would let the ST-visible layout and the
  // parked buffers collide.
  {
    extern char __cart_app_free_start__[], __cart_app_free_end__[];
    const char *base = (const char *)&__rom_in_ram_start__;
    if (__cart_app_free_start__ < base + CART_APP_FREE_OFFSET ||
        __cart_app_free_end__ > base + CART_FRAMEBUFFER_OFFSET) {
      panic(".cart_app_free outside the shared-region hole");
    }
  }

  // RP2040 RAM is undefined at power-on and firmware.py only emits the
  // bytes up to the last non-zero in BOOT.BIN, so zero the whole 64 KB
  // shared region first -- every byte the m68k can see must be
  // deterministic -- then copy the cartridge image into it.
  ERASE_FIRMWARE_IN_RAM();
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);
  cart_check("post-copy");

  // Controller-input receivers before commemul: both are fed by the ROM3
  // dispatcher from fb_init onward, and their producers run on the main
  // loop rather than from an IRQ.
  ikbd_init();
  xpadin_init();

  // Cartridge ROM4 read engine (served by chained DMA -> PIO, no CPU).
  if (init_romemul(false) < 0) {
    panic("init_romemul failed");
  }

  // ROM3 cart-bus capture ring (PIO + DMA) BEFORE fb_init, whose first
  // fb_publish() drains it while waiting for the m68k VBL ack.
  if (commemul_init() < 0) {
    panic("commemul_init failed");
  }
  cart_check("post-commemul");

  // 320x200 4bpp framebuffer + fb_screen for the draw primitives.
  // (This launches Core 1 for the chunky->planar worker.)
  if (fb_init(&fb_mode_320x200) < 0) {
    panic("fb_init failed");
  }
  DPRINTF("fb_init OK\n");
  cart_check("post-fb");

  // Default palette; lynxemu_init() replaces it with the menu palette,
  // and each running cartridge with its own 16 Lynx colours.
  palette_init();

  // Cart audio buffer producer. The back-end (STE DMA or YM2149) is
  // reported by the m68k over the cart bus; audio.c decodes that report
  // from the ROM3 ring and picks the matching sample format.
  audio_init();

  // SD card -- best effort. Cartridges live in `folderName`.
  FATFS fsys;
  if (sdcard_initFilesystem(&fsys, folderName) != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable. Continuing without SD.\n");
  }
  cart_check("post-sd");

  // Cartridge SELECT button -- apps can poll select_isPressed().
  select_configure();

  // Bring up the emulator front-end: palette, cartridge list, menu.
  // It takes ownership of the ESC key (ESC toggles the menu; leaving to
  // GEM is the menu's own Exit item).
  DPRINTF("lynxemu_init...\n");
  lynxemu_init();
  cart_check("post-lynxemu-init");
  audio_set_fill_callback(lynxemu_audio_fill);

  // Main loop:
  //   0. Re-arm the command sentinel to NOP (makes an exit's BOOT_GEM a
  //      one-shot so it doesn't re-trigger on the next ST reset).
  //   1. Drain the ROM3 ring -> IKBD demux + VBL frame-sync + the
  //      m68k's sound-capability report.
  //   2. Run the IKBD demux.
  //   3. Forward decoded key events to the emulator front-end.
  //   4. Render one frame into the framebuffer (blocks on the VBL).
  //   5. Refill the cart audio buffer.
  DPRINTF("Entering main loop\n");
  while (true) {
    ikbd_clear_command();
    fb_pump_rom3();
    ikbd_pump();

    ikbd_key_event_t k;
    while (ikbd_pop_key(&k)) {
      lynxemu_handle_key(&k);
    }

    lynxemu_render_frame();
    audio_render_frame();
    cart_check("main-loop");
  }
}
