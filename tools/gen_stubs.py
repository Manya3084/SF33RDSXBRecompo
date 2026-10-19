#!/usr/bin/env python3
"""Write a stub for every name the game code needs and nothing defines.

Reads the symbol tables of the given archives (the decomp's C and the
port shims), takes every undefined name no archive defines and no host
library (libc, libm, zlib, SDL3) exports, and writes one definition per
name:

  a function   reports the call once (sf_port_missing) and returns 0
  data         a zeroed, 16-byte aligned block of 64 KiB

A name is a function when every relocation against it is a call
(R_X86_64_PLT32 or PC32 from a call instruction is what gcc emits); any
other use (an absolute or 32S address load) makes it data.

A name the decomp's fold list gives (tools/fold_aliases.py --format ld:
retail's linker merged identical bodies, and a caller kept the address of
the merged body) is not a stub: it jumps to the body's own name, so the
arguments pass through untouched.

    gen_stubs.py [--aliases FOLD.txt] OUT.c ARCHIVE...
"""

from __future__ import annotations

import re
import subprocess
import sys

HOST_LIBS = ["libc.so.6", "libm.so.6", "libz.so.1", "libSDL3.so.0", "libvulkan.so.1", "libGL.so.1", "libopenal.so.1"]


def syms(path: str, dynamic: bool = False) -> tuple[set[str], set[str]]:
    args = ["nm", "-D", "--defined-only", path] if dynamic else ["nm", path]
    out = subprocess.run(args, capture_output=True, text=True).stdout
    d, u = set(), set()
    for line in out.splitlines():
        p = line.split()
        if len(p) == 2 and p[0] == "U":
            u.add(p[1])
        elif len(p) == 3 and (p[1].isupper() or (dynamic and p[1] in "iVW")) and p[1] != "U" and (dynamic or p[1] not in "VW"):
            d.add(p[2].split("@")[0])
    return d, u


def call_only(archives: list[str]) -> set[str]:
    """Names whose every relocation is R_X86_64_PLT32."""
    kinds: dict[str, set[str]] = {}
    for a in archives:
        out = subprocess.run(["objdump", "-r", a], capture_output=True, text=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3 and p[1].startswith("R_X86_64_"):
                n = re.split(r"[-+]0x", p[2])[0]
                kinds.setdefault(n, set()).add(p[1])
    return {n for n, k in kinds.items() if k == {"R_X86_64_PLT32"}}


def host_lib(name: str) -> str | None:
    out = subprocess.run(["gcc", f"-print-file-name={name}"], capture_output=True, text=True).stdout.strip()
    if out and out != name:
        return out
    for d in ("/usr/lib", "/usr/lib64", "/lib/x86_64-linux-gnu"):
        p = f"{d}/{name}"
        try:
            open(p, "rb").close()
            return p
        except OSError:
            pass
    return None


def main() -> int:
    args = sys.argv[1:]
    aliases: dict[str, str] = {}
    if args[0] == "--aliases":
        for line in open(args[1]):
            m = re.fullmatch(r"--defsym=(\w+)=(\w+)\s*", line)
            if m:
                aliases[m[1]] = m[2]
        args = args[2:]
    out, archives = args[0], args[1:]
    defined, undef = set(), set()
    for a in archives:
        d, u = syms(a)
        defined |= d
        undef |= u
    host = set()
    for lib in HOST_LIBS:
        p = host_lib(lib)
        if p:
            host |= syms(p, dynamic=True)[0]
    # names the linker itself provides: the section bounds the game-state
    # snapshot reads (src/port/state.c)
    linker = {"__data_start", "_edata", "__bss_start", "_end"}
    need = sorted(
        n
        for n in undef - defined - host - linker
        if not n.startswith(("_GLOBAL_OFFSET_TABLE_", "__start_", "__stop_"))
    )
    # an alias whose body is not built yet needs a stub of its own
    need += sorted({aliases[n] for n in need if n in aliases} - defined - set(need))
    lines = [
        "/* stubs.c: written by tools/gen_stubs.py at build time. Do not edit.",
        f" * {len(need)} names the game code needs that no shim defines yet. */",
        "",
        'void sf_port_missing(const char *name);',
        "",
    ]
    funcs = call_only(archives)
    for n in need:
        if n in aliases:
            t = aliases[n]
            lines.append(f'__asm__(".text\\n.globl {n}\\n.type {n}, @function\\n{n}:\\n    jmp {t}\\n");')
        elif n not in funcs:
            lines.append(f"__attribute__((aligned(16))) unsigned char {n}[65536];")
        else:
            lines.append(f"long {n}(void) {{ sf_port_missing(\"{n}\"); return 0; }}")
    open(out, "w").write("\n".join(lines) + "\n")
    print(f"gen_stubs: {len(need)} stubs", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
