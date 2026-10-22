#!/usr/bin/env python3
"""A sync log (SF_SYNC_LOG: "FRAME HASH" a line) into the hash list a sync
test reads (SF_SYNC_TEST, src/port/sync.c): "SF3S", the first frame and
the count, then a hash a frame, all little-endian u32.

    sync_pack.py LOG OUT [FRAMES]   (the first FRAMES frames; all by default)
"""

import struct
import sys


def main() -> int:
    log, out = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else None
    frames = []
    for line in open(log):
        f, h = line.split()
        frames.append((int(f), int(h, 16)))
        if limit and len(frames) >= limit:
            break
    first = frames[0][0]
    for i, (f, _) in enumerate(frames):
        if f != first + i:
            raise SystemExit(f"sync_pack: frame {f} out of order")
    with open(out, "wb") as o:
        o.write(b"SF3S" + struct.pack("<II", first, len(frames)))
        o.write(b"".join(struct.pack("<I", h) for _, h in frames))
    print(f"sync_pack: {len(frames)} frames from frame {first}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
