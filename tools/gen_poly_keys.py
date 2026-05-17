#!/usr/bin/env python3
"""
Emit the 781 canonical Polyglot Zobrist constants as a C header.
Run:  python3 tools/gen_poly_keys.py > src/poly_keys.h
"""
import chess.polyglot

print("/* Auto-generated Polyglot random numbers — do not edit. */")
print("#pragma once")
print("#include <stdint.h>")
print("static const uint64_t POLY[781] = {")
for v in chess.polyglot.POLYGLOT_RANDOM_ARRAY:
    print(f"    UINT64_C(0x{v:016X}),")
print("};")
