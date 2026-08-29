/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * lynxemu.h -- app-facing entry points for the Atari Lynx emulator
 * running on the SidecarTridge Multi-device framebuffer template.
 *
 * emul.c drives these from its main loop: init once, feed decoded
 * keyboard events, render a frame per VBL, and hand the Lynx -> ST
 * audio fill callback to the audio layer.
 *
 * The emulation core under lynx/ is Handy (K. Wilkins, zlib licence)
 * by way of xrip's pico-lynx; see lynx/license.txt. Every file altered
 * for this port carries an "ALTERED SOURCE VERSION" banner as the zlib
 * licence requires.
 */

#ifndef LYNXEMU_H_INCLUDED
#define LYNXEMU_H_INCLUDED

#include <stdint.h>

#include "ikbd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-time bring-up: palette, games list, menu. No cartridge is loaded
 * -- the user picks one from the menu. Call after fb_init() /
 * palette_init() / the SD mount in emul_start(). */
void lynxemu_init(void);

/* Render one frame into the framebuffer and publish it (blocks on the
 * ST VBL via fb_publish, pacing this loop to 50 Hz). Runs the menu when
 * it is up, otherwise advances the Lynx by one or two emulated 60 Hz
 * frames. Call once per main-loop iteration. */
void lynxemu_render_frame(void);

/* Feed one decoded Atari ST key event (press/release). ESC toggles the
 * menu; in play the keys map onto the Lynx pad. */
void lynxemu_handle_key(const ikbd_key_event_t *k);

/* Per-VBL audio fill callback: converts the Lynx's 25 kHz mixed output
 * into whichever format the m68k detected (STE DMA signed PCM, or
 * YM2149 (vA,vB) volume pairs). Install via audio_set_fill_callback(). */
void lynxemu_audio_fill(uint8_t *buf, uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* LYNXEMU_H_INCLUDED */
