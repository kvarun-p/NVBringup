#!/usr/bin/env python3
"""Embed a binary file as a C array: bin2h.py <input> <symbol> > out.h"""
import sys

data = open(sys.argv[1], "rb").read()
name = sys.argv[2]
print(f"// Generated from {sys.argv[1]} by tools/bin2h.py. Do not edit.")
print("#pragma once\n#include <stdint.h>\n")
print(f"static const uint8_t {name}[{len(data)}] = {{")
for i in range(0, len(data), 12):
    print("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 12]) + ",")
print("};")
