#!/usr/bin/env python3
"""Write SPIR-V for one .xvu or .xpu file.

The binary goes to -o. It is not written to stdout.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ogxb.spirv import combiner_spirv, vertex_spirv
from ogxb.xpu import parse_xpu
from ogxb.xvs import decode_program


def convert(source: str, dest: str) -> int:
    path = Path(source)
    if dest == "-":
        print("refusing to write SPIR-V to stdout", file=sys.stderr)
        return 2
    blob = path.read_bytes()
    suffix = path.suffix.lower()
    if suffix == ".xvu":
        module = vertex_spirv(blob)
        detail = None if module is None else f"{len(decode_program(blob) or [])} slots"
    elif suffix == ".xpu":
        module = combiner_spirv(blob)
        shader = parse_xpu(blob)
        detail = None if shader is None else f"{shader.stages} stages"
    else:
        print(f"bad {path.name}", file=sys.stderr)
        return 1
    if module is None or detail is None:
        print(f"bad {path.name}", file=sys.stderr)
        return 1
    Path(dest).write_bytes(module)
    print(f"{path.name}  {detail}  wrote {Path(dest).name}")
    return 0


def main() -> int:
    if len(sys.argv) != 4 or sys.argv[2] != "-o":
        print("usage: python3 tools/spirv.py FILE -o OUT.spv", file=sys.stderr)
        return 2
    return convert(sys.argv[1], sys.argv[3])


if __name__ == "__main__":
    raise SystemExit(main())
