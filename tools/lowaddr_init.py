#!/usr/bin/env python3
"""Move address words out of static initializers.

The Xbox C stores addresses in u32 table words: `(u32)erw020` in a
`const u32 tbl[40][3]` initializer. On x86-64 that is not a constant
expression, because the address is 64 bits wide, even though this program
is linked non-PIE and every address fits in 32 bits.

This rewrites such a file: each address word in a file-scope initializer
becomes 0, `const` is dropped from that definition, and a constructor at
the end of the file stores the low 32 bits of each address at run time,
before main. A table whose initializer leaves elements implicit, or holds
anything other than scalar leaves, is refused, because its flat index
could not be trusted.

    lowaddr_init.py IN.c OUT.c
"""

from __future__ import annotations

import re
import sys

ADDR = re.compile(r"\(\s*(?:u32|s32|unsigned int|int|unsigned long)\s*\)\s*(&?\s*[A-Za-z_]\w*(?:\s*\+\s*(?:0x[0-9A-Fa-f]+|\d+))?)")
DEF = re.compile(r"^((?:XB_ALIGN16\s+)?(?:static\s+)?)(const\s+)?((?:unsigned\s+)?\w+)\s+([A-Za-z_]\w*)((?:\s*\[[^\]]*\])+)\s*=\s*\{", re.M)


def leaves(body: str) -> list[str]:
    """Scalar leaves of a brace initializer, in order."""
    out, cur, depth = [], "", 0
    for ch in body:
        if ch == "{":
            depth += 1
            continue
        if ch == "}":
            depth -= 1
            if cur.strip():
                out.append(cur.strip())
            cur = ""
            continue
        if ch == "," :
            if cur.strip():
                out.append(cur.strip())
            cur = ""
            continue
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def dims(s: str) -> list[int]:
    out = []
    for d in re.findall(r"\[([^\]]*)\]", s):
        d = d.strip()
        out.append(int(d, 0) if d else -1)
    return out


def strip_comments(s: str) -> str:
    s = re.sub(r"/\*.*?\*/", lambda m: " " * len(m[0]), s, flags=re.S)
    return re.sub(r"//[^\n]*", lambda m: " " * len(m[0]), s)


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src, encoding="utf-8", errors="surrogateescape").read()
    bare = strip_comments(text)
    edits: list[tuple[int, int, str]] = []
    fix: list[str] = []
    for m in DEF.finditer(bare):
        start = m.end() - 1
        depth, i = 0, start
        while True:
            if bare[i] == "{":
                depth += 1
            elif bare[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = bare[start:i + 1]
        if not ADDR.search(body):
            continue
        name, typ = m[4], m[3]
        if typ not in ("u32", "s32"):
            sys.exit(f"{src}: {name}: element type {typ} is not a 32-bit word")
        lv = leaves(body)
        d = dims(m[5])
        if -1 in d[1:]:
            sys.exit(f"{src}: {name}: inner dimension not given")
        inner = 1
        for x in d[1:]:
            inner *= x
        if d[0] != -1 and len(lv) != d[0] * inner:
            sys.exit(f"{src}: {name}: {len(lv)} leaves for {d}")
        if d[0] == -1 and len(lv) % inner:
            sys.exit(f"{src}: {name}: {len(lv)} leaves do not fill rows of {inner}")
        for k, leaf in enumerate(lv):
            a = ADDR.fullmatch(leaf)
            if a:
                expr = a[1].replace(" ", "")
                base = expr.split("+")[0].lstrip("&")
                off = "+" + expr.split("+")[1] if "+" in expr else ""
                fix.append(f"    ((u32 *){name})[{k}] = (u32)(unsigned long)&{base}{off};")
            elif ADDR.search(leaf):
                sys.exit(f"{src}: {name}[{k}]: address inside an expression: {leaf}")
        # zero the address words in the original text (same span as bare)
        seg = text[start:i + 1]
        seg = ADDR.sub("0", strip_comments(seg)) if ADDR.search(strip_comments(seg)) else seg
        edits.append((start, i + 1, seg))
        if m[2]:
            edits.append((m.start(2), m.end(2), ""))
    if not fix:
        sys.exit(f"{src}: no address words in a static initializer")
    for a, b, s in sorted(edits, reverse=True):
        text = text[:a] + s + text[b:]
    text += ("\n/* lowaddr_init.py: the address words above, stored before main. */\n"
             "__attribute__((constructor)) static void lowaddr_init(void) {\n" + "\n".join(fix) + "\n}\n")
    open(dst, "w", encoding="utf-8", errors="surrogateescape").write(text)
    print(f"{src}: {len(fix)} address words", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
