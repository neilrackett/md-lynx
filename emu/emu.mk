# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make emu, included from the Makefile. It lives here rather than there so
# that changing it is an emulator-only change.

## Build MD/Lynx for this computer and run it in Hatari with EmuMD (see
## README.md). TOS= a TOS image (default: EmuMD's EmuTOS); RECORD=FILE.avi
## records picture and sound until you quit; EMUMD= another EmuMD checkout.
EMUMD ?= emu/emumd
.PHONY: emu
emu:
	$(EMUMD)/tools/mdfw run $(if $(TOS),--tos $(TOS)) $(if $(RECORD),--record $(RECORD))
