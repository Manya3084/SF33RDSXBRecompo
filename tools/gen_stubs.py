#!/usr/bin/env python3
"""Write a stub for every name the game code needs and nothing defines.

Reads the symbol tables of the given archives (the decomp's C and the
port shims), takes every undefined name no archive defines and no host
library (libc, libm, zlib, SDL3) exports, and writes one definition per
name:

  a function   reports the call once (sf_port_missing) and returns 0
  data         a zeroed, 16-byte aligned block of 64 KiB, or of twice
               the retail global's size where that is larger (--sizes: the
               decomp's config/sf33rd/symbols/data.txt): the game writes
               a buffer's whole length, and a short block let it run on
               into the next one

A name is a function when every relocation against it is a call
(R_X86_64_PLT32 or PC32 from a call instruction is what gcc emits); any
other use (an absolute or 32S address load) makes it data.

A name the decomp's fold list gives (tools/fold_aliases.py --format ld:
retail's linker merged identical bodies, and a caller kept the address of
the merged body) is not a stub: it jumps to the body's own name, so the
arguments pass through untouched.

    gen_stubs.py [--aliases FOLD.txt] OUT.c ARCHIVE...

For Android (an NDK cross build), --arch aarch64 (arm64's calls, and a
branch for an alias), --no-system and a --host-so for each of the
platform's libraries.

For Windows (a MinGW-w64 cross build), --coff with --nm and --objdump
naming that toolchain's tools, and --host for each import library whose
names the system gives (in place of the Linux shared libraries). COFF
gives calls and data references the same relocation, so there a name is
a function when every instruction that refers to it is a call or a jump.
"""

from __future__ import annotations

import re
import subprocess
import sys

NM = "nm"
OBJDUMP = "objdump"
HOST_LIBS = ["libc.so.6", "libm.so.6", "libz.so.1", "libSDL3.so.0", "libvulkan.so.1", "libGL.so.1", "libopenal.so.1", "libarchive.so.13"]


def syms(path: str, dynamic: bool = False) -> tuple[set[str], set[str]]:
    args = [NM, "-D", "--defined-only", path] if dynamic else [NM, path]
    out = subprocess.run(args, capture_output=True, text=True).stdout
    d, u = set(), set()
    for line in out.splitlines():
        p = line.split()
        if len(p) == 2 and p[0] == "U":
            u.add(p[1])
        elif len(p) == 3 and (p[1].isupper() or (dynamic and p[1] in "iVW")) and p[1] != "U" and (dynamic or p[1] not in "VW"):
            d.add(p[2].split("@")[0])
    return d, u


# a call's relocation: x86-64's PLT32, arm64's CALL26 (bl) and JUMP26 (b)
CALLS = {"R_X86_64_PLT32", "R_AARCH64_CALL26", "R_AARCH64_JUMP26"}


def call_only(archives: list[str]) -> set[str]:
    """Names whose every relocation is a call's."""
    kinds: dict[str, set[str]] = {}
    for a in archives:
        out = subprocess.run([OBJDUMP, "-r", a], capture_output=True, text=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3 and p[1].startswith(("R_X86_64_", "R_AARCH64_")):
                n = re.split(r"[-+]0x", p[2])[0]
                kinds.setdefault(n, set()).add(p[1])
    return {n for n, k in kinds.items() if k <= CALLS}


def call_only_coff(archives: list[str]) -> set[str]:
    """Names every instruction refers to by a call or a jump (COFF)."""
    kinds: dict[str, set[bool]] = {}
    for a in archives:
        out = subprocess.run([OBJDUMP, "-dr", a], capture_output=True, text=True).stdout
        last = ""
        for line in out.splitlines():
            m = re.match(r"\s+[0-9a-f]+:\s+IMAGE_REL_AMD64_\w+\s+(\S+)", line)
            if m:
                n = re.split(r"[-+]0x", m[1])[0]
                kinds.setdefault(n, set()).add(last.split()[0].startswith(("call", "jmp")) if last.split() else False)
                continue
            m = re.match(r"\s+[0-9a-f]+:\t(?:[0-9a-f]{2} )+\s*\t(.*)", line)
            if m:
                last = m[1]
    return {n for n, k in kinds.items() if k == {True}}


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
    global NM, OBJDUMP
    args = sys.argv[1:]
    aliases: dict[str, str] = {}
    coff = False
    arch = "x86_64"
    system = True
    hosts: list[str] = []
    host_so: list[str] = []
    sizes: dict[str, int] = {}
    while args and args[0].startswith("--"):
        if args[0] == "--aliases":
            for line in open(args[1]):
                m = re.fullmatch(r"--defsym=(\w+)=(\w+)\s*", line)
                if m:
                    aliases[m[1]] = m[2]
        elif args[0] == "--nm":
            NM = args[1]
        elif args[0] == "--objdump":
            OBJDUMP = args[1]
        elif args[0] == "--host":
            hosts.append(args[1])
        elif args[0] == "--host-so":
            host_so.append(args[1])
        elif args[0] == "--sizes":
            for line in open(args[1]):
                m = re.match(r"\s*(\w+)\s*=\s*0x[0-9A-Fa-f]+;.*size:0x([0-9A-Fa-f]+)", line)
                if m:
                    sizes[m[1]] = int(m[2], 16)
        elif args[0] == "--coff":
            coff = True
            args = args[1:]
            continue
        elif args[0] == "--no-system":
            # a cross build: only the --host-so libraries, not this machine's
            system = False
            args = args[1:]
            continue
        elif args[0] == "--arch":
            arch = args[1]
        args = args[2:]
    out, archives = args[0], args[1:]
    defined, undef = set(), set()
    for a in archives:
        d, u = syms(a)
        defined |= d
        undef |= u
    host = set()
    for p in hosts:
        d = syms(p)[0]
        # an import library defines NAME and __imp_NAME
        host |= d | {n[6:] for n in d if n.startswith("__imp_")}
    # the libraries the build links (wherever they are installed), then the
    # system's (not for Windows, whose --host list is all of them)
    for p in host_so + ([q for q in map(host_lib, HOST_LIBS) if q] if system and not hosts else []):
        host |= syms(p, dynamic=True)[0]
    # names the linker itself provides: the section bounds the game-state
    # snapshot reads (src/port/state.c)
    linker = {"__data_start", "_edata", "__bss_start", "_end"}
    need = sorted(
        n
        for n in undef - defined - host - linker
        # (and a sanitizer build's runtime: -fsanitize=address, undefined)
        if not n.startswith(("_GLOBAL_OFFSET_TABLE_", "__start_", "__stop_", "__asan_", "__ubsan_", "__sanitizer_"))
    )
    # a library's own function stubbed means the library was not found:
    # the program would run with it missing
    lost = [n for n in need if re.match(r"(SDL_|vk[A-Z]|al[A-Z]|alc[A-Z]|gl[A-Z]|archive_|sqlite3_|__aarch64_|__atomic_|__sync_)", n)]
    if lost:
        print(f"gen_stubs: library functions not found in any host library: {' '.join(lost[:12])}", file=sys.stderr)
        return 1
    # an alias whose body is not built yet needs a stub of its own
    need += sorted({aliases[n] for n in need if n in aliases} - defined - set(need))
    lines = [
        "/* stubs.c: written by tools/gen_stubs.py at build time. Do not edit.",
        f" * {len(need)} names the game code needs that no shim defines yet. */",
        "",
        'void sf_port_missing(const char *name);',
        "",
    ]
    funcs = call_only_coff(archives) if coff else call_only(archives)
    for n in need:
        if n in aliases:
            t = aliases[n]
            kind = "" if coff else f"\\n.type {n}, @function"
            jump = "b" if arch == "aarch64" else "jmp"
            lines.append(f'__asm__(".text\\n.globl {n}{kind}\\n{n}:\\n    {jump} {t}\\n");')
        elif n not in funcs:
            # twice the retail size: a global that holds addresses (the
            # effect pool, frw, is uintptr_t rows) takes twice the bytes
            # on a 64-bit target
            size = max(65536, (2 * sizes.get(n, 0) + 15) & ~15)
            lines.append(f"__attribute__((aligned(16))) unsigned char {n}[{size}];")
        else:
            lines.append(f"long {n}(void) {{ sf_port_missing(\"{n}\"); return 0; }}")
    open(out, "w").write("\n".join(lines) + "\n")
    print(f"gen_stubs: {len(need)} stubs", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
