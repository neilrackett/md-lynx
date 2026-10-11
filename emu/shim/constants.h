/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: constants.h
 * Description: rp/src/include/constants.h for EmuMD: the real header,
 *              with MD/Lynx's ROM_TEMP window put where memmap_rp.ld puts
 *              it. EmuMD's own _rom_temp_start is the microfirmware
 *              template's (0x100000, 128 KB), and MD/Lynx moved it to
 *              0xA0000 to hold a 512 KB cartridge. Its size comes from
 *              the address of an absolute linker symbol, which a shared
 *              library cannot have, so that is set here too.
 */

#ifndef MDLYNX_EMU_CONSTANTS_H
#define MDLYNX_EMU_CONSTANTS_H

#include "mdfw.h"

/* The real rom_temp_size() and the symbol it reads, out of the way. */
#define __rom_temp_size__ mdlynx_emu_no_rom_temp_size
#define rom_temp_size mdlynx_emu_linker_rom_temp_size
#include_next "constants.h"
#undef rom_temp_size
#undef __rom_temp_size__

/* memmap_rp.ld's ROM_TEMP, as an offset into the 2 MB flash. */
#define MDLYNX_ROM_TEMP_FLASH_OFFSET 0xA0000u
#define MDLYNX_ROM_TEMP_FLASH_BYTES (512u * 1024u)

// NOLINTBEGIN(readability-identifier-naming)
#define _rom_temp_start \
  (*(unsigned int *)&mdfw_flash[MDLYNX_ROM_TEMP_FLASH_OFFSET])
// NOLINTEND(readability-identifier-naming)

static inline uint32_t rom_temp_size(void) {
  return MDLYNX_ROM_TEMP_FLASH_BYTES;
}

#endif
