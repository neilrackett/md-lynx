/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: fb_chunked.c
 * Description: rp/src/fb_chunked.c for EmuMD. The chunked buffer, its
 *              guard bands and the Core 1 job server are the same, but a
 *              job and its argument are 64-bit pointers on the host, so
 *              they cannot travel through the 32-bit inter-core FIFO as
 *              they do on the RP2040: they wait in a slot, and the FIFO
 *              only says when. Parking Core 1 has nothing to protect here
 *              (flash is always readable), the transposes run on one core
 *              (the emulated Multi-device has time to spare), and
 *              fb_chunked_asm.S's two Thumb workers are plain C.
 */

#include "fb_chunked.h"

#include <string.h>

#include "cart_shared.h"
#include "pico/multicore.h"

struct FB_CHUNKED_STORE fb_chunked_store __attribute__((aligned(4)));

#define FB_ROW_SRC_BYTES FB_CHUNKED_W
#define FB_ROW_PLANAR_BYTES (FB_CHUNKED_W / 2)

static uint16_t fb_planar_scratch[CART_FRAMEBUFFER_SIZE / sizeof(uint16_t)]
    __attribute__((aligned(4)));

/* fb_chunked_asm.S's fb_c2p_half: pixels [src, src_end) as 16-pixel
 * blocks of four plane words, the leftmost pixel in bit 15, from the low
 * nibble of each byte. */
static void fb_c2p_half(uint16_t *dst, const uint8_t *src,
                        const uint8_t *src_end) {
  for (; src < src_end; src += 16, dst += 4) {
    uint16_t planes[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < 16; i++) {
      for (unsigned k = 0; k < 4; k++) {
        planes[k] |= (uint16_t)(((src[i] >> k) & 1u) << (15u - i));
      }
    }
    memcpy(dst, planes, sizeof(planes));
  }
}

/* One job at a time, as on the RP2040: dispatch and wait always pair. */
static fb_core1_job_t s_job;
static void *s_job_arg;

static void fb_core1_loop(void) {
  for (;;) {
    (void)multicore_fifo_pop_blocking();
    s_job(s_job_arg);
    multicore_fifo_push_blocking(0); /* done */
  }
}

void fb_core1_dispatch(fb_core1_job_t job, void *arg) {
  s_job = job;
  s_job_arg = arg;
  multicore_fifo_push_blocking(1); /* the FIFO's lock orders the slot */
}

void fb_core1_wait(void) { (void)multicore_fifo_pop_blocking(); }

void fb_chunked_init(void) { multicore_launch_core1(fb_core1_loop); }

void fb_core1_park(void) {}
void fb_core1_unpark(void) {}

void fb_chunked_clear(uint8_t color) {
  memset(fb_chunked_buffer, color, FB_CHUNKED_SIZE);
}

void fb_transpose(void) {
  fb_c2p_half(fb_planar_scratch, fb_chunked_buffer,
              fb_chunked_buffer + FB_CHUNKED_SIZE);
}

/* Only the even chunked rows, each planar result copied into the row
 * below it. */
void fb_transpose_row_doubled(void) {
  uint8_t *planar = (uint8_t *)fb_planar_scratch;
  const uint8_t *src = fb_chunked_buffer;
  for (unsigned k = 0; k < FB_CHUNKED_H / 2u; k++) {
    fb_c2p_half((uint16_t *)planar, src, src + FB_ROW_SRC_BYTES);
    memcpy(planar + FB_ROW_PLANAR_BYTES, planar, FB_ROW_PLANAR_BYTES);
    planar += 2u * FB_ROW_PLANAR_BYTES;
    src += 2u * FB_ROW_SRC_BYTES;
  }
}

/* The chunk-reversed layout the m68k's predecrement MOVEM blit expects
 * (cart_shared.h): cart chunk k is scratch chunk last-k, and the tail is
 * copied as it is. fb_chunked_asm.S's fb_chunk_reverse_copy48 in C. */
void fb_planar_publish(uint16_t *planar) {
  const uint8_t *scratch = (const uint8_t *)fb_planar_scratch;
  uint8_t *cart = (uint8_t *)planar;
  for (unsigned k = 0; k < CART_FB_CHUNK_COUNT; k++) {
    memcpy(cart + k * CART_FB_CHUNK_BYTES,
           scratch + (CART_FB_CHUNK_COUNT - 1u - k) * CART_FB_CHUNK_BYTES,
           CART_FB_CHUNK_BYTES);
  }
  memcpy(cart + CART_FB_CHUNK_COVERED, scratch + CART_FB_CHUNK_COVERED,
         CART_FB_CHUNK_TAIL);
}

void fb_chunky_to_planar(uint16_t *planar) {
  fb_transpose();
  fb_planar_publish(planar);
}
