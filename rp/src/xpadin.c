/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: xpadin.c
 * Description: Reassembles the m68k's per-VBL Xpad controller report
 *              from the commemul ROM3 ring. See xpadin.h for the
 *              protocol and userfw.s for the producer.
 */

#include "xpadin.h"

#include "pico/stdlib.h"

/* The m68k reports every VBL (20 ms) while a pad is connected in slot 0.
 * Twelve frames of grace covers a dropped ring sample or a provider that
 * skips a refresh, while still noticing an unplugged pad within a
 * quarter of a second. */
#define XPADIN_STALE_US 250000u

static uint8_t s_hi_pending;
static bool s_hi_seen;
static uint16_t s_buttons;
static uint32_t s_last_us;
static bool s_have;

void xpadin_init(void) {
  s_hi_pending = 0;
  s_hi_seen = false;
  s_buttons = 0;
  s_last_us = 0;
  s_have = false;
}

void __not_in_flash_func(xpadin_consume_rom3_sample)(uint16_t addr_lsb) {
  const uint16_t window = addr_lsb & XPADIN_WINDOW_MASK;

  if (window == XPADIN_HI_LO16) {
    s_hi_pending = (uint8_t)addr_lsb;
    s_hi_seen = true;
    return;
  }
  if (window != XPADIN_LO_LO16) {
    return;
  }
  /* A low byte with no high byte ahead of it means the ring lost the
   * first half of the pair. Drop the frame rather than combine it with
   * a stale high byte -- holding the previous mask for one more frame
   * is a far smaller lie than a button that never was. */
  if (!s_hi_seen) {
    return;
  }
  s_buttons = (uint16_t)(((uint16_t)s_hi_pending << 8) | (addr_lsb & 0xFFu));
  s_hi_seen = false;
  s_last_us = time_us_32();
  s_have = true;
}

bool xpadin_get(uint16_t *buttons) {
  if (!s_have) {
    return false;
  }
  if (time_us_32() - s_last_us > XPADIN_STALE_US) {
    s_have = false;
    return false;
  }
  if (buttons != NULL) {
    *buttons = s_buttons;
  }
  return true;
}
