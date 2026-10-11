/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: cart_shared.h
 * Description: rp/src/include/cart_shared.h for EmuMD. The layout is
 *              the real one; only the buffers memmap_rp.ld parks in the
 *              hole inside the shared region are ordinary variables
 *              here (there is no linker script, and host section names
 *              differ).
 */

#include_next "cart_shared.h"

#undef __cart_app_free
#define __cart_app_free(name)
