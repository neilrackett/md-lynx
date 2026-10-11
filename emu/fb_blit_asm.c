/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: fb_blit_asm.c
 * Description: rp/src/fb_blit_asm.S for EmuMD, in C: one row of a
 *              colour-keyed blit, copying `cw` bytes and skipping those
 *              equal to `key`.
 */

#include <stdint.h>

void fb_blit_key_row(uint8_t *dst, const uint8_t *src, uint32_t cw,
                     uint8_t key) {
  for (uint32_t i = 0; i < cw; i++) {
    if (src[i] != key) dst[i] = src[i];
  }
}
