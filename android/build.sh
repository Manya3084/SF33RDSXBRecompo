#!/bin/sh
# The Android APK, start to end:
#
#   android/build.sh [debug|release]
#
# ANDROID_HOME is the SDK (with the NDK in it), SF_DECOMP_DIR the decomp
# checkout (default ../3s-decomp-ogxb), SF_ANDROID_WORK where the
# libraries are built (default build-android-work). The APK is in
# android/app/build/outputs/apk/.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KIND=${1:-debug}
: "${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
NDK=${ANDROID_NDK:-$ANDROID_HOME/ndk/30.0.16248370}
WORK=${SF_ANDROID_WORK:-$ROOT/build-android-work}
DECOMP=${SF_DECOMP_DIR:-$ROOT/../3s-decomp-ogxb}
PREFIX=$WORK/prefix

if [ ! -f "$PREFIX/lib/libSDL3.so" ]; then
    ANDROID_NDK=$NDK "$ROOT/android/build-deps.sh" "$WORK"
fi
cmake -S "$ROOT" -B "$ROOT/build-android" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
    -DSF_ANDROID_PREFIX="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DSF_DECOMP_DIR="$DECOMP" -DGLSLC="$NDK/shader-tools/linux-x86_64/glslc" >/dev/null
cmake --build "$ROOT/build-android"

LIBS=$ROOT/android/app/src/main/jniLibs/arm64-v8a
mkdir -p "$LIBS"
cp "$ROOT/build-android/libmain.so" "$PREFIX/lib/libSDL3.so" "$PREFIX/lib/libopenal.so" "$LIBS/"

cd "$ROOT/android"
TASK=assembleDebug
[ "$KIND" = release ] && TASK=assembleRelease
# clean first: an APK packed over the last one keeps the old libraries'
# bytes as dead space
./gradlew --no-daemon -q clean "$TASK" -PsdlJava="$WORK/src/SDL/android-project/app/src/main/java"
ls -l app/build/outputs/apk/*/*.apk
