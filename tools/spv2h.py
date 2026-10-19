#!/usr/bin/env python3
"""Write a SPIR-V module as a C array: spv2h.py IN.spv OUT.h NAME."""

import sys

data = open(sys.argv[1], "rb").read()
name = sys.argv[3]
words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
with open(sys.argv[2], "w") as f:
    f.write(f"/* {name}: generated from {sys.argv[1].rsplit('/', 1)[-1]} by tools/spv2h.py */\n")
    f.write("#include <stddef.h>\n#include <stdint.h>\n")
    f.write(f"static const uint32_t {name}[] = {{\n")
    for i in range(0, len(words), 8):
        f.write("    " + ", ".join(f"0x{w:08x}" for w in words[i:i + 8]) + ",\n")
    f.write("};\n")
    f.write(f"static const size_t {name}_size = sizeof {name};\n")
