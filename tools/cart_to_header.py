#!/usr/bin/env python3
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert a Lynx cartridge image into a C header for the firmware.

The image lands in flash as a `static const uint8_t` array; the emulator
seeds it into the SD cartridge folder on boot when it isn't there
already (see scan_games() in rp/src/lynxemu.cpp).

Usage:
  cart_to_header.py GAME.lnx --out rp/src/include/builtin_game.h --name lynx_builtin_game
"""
import argparse
import os


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cart")
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", required=True, help="C symbol prefix")
    args = ap.parse_args()

    with open(args.cart, "rb") as f:
        data = f.read()

    lines = []
    for i in range(0, len(data), 16):
        row = "".join("0x%02x," % b for b in data[i:i + 16])
        lines.append("    " + row)

    with open(args.out, "w") as f:
        f.write("/*\n"
                " * Copyright (C) 2026 Neil Rackett\n"
                " * SPDX-License-Identifier: GPL-3.0-or-later\n"
                " *\n"
                " * Auto-generated -- do not edit by hand.\n"
                " * Source: %s\n"
                " */\n"
                "#pragma once\n"
                "#include <stdint.h>\n"
                "\n"
                "static const uint8_t %s[] = {\n"
                % (os.path.basename(args.cart), args.name))
        f.write("\n".join(lines))
        # A macro, not a const variable, so it can size a file-scope table.
        f.write("\n};\n"
                "#define %s_len %du\n" % (args.name, len(data)))

    print("%s: %d bytes -> %s" % (args.cart, len(data), args.out))


if __name__ == "__main__":
    main()
