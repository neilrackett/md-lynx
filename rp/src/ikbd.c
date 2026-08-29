/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: ikbd.c
 * Description: IKBD keyboard + joystick ingest + demux.
 *
 * Raw byte ring filled by ikbd_consume_rom3_sample (called directly
 * from `commemul_poll(ikbd_consume_rom3_sample)` in the emul.c main
 * loop on each commemul ROM3 sample) and drained by ikbd_pump on
 * the same loop. Single-thread access
 * everywhere outside the IRQ-free ingest path -- `volatile` is for
 * compiler hygiene only.
 *
 * Demux is a stateless byte classifier: each byte processed
 * independently in IDLE-equivalent context. No multi-byte collect
 * state -- mouse and joystick are disabled at boot ($12 / $1A), so
 * packet headers ($F2-$FF) shouldn't arrive in steady state. If
 * any leak through (boot self-test window, etc.), they're discarded
 * as single bytes; their follow bytes may emit one-shot spurious
 * key events but the demux never sticks. Avoiding the state
 * machine eliminates the "stuck in D_COLLECT after losing the last
 * follow byte" failure mode that broke keyboard input in an
 * earlier attempt.
 */

#include "ikbd.h"

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "pico/stdlib.h"
#include "pico/time.h"

/* IKBD scancode for the ESC key. ESC press+release within
 * IKBD_ESC_RELEASE_TIMEOUT_US triggers CMD_BOOT_GEM via the cart
 * command sentinel. Single stray $01 bytes don't reliably pair with
 * an $81 release within the window, so they won't false-trigger. */
#define IKBD_SCANCODE_ESC 0x01u
#define IKBD_ESC_RELEASE_TIMEOUT_US 200000u  /* 200 ms */

#define IKBD_RING_MASK (IKBD_RING_CAPACITY - 1u)

#if (IKBD_RING_CAPACITY & IKBD_RING_MASK) != 0
#error "IKBD_RING_CAPACITY must be a power of two"
#endif

/* Raw-byte ring (producer = ikbd_consume_rom3_sample, consumer =
 * ikbd_pump). */
static volatile uint8_t  s_ring[IKBD_RING_CAPACITY];
static volatile uint8_t  s_head = 0;
static volatile uint8_t  s_tail = 0;
static volatile uint32_t s_dropped = 0;

/* Decoded key event ring. 16 entries; main-loop producer/consumer
 * (no IRQ-safety needed). */
#define IKBD_KEY_RING_SIZE 16u
#define IKBD_KEY_RING_MASK (IKBD_KEY_RING_SIZE - 1u)

static ikbd_key_event_t s_key_ring[IKBD_KEY_RING_SIZE];
static uint8_t          s_key_head = 0;
static uint8_t          s_key_tail = 0;

/* ESC press+release timestamp (microseconds, 0 = no press pending). */
static uint32_t s_esc_press_us = 0;

/* When true (default), ESC press+release pairs write CMD_BOOT_GEM to
 * the cart sentinel and userfw exits to GEM. Apps that want to own
 * the ESC key (e.g. menu+demo dispatcher) clear this via
 * ikbd_set_esc_auto_exit(false) -- ESC events are still delivered
 * through ikbd_pop_key, the auto-write is just gated off. */
static bool s_esc_auto_exit = true;

void __not_in_flash_func(ikbd_consume_rom3_sample)(uint16_t addr_lsb) {
  if ((addr_lsb & IKBD_WINDOW_MASK) == IKBD_WINDOW_LO16) {
    uint8_t byte = (uint8_t)(addr_lsb & 0xFFu);
    uint8_t next_head = (uint8_t)((s_head + 1u) & IKBD_RING_MASK);
    if (next_head != s_tail) {
      s_ring[s_head] = byte;
      s_head = next_head;
    } else {
      s_dropped++;
    }
  }
}

void ikbd_init(void) {
  s_head = 0;
  s_tail = 0;
  s_dropped = 0;
  s_key_head = 0;
  s_key_tail = 0;
  s_esc_press_us = 0;
  s_esc_auto_exit = true;
}

void ikbd_set_esc_auto_exit(bool enabled) {
  s_esc_auto_exit = enabled;
}

void ikbd_request_boot_gem(void) {
  /* Pre-swap the longword halves so m68k's move.l sees the expected
   * value (cart_asM68kLong). userfw's VBL loop polls this sentinel and
   * exits to GEM on match. */
  *((volatile uint32_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CART_CMD_SENTINEL_OFFSET)) =
      cart_asM68kLong(CART_CMD_BOOT_GEM);
}

void ikbd_clear_command(void) {
  /* Re-arm the command sentinel to NOP. ikbd_request_boot_gem() posts
   * BOOT_GEM here for userfw to consume, but the m68k can't write it
   * back (the cart region is RP-owned) and the RP only zeroes it at
   * boot. Left set, a BOOT_GEM from an exit persists across an ST reset:
   * the re-run userfw re-reads it and quits to GEM instead of running.
   * Called once per main-loop iteration -- BOOT_GEM survives the single
   * frame between the exit write and userfw's VBL read (fb_publish
   * blocks until that read), then this wipes it. */
  *((volatile uint32_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CART_CMD_SENTINEL_OFFSET)) =
      cart_asM68kLong(CART_CMD_NOP);
}

size_t ikbd_ring_count(void) {
  uint8_t h = s_head;
  uint8_t t = s_tail;
  return (size_t)((h - t) & IKBD_RING_MASK);
}

uint32_t ikbd_ring_dropped(void) { return s_dropped; }

/* Pop one byte from the raw ring. Returns false if empty. Internal
 * helper for ikbd_pump. */
static bool raw_pop(uint8_t *out) {
  uint8_t h = s_head;
  uint8_t t = s_tail;
  if (h == t) return false;
  *out = s_ring[t];
  s_tail = (uint8_t)((t + 1u) & IKBD_RING_MASK);
  return true;
}

static void push_key(uint8_t scancode, bool is_press) {
  /* Scancode 0 is not a valid IKBD key. Stray $00 bytes in the
   * stream get classified as "KEY $00" by the demux; suppress them
   * so apps don't see spurious events. Also covers $80 (which
   * decodes as "release of scancode 0"). */
  if (scancode == 0u) return;

  /* ESC press+release pair → trigger exit via the cart command
   * sentinel. Pre-swap the longword halves so m68k's move.l sees
   * the expected value (cart_asM68kLong). */
  if (scancode == IKBD_SCANCODE_ESC) {
    if (is_press) {
      uint32_t now_us = time_us_32();
      if (now_us == 0u) now_us = 1u;  /* avoid the "no press" sentinel */
      s_esc_press_us = now_us;
    } else {
      uint32_t press = s_esc_press_us;
      s_esc_press_us = 0;
      if (s_esc_auto_exit && press != 0u &&
          (time_us_32() - press) < IKBD_ESC_RELEASE_TIMEOUT_US) {
        ikbd_request_boot_gem();
      }
    }
  }

  uint8_t next = (uint8_t)((s_key_head + 1u) & IKBD_KEY_RING_MASK);
  if (next == s_key_tail) return;  /* ring full, drop event */
  s_key_ring[s_key_head].scancode = scancode;
  s_key_ring[s_key_head].is_press = is_press;
  s_key_head = next;
}

/* Joystick support. The m68k IKBD handler forwards
 * every byte interrupt-driven, so the multi-byte joystick packets
 * arrive intact and this demux frames them into s_joy_state[].
 *
 * The two ports are kept separate and only port 1 is reported: with
 * joystick event reporting on ($14), the mouse in port 0 reports as
 * joystick 0, and a stationary mouse holds its quadrature lines in a
 * fixed pattern -- a steady non-zero direction byte that, being
 * change-triggered, latches forever. Folding both ports into one
 * state let that phantom overwrite the real stick in port 1 (games
 * waiting for the stick to centre would never start). */

/* Latest joystick state per port (bit0 up, bit1 down, bit2 left,
 * bit3 right, bit7 fire). [0] = port 0 (mouse), [1] = port 1. */
static volatile uint8_t s_joy_state[2] = {0, 0};
/* Which state bytes are still expected after a packet header, as a
 * bit mask: bit0 = a port-0 byte, bit1 = a port-1 byte. $FD sends
 * port 0 then port 1, so its first follow byte is the port-0 one. */
static uint8_t s_joy_pending = 0;

uint8_t ikbd_get_joystick(void) { return s_joy_state[1]; }

void ikbd_pump(void) {
  uint8_t b;
  while (raw_pop(&b)) {
    /* Consume the state byte(s) that follow a joystick packet header so
     * they are never misclassified as key scancodes. Mask to the
     * meaningful bits (directions + fire) so a stray byte swallowed
     * here can't inject phantom bits that persist forever. */
    if (s_joy_pending) {
      if (s_joy_pending & 1u) {
        s_joy_state[0] = b & 0x8Fu;
        s_joy_pending &= (uint8_t)~1u;
      } else {
        s_joy_state[1] = b & 0x8Fu;
        s_joy_pending = 0;
      }
      continue;
    }
    if (b == 0xFEu) { s_joy_pending = 1; continue; }  /* joystick 0 */
    if (b == 0xFFu) { s_joy_pending = 2; continue; }  /* joystick 1 */
    if (b == 0xFDu) { s_joy_pending = 3; continue; }  /* both, 0 then 1 */
    if (b < 0x80u) {
      push_key(b, true);
    } else if (b < 0xF2u) {
      /* $80..$F1 → key release of scancode (b & $7F). */
      push_key((uint8_t)(b & 0x7Fu), false);
    }
    /* $F2..$FF: mouse / joystick / status / TOD packet headers.
     * Mouse is disabled at boot; joystick headers are handled above
     * when the port is built. Any other header byte is discarded. */
  }
}

bool ikbd_pop_key(ikbd_key_event_t *out) {
  if (s_key_head == s_key_tail) return false;
  if (out) *out = s_key_ring[s_key_tail];
  s_key_tail = (uint8_t)((s_key_tail + 1u) & IKBD_KEY_RING_MASK);
  return true;
}
