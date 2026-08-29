/**
 * File: fb.h
 * Description: Framebuffer module — owns the single 32 KB cartridge
 *              framebuffer the m68k reads each VBL.
 *
 * Single-FB design: there is exactly one framebuffer in
 * the shared region (`$FA8300`, 32 KB, 320x200x4bpp). Double-buffering
 * happens on the ST side; the RP just writes into this one buffer.
 *
 * This header is introduced alongside fb_font.* and
 * fb_draw.* so the ported font/draw modules have a stable target
 * type. fb.c defines `fb_screen` and provides
 * the init / clear / address-accessor entry points.
 */

#ifndef FB_H
#define FB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FB_MODE {
  unsigned short h_pixels;
  unsigned short v_pixels;
  unsigned char color_bits; /* bits per pixel */
};

/* Mirrors md-sprites-demo's VGA_SCREEN but with a single framebuffer
 * pointer instead of A/B/current/hidden (we don't keep a back buffer in
 * cartridge). The ported text renderer writes into
 * `framebuffer` directly. */
struct FB_SCREEN {
  unsigned int *framebuffer;
  uint16_t width;
  uint16_t height;
  uint8_t color_bits;
  uint8_t _pad;
};

extern struct FB_SCREEN fb_screen;

/* Built-in 320x200 4 bpp Atari ST low-res mode. Defined in fb.c. */
extern const struct FB_MODE fb_mode_320x200;

/**
 * @brief Populate `fb_screen` from a mode descriptor, build the pixel
 *        mask LUT used by the text/draw primitives, and zero the
 *        framebuffer.
 *
 * @return 0 on success, -1 if `mode` is NULL.
 */
int fb_init(const struct FB_MODE *mode);

/* fb_init paints a one-frame boot splash internally; the app owns
 * every frame after that, so the splash renderers are static to fb.c
 * rather than public API. */

/** @brief Publish the current chunked buffer to the cart framebuffer,
 *         synchronized to the Atari's 50 Hz VBL, tear-free.
 *
 *         Three steps: (1) transpose chunked -> planar SCRATCH (RP RAM,
 *         the slow ~1 ms part) -- this overlaps the m68k's blit of the
 *         previous frame since it doesn't touch the cart FB; (2) BLOCK
 *         until the m68k acks it finished that blit (a cart-bus read at
 *         $FB8400 captured by commemul) so the cart FB is free; (3) do
 *         the fast ~120 us chunk-reversed copy scratch -> cart FB and
 *         bump FB_FRAME_COUNTER.
 *
 *         Because only the short copy in step 3 writes the cart FB, and
 *         it runs in the m68k's post-blit slack, the RP never writes
 *         the FB while the m68k reads it -- no tearing, full 50 Hz. A
 *         ~60 ms timeout (safety net for "m68k not running", e.g. at
 *         boot) keeps the RP from hanging. Drawing into
 *         `fb_chunked_buffer` (RP RAM) is unsynchronized; call this
 *         once per frame after drawing. */
void fb_publish(void);

/** @brief Drain the ROM3 commemul ring once, routing each captured
 *         sample to BOTH the IKBD demux and the VBL frame-sync
 *         detector. Call from the main loop in place of a bare
 *         commemul_poll(); fb_publish() also calls it internally while
 *         waiting for the VBL ack, so IKBD/ESC stay responsive during
 *         the wait. */
void fb_pump_rom3(void);

#ifdef __cplusplus
}
#endif

#endif /* FB_H */
