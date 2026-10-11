/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: fb.c
 * Description: Framebuffer module — owns `fb_screen` and brings up the
 *              single 32 KB cartridge framebuffer the m68k reads each
 *              VBL (single-FB design).
 *
 * The framebuffer base is derived from the linker symbol
 * `__rom_in_ram_start__` plus `CART_FRAMEBUFFER_OFFSET`, so the
 * layout stays the single source of truth — apps must never hard-code
 * the address.
 */

#include "fb.h"

#include <stdbool.h>
#include <string.h>

#include "audio.h"
#include "cart_shared.h"
#include "commemul.h"
#include "debug.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "font8x8.h"            /* defines `font8x8` (FB_FONT instance) */
#include "ikbd.h"
#include "xpadin.h"
#include "pico/time.h"          /* time_us_32 for the timing overlay */

/* VBL frame-sync. The m68k does a cart-bus read at
 * $FB8400 after each blit (see VBLSYNC_ADDR in userfw.s); the
 * commemul ring captures it with low-16 = 0x84xx. fb_pump_rom3
 * routes ROM3 samples to both the IKBD demux and this detector;
 * fb_publish() blocks until s_vbl_seen advances before overwriting
 * the cart FB. The ~33 ms timeout keeps the RP from hanging if the
 * m68k isn't emitting acks (e.g. before it boots). */
#define FB_VBLSYNC_HIBYTE   0x8400u
/* Every ROM3 window is a 256-byte page, so the high byte is the
 * discriminator and the low byte the payload. */
#define FB_WINDOW_HIMASK    0xFF00u
/* Safety net only: must comfortably exceed the worst-case latency from
 * "counter bumped" to "m68k VBLSYNC" (up to ~2 VBLs = 40 ms when c2p
 * overruns the slack and the m68k skips a frame). Firing early would
 * let the next c2p race the blit -- so keep it generous; it should
 * never fire while the m68k is actually running. */
#define FB_VSYNC_TIMEOUT_US 60000u

static volatile uint32_t s_vbl_seen;
static uint32_t s_vbl_published;

/* Boot splash, painted once by fb_init (defined below). */
static void fb_render_frame(void);

/* Frames published so far; also the value written to the cart-side
 * dirty-frame counter the m68k VBL loop compares against. */
static uint32_t fb_frame_tick = 0;

const struct FB_MODE fb_mode_320x200 = {320, 200, 4};

struct FB_SCREEN fb_screen;

/* RP-incremented dirty-frame counter at $FA400C. The m68k userfw reads
 * this each VBL and only blits cart->ST screen when the value differs
 * from what it saw last iteration. Set in fb_init, bumped at the end of
 * fb_render_frame after all FB writes commit. */
static volatile uint32_t *fb_frame_counter;

int fb_init(const struct FB_MODE *mode) {
  if (mode == NULL) {
    return -1;
  }
  fb_screen.framebuffer =
      (unsigned int *)((uintptr_t)&__rom_in_ram_start__ +
                       CART_FRAMEBUFFER_OFFSET);
  fb_screen.width = mode->h_pixels;
  fb_screen.height = mode->v_pixels;
  fb_screen.color_bits = mode->color_bits;
  fb_frame_counter =
      (volatile uint32_t *)((uint8_t *)&__rom_in_ram_start__ +
                            CART_FB_FRAME_COUNTER_OFFSET);

  fb_frame_tick = 0;

  /* Zero the dirty-frame counter so the m68k userfw VBL loop sees a
   * clean baseline. The full shared region is already zeroed by
   * emul_start's ERASE_FIRMWARE_IN_RAM, so this is defensive against
   * future callers that might re-init fb without erasing. (Used to
   * live in chandler_init; relocated when chandler was removed.) */
  *fb_frame_counter = 0;

  /* Launch Core 1 with the chunky-to-planar bottom-half worker. */
  fb_chunked_init();

  /* Paint the boot splash + run the conversion once so the cart
   * framebuffer is in a deterministic state before the main loop
   * spins up. */
  fb_render_frame();

  DPRINTF("FB: %dx%d, %d bpp at %p (size=%u)\n", fb_screen.width,
          fb_screen.height, fb_screen.color_bits, fb_screen.framebuffer,
          (unsigned int)CART_FRAMEBUFFER_SIZE);
  return 0;
}

/* Boot splash rendered via the chunked path.
 *
 * fb_font's render_text writes directly into fb_chunked_buffer (one byte
 * per pixel, palette index in low nibble); fb_chunky_to_planar publishes
 * the whole buffer into the cart FB. The emulator front-end (lynxemu)
 * owns every frame after boot -- these two entry points only paint the
 * one splash frame fb_init needs so the ST has something deterministic
 * to blit before the games list is up. */

static void fb_render_static(void) {
  font_set_font(&font8x8);
  font_set_color(0);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_CENTER);

  font_move(160, 88);
  font_print("MD/LYNX");
  font_move(160, 104);
  font_print("STARTING UP...");
}

static void fb_render_frame(void) {
  /* Black background (palette index 15 on the default TOS low-res
   * palette; lynxemu replaces the palette once a game is running). */
  fb_chunked_clear(15);
  fb_render_static();
  fb_publish();
}

/* ROM3 ring dispatch: route each captured cart-bus read to the IKBD
 * demux, the Xpad report receiver, the VBL frame-sync detector and the
 * sound-capability decoder. Each looks only at its own window. */
static void fb_rom3_dispatch(uint16_t sample) {
  ikbd_consume_rom3_sample(sample);
  xpadin_consume_rom3_sample(sample);
  audio_consume_rom3_sample(sample);
  if ((sample & FB_WINDOW_HIMASK) == FB_VBLSYNC_HIBYTE) {
    s_vbl_seen++;
  }
}

void fb_pump_rom3(void) { commemul_poll(fb_rom3_dispatch); }

static void fb_publish_common(bool row_doubled) {
  /* 1. Transpose chunked -> planar SCRATCH (RP RAM, dual-core). This is
   *    the slow part (~1 ms) but it does NOT touch the cart FB, so it
   *    runs unsynchronized and overlaps the m68k's blit of the previous
   *    frame. */
  if (row_doubled) {
    fb_transpose_row_doubled();
  } else {
    fb_transpose();
  }

  /* 2. Block until the m68k has finished blitting the previous frame
   *    (its VBLSYNC ack) so the cart FB is free to overwrite. Drain the
   *    ROM3 ring meanwhile so IKBD / ESC stay live; the timeout is a
   *    safety net for "m68k not running" (e.g. at boot). */
  uint32_t t_wait = time_us_32();
  while (s_vbl_seen == s_vbl_published) {
    fb_pump_rom3();
    if (time_us_32() - t_wait > FB_VSYNC_TIMEOUT_US) {
      break;
    }
  }
  s_vbl_published = s_vbl_seen;

  /* 3. Publish: fast chunk-reversed memcpy scratch -> cart FB. This is
   *    the only cart-FB write and it's short (~120 us), so it fits in
   *    the m68k's post-blit slack -- no overrun, no tearing, full 50 Hz.
   *    Mark the frame ready as the last write (barrier first). */
  fb_planar_publish((uint16_t *)fb_screen.framebuffer);

  fb_frame_tick++;
  __sync_synchronize();
  *fb_frame_counter = fb_frame_tick;
}

void fb_publish(void) { fb_publish_common(false); }

void fb_publish_row_doubled(void) { fb_publish_common(true); }
