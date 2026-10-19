#!/bin/sh
# Every global the game code declares with different types in different
# files. On the 32-bit build two such types often have the same size, and
# on x86-64 they do not (a byte-array row the size of 448 u32 words, a
# partial struct copy with 32-bit padding), so one file reads what another
# never wrote. GCC's LTO link compares every declaration of a name; this
# compiles the files the port builds as LTO objects and prints its report.
#
#     tools/lto_type_check.sh BUILD_DIR > report.txt
set -eu
build=$(cd "${1:?usage: $0 BUILD_DIR}" && pwd)
decomp=$(sed -n 's/^SF_DECOMP_DIR:PATH=//p' "$build/CMakeCache.txt")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
grep -oE '"/[^"]+\.c"' "$build/CMakeFiles/sf_decomp.dir/DependInfo.cmake" | tr -d '"' | sort -u |
    xargs -P"$(nproc)" -I{} sh -c '
        f={}; o='"$work"'/$(echo "$f" | md5sum | cut -c1-16).o
        gcc -m64 -O0 -std=gnu17 -w -fcommon -fno-strict-aliasing -fwrapv -fno-pie -D_GNU_SOURCE \
            -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-implicit-function-declaration \
            -Wno-implicit-int -Wno-return-mismatch -flto \
            -I'"$build"'/decomp/include -I"$(dirname "$f")" -I'"$decomp"'/src/sf33rd/Source/Game \
            -c "$f" -o "$o" || echo "does not compile: $f" >&2'
gcc -m64 -flto -fcommon -r -nostdlib -flinker-output=nolto-rel -Wlto-type-mismatch \
    -o "$work/all.o" "$work"/*.o 2>&1 | sed "s#$decomp/##g"
