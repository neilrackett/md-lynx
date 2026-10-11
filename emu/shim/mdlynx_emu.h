/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: mdlynx_emu.h
 * Description: Included ahead of every source (mdfw.ini's cflags). The
 *              headers in rp/src/include include their neighbours in
 *              quotes, which finds them in the same folder before any
 *              stand-in, so the stand-ins go first and their include
 *              guards keep the originals out -- in C++ too, which
 *              EmuMD's own template prefix leaves to the source.
 */

#ifndef MDLYNX_EMU_H
#define MDLYNX_EMU_H

#include <stdint.h>
#include <sys/types.h>

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"

#endif
