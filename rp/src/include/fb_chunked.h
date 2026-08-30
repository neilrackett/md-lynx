/**
 * File: fb_chunked.h
 * Description: Chunked-pixel framebuffer + chunky-to-planar publish path.
 *
 * App code draws into `fb_chunked_buffer` (one byte per pixel, palette
 * index in the low nibble). A single conversion pass per frame
 * (`fb_chunky_to_planar`) transposes the chunked bytes into the Atari
 * ST 4 bpp planar layout that lives in the shared cart framebuffer at
 * $FA8300, where the m68k VBL blit picks it up.
 *
 * The conversion writes 16-bit plane WORDS into cart-mirrored RP RAM,
 * so the cart-bus byte-swap is transparent at the word level (same
 * property the older direct-planar `pixel_masks_flat` writes relied
 * on). Only the low 4 bits of each chunked byte are used; the high 4
 * bits are dropped during transposition.
 */

#ifndef FB_CHUNKED_H_INCLUDED
#define FB_CHUNKED_H_INCLUDED

#include <stdint.h>

#include "pico/stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Chunked framebuffer dimensions. Matches the Atari ST low-res screen
 * the planar conversion targets. */
#define FB_CHUNKED_W 320
#define FB_CHUNKED_H 200
#define FB_CHUNKED_SIZE (FB_CHUNKED_W * FB_CHUNKED_H)

/* Storage lives in regular RP RAM (`RAM` region at 0x20000000), not the
 * cart-mirrored ROM_IN_RAM region. Apps poke pixels here directly.
 *
 * MD/Lynx: the pixel array is wrapped in a struct with guard bands so the
 * Lynx renderer can write 2x-scaled scanlines straight into it, saving a
 * 16 KB intermediate buffer this port cannot afford. Mikey emits 102
 * scanlines at a 640-byte pitch and the first and last of them fall
 * outside the ST's 200 visible rows (that is the "crop 2 pixels top and
 * bottom of 320x204" the port is built around), so one scanline of slack
 * is needed below the buffer and half a one above it. Nothing ever reads
 * the guards -- the chunky-to-planar pass only sees `pixels`.
 *
 * `fb_chunked_buffer` is a macro, not a variable, so it still behaves as
 * a `uint8_t[FB_CHUNKED_SIZE]` for indexing, address-of and sizeof. */
#define FB_CHUNKED_GUARD_LO 640u
#define FB_CHUNKED_GUARD_HI 384u

struct FB_CHUNKED_STORE {
  uint8_t guard_lo[FB_CHUNKED_GUARD_LO];
  uint8_t pixels[FB_CHUNKED_SIZE];
  uint8_t guard_hi[FB_CHUNKED_GUARD_HI];
};
extern struct FB_CHUNKED_STORE fb_chunked_store;
#define fb_chunked_buffer (fb_chunked_store.pixels)

/* Launch Core 1 with the chunky-to-planar worker loop. Must be called
 * exactly once, from Core 0, before the first call to
 * fb_chunky_to_planar. fb_init() does this. A second call would
 * deadlock inside pico-sdk's launch handshake. */
void fb_chunked_init(void);

/* Run a job on Core 1, in parallel with Core 0 (dual-core).
 * `fb_core1_dispatch` hands `job(arg)` to the parked Core 1 worker and
 * returns immediately; the caller then does its own half of the work and
 * calls `fb_core1_wait()` to join. The fn+arg are passed through the
 * inter-core FIFO, whose push/pop carry the cross-core memory barriers,
 * so data prepared before dispatch is visible to the job and the job's
 * writes are visible after wait. Each dispatch MUST be paired with one
 * wait before the next dispatch (the c2p in fb_transpose uses this too,
 * so demos must join their own job before fb_publish). */
typedef void (*fb_core1_job_t)(void *arg);
void __not_in_flash_func(fb_core1_dispatch)(fb_core1_job_t job, void *arg);
void __not_in_flash_func(fb_core1_wait)(void);

/* Hold Core 1 in a RAM-resident spin so Core 0 can erase or program
 * flash. Core 1's idle FIFO pop is a flash-resident SDK call and would
 * fault with XIP disabled. Park, do the flash work with interrupts off,
 * unpark; the pair counts as one dispatch/wait, so it must not straddle
 * an fb_publish(). */
void fb_core1_park(void);
void fb_core1_unpark(void);

/* Fill the entire chunked buffer with a single palette index. */
void fb_chunked_clear(uint8_t color);

/* Bounds-checked single-pixel plot; mostly useful for diagnostics. */
static inline void fb_chunked_plot(unsigned int x, unsigned int y,
                                   uint8_t color) {
  if (x < FB_CHUNKED_W && y < FB_CHUNKED_H) {
    fb_chunked_buffer[y * FB_CHUNKED_W + x] = color;
  }
}

/**
 * Transpose the chunked buffer into Atari ST 4 bpp planar format.
 *
 * Output layout per 16-pixel block (matches ST low-res):
 *   word 0 = plane 0 (LSB of palette index)
 *   word 1 = plane 1
 *   word 2 = plane 2
 *   word 3 = plane 3 (MSB)
 * Within each word, bit (15 - i) corresponds to pixel `i` of the block.
 *
 * **Must be called from Core 0 only.** The implementation dispatches
 * the bottom half of the buffer to Core 1 via the inter-core FIFO
 * and blocks on Core 1's completion signal. Calling from Core 1
 * deadlocks (Core 1 would push to a FIFO it isn't reading from, then
 * block on a pop that never resolves).
 *
 * @param planar  Destination, must point to the cart framebuffer at
 *                $FA8300 (= 32 000 bytes of cart-mirrored RP RAM).
 *                Cart-bus byte-swap is invisible because we write
 *                16-bit values, not raw bytes.
 */
void __not_in_flash_func(fb_chunky_to_planar)(uint16_t *planar);

/* Split halves of the above, so the slow transpose can run before the
 * VBL wait (overlapping the m68k blit) and only the fast cart-FB write
 * runs after it. fb.c's fb_publish() uses these directly. */

/* Dual-core transpose: chunked buffer -> internal planar scratch (RP
 * RAM). Does NOT touch the cart FB; safe to run while the m68k blits. */
void __not_in_flash_func(fb_transpose)(void);

/* As above, but only the EVEN chunked rows are read; each planar row is
 * then duplicated into the one below. For callers that draw at a
 * 2-row pitch (the Lynx renderer does), this halves the conversion --
 * the odd rows are a 160-byte memcpy rather than 320 bytes of
 * transpose, and the caller no longer fills them itself. The published
 * image is identical. */
void __not_in_flash_func(fb_transpose_row_doubled)(void);

/* Chunk-reversed copy of the last transpose's scratch -> `planar` (the
 * cart FB). The ONLY cart-FB write; ~120 us. Call after fb_transpose()
 * and after the m68k has finished blitting the previous frame. */
void __not_in_flash_func(fb_planar_publish)(uint16_t *planar);

#ifdef __cplusplus
}
#endif

#endif /* FB_CHUNKED_H_INCLUDED */
