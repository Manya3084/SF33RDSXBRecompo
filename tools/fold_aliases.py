#!/usr/bin/env python3
"""The linker aliases for calls to folded bodies, from the port's own build.

Retail's linker folded identical functions into one body
(analysis-ps2map/folded.json in 3s-decomp-ogxb). Where the decomp's C
keeps a call by the body's address (sub_ADDR) because several names share
it, the body is defined under one of those names only and sub_ADDR is
left undefined. This lists every such name the archives reference and
none defines, and the defined function at the same address it stands
for, as --defsym lines (tools/gen_stubs.py --aliases reads them). It is
the decomp's tools/fold_aliases.py with the port's archives in place of
the decomp's MSVC objects, so it needs no Xbox build.

    fold_aliases.py [--nm NM] DECOMP_DIR OUT ARCHIVE...
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path


def names_at(decomp: Path) -> dict[int, list[str]]:
    at: dict[int, list[str]] = {}
    funcs = decomp / "config" / "sf33rd" / "symbols" / "funcs.txt"
    for line in funcs.read_text(errors="replace").splitlines():
        m = re.match(r"(\S+) = 0x([0-9A-Fa-f]+);", line)
        if m:
            at.setdefault(int(m[2], 16), []).append(m[1])
    folded = decomp / "analysis-ps2map" / "folded.json"
    if folded.is_file():
        for a, ns in json.loads(folded.read_text()).items():
            at.setdefault(int(a, 16), []).extend(ns)
    return at


def main() -> int:
    args = sys.argv[1:]
    nm = "nm"
    if args[:1] == ["--nm"]:
        nm, args = args[1], args[2:]
    decomp, out, archives = Path(args[0]), args[1], args[2:]
    defined: set[str] = set()
    undef: set[str] = set()
    for a in archives:
        text = subprocess.run([nm, a], capture_output=True, text=True).stdout
        for line in text.splitlines():
            p = line.split()
            if len(p) == 2 and p[0] == "U":
                undef.add(p[1])
            elif len(p) == 3 and p[1] in "TtWw":
                defined.add(p[2])
    at = names_at(decomp)
    lines = []
    for n in sorted(undef - defined):
        m = re.fullmatch(r"sub_([0-9A-F]{8})", n)
        if not m:
            continue
        to = [t for t in at.get(int(m[1], 16), []) if t in defined]
        if to:
            lines.append(f"--defsym={n}={to[0]}")
    Path(out).write_text("".join(l + "\n" for l in lines))
    print(f"fold_aliases: {len(lines)} aliases", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
