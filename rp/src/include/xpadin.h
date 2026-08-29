/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: xpadin.h
 * Description: Receiver for the m68k's per-VBL Xpad controller report.
 *
 * Xpad (https://github.com/neilrackett/atarist-xpad) is an open standard
 * for describing a modern gamepad to Atari ST software: a provider
 * publishes a shared block and installs the 'XPAD' cookie pointing at
 * it, and consumers poll the block. The block lives in ST RAM, so the
 * consumer half runs on the m68k -- target/atarist/src/userfw.s locates
 * and validates it at boot, then reads pad 0 tear-free every VBL and
 * forwards the button mask to the RP as two cart-bus reads in the
 * $FB8800 (high byte) and $FB8A00 (low byte) windows.
 *
 * This module reassembles those two bytes. The high byte is latched and
 * the pair committed only when the low byte lands, so a commemul ring
 * drain falling between the two reads cannot tear the value.
 *
 * Only bits 0..15 of the Xpad mask are carried, which covers every
 * button MD/Lynx uses (the d-pad, the four face buttons, both shoulders,
 * both triggers, Select, Start, Guide and the left stick click). Bit 16,
 * the right stick click, is dropped.
 *
 * Xpad is an enhancement, never a requirement: with no provider present
 * -- or with nothing plugged into pad 0 -- the m68k reports nothing,
 * xpadin_get() returns false, and the app falls back to the IKBD
 * joystick, which is the standard's own fallback ladder.
 */

#ifndef XPADIN_H_INCLUDED
#define XPADIN_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Report windows, matching XPAD_{HI,LO}_WINDOW_BASE in userfw.s. */
#define XPADIN_HI_LO16 0x8800u
#define XPADIN_LO_LO16 0x8A00u
#define XPADIN_WINDOW_MASK 0xFF00u

/* Reset the cached state. Safe to call more than once. */
void xpadin_init(void);

/* commemul ring consumer. Passed each captured ROM3 sample (fb.c's
 * dispatch does this); filters for the two Xpad windows and ignores
 * everything else. Runs in main-loop context. */
void xpadin_consume_rom3_sample(uint16_t addr_lsb);

/* Latest button mask for pad 0, using the XPAD_* bits from xpad.h.
 * Returns false when no report has arrived recently -- no provider, no
 * pad in slot 0, or the m68k stopped reporting -- in which case
 * `buttons` is left alone. */
bool xpadin_get(uint16_t *buttons);

#ifdef __cplusplus
}
#endif

#endif /* XPADIN_H_INCLUDED */
