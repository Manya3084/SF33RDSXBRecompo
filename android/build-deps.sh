#!/bin/sh
# The Android build's libraries, built from source for arm64-v8a:
# SDL3, OpenAL Soft, xz's liblzma and libarchive (the import's 7z and
# zip). zlib and Vulkan come with the NDK.
#
#   android/build-deps.sh WORK
#
# WORK gets src/ (the checkouts), build/ and prefix/ (what the game's
# build reads: -DSF_ANDROID_PREFIX=WORK/prefix). ANDROID_NDK is the NDK.
set -eu

WORK=${1:?usage: build-deps.sh WORK}
NDK=${ANDROID_NDK:?set ANDROID_NDK to the NDK}
ABI=arm64-v8a
API=26
PREFIX=$WORK/prefix
mkdir -p "$WORK/src" "$WORK/build" "$PREFIX"

fetch() { # url tag dir
    if [ ! -d "$WORK/src/$3" ]; then
        git clone -q --depth 1 --branch "$2" "$1" "$WORK/src/$3"
    fi
}

build() { # dir, then CMake options
    d=$1
    shift
    cmake -S "$WORK/src/$d" -B "$WORK/build/$d" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$PREFIX" "$@" >/dev/null
    cmake --build "$WORK/build/$d" >/dev/null
    cmake --install "$WORK/build/$d" >/dev/null
    echo "built $d"
}

fetch https://github.com/libsdl-org/SDL.git release-3.4.18 SDL
fetch https://github.com/kcat/openal-soft.git 1.25.2 openal-soft
fetch https://github.com/tukaani-project/xz.git v5.8.4 xz
fetch https://github.com/libarchive/libarchive.git v3.8.10 libarchive

build SDL -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
build openal-soft -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF -DALSOFT_INSTALL_CONFIG=OFF \
    -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF -DALSOFT_INSTALL_UTILS=OFF \
    -DALSOFT_INSTALL_EXAMPLES=OFF
build xz -DBUILD_SHARED_LIBS=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF \
    -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF -DXZ_DOC=OFF -DXZ_NLS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
build libarchive -DBUILD_SHARED_LIBS=OFF -DENABLE_TEST=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF \
    -DENABLE_UNZIP=OFF -DENABLE_OPENSSL=OFF -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_ICONV=OFF \
    -DENABLE_LZ4=OFF -DENABLE_ZSTD=OFF -DENABLE_BZip2=OFF -DENABLE_LIBB2=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF \
    -DENABLE_LZMA=ON -DENABLE_ZLIB=ON -DENABLE_WERROR=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DLIBLZMA_INCLUDE_DIR="$PREFIX/include" -DLIBLZMA_LIBRARY="$PREFIX/lib/liblzma.a"
