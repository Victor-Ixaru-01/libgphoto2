#!/bin/bash
#
# build.sh — build a curated libgphoto2 (core + libgphoto2_port + camlibs/ptp2)
# static library for iOS, and package it as an .xcframework.
#
# Usage:
#   ./ios/build.sh test         # native macOS build + run the smoke test (fast validation)
#   ./ios/build.sh ios          # build device slice   (iphoneos, arm64)
#   ./ios/build.sh sim          # build simulator slice (iphonesimulator, arm64)
#   ./ios/build.sh xcframework   # build both iOS slices + package libgphoto2.xcframework
#   ./ios/build.sh clean
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IOS="$ROOT/ios"
OUT="$IOS/build"
IOS_MIN="16.0"
MAC_MIN="13.0"

# Generate the byte-order header (normally produced by configure/meson) if it's absent.
# It is .gitignored, so a fresh checkout needs this. All HAS_* undefined → portable LE path.
if [ ! -f "$ROOT/libgphoto2/gphoto2-endian.h" ]; then
  sed 's/#mesondefine/#undef/' "$ROOT/libgphoto2/gphoto2-endian.h.in" > "$ROOT/libgphoto2/gphoto2-endian.h"
fi

# --- source set -------------------------------------------------------------
CORE_SRCS=(
  libgphoto2/gphoto2-abilities-list.c libgphoto2/ahd_bayer.c libgphoto2/bayer.c
  libgphoto2/gphoto2-camera.c libgphoto2/gphoto2-context.c libgphoto2/exif.c
  libgphoto2/gphoto2-file.c libgphoto2/gphoto2-filesys.c libgphoto2/gamma.c
  libgphoto2/jpeg.c libgphoto2/gphoto2-list.c libgphoto2/gphoto2-result.c
  libgphoto2/gphoto2-version.c libgphoto2/gphoto2-setting.c libgphoto2/gphoto2-widget.c
)
PORT_SRCS=(
  libgphoto2_port/libgphoto2_port/gphoto2-port.c
  libgphoto2_port/libgphoto2_port/gphoto2-port-info-list.c
  libgphoto2_port/libgphoto2_port/gphoto2-port-log.c
  libgphoto2_port/libgphoto2_port/gphoto2-port-version.c
  libgphoto2_port/libgphoto2_port/gphoto2-port-portability.c
  libgphoto2_port/libgphoto2_port/gphoto2-port-result.c
  libgphoto2_port/libgphoto2_port/gphoto2-port-locking.c
)
# ptp2: omit olympus-wrap.c (needs libxml2). usb.c/ptpip.c kept so library.c links.
PTP2_SRCS=(
  camlibs/ptp2/ptp.c camlibs/ptp2/library.c camlibs/ptp2/usb.c
  camlibs/ptp2/ptpip.c camlibs/ptp2/config.c camlibs/ptp2/chdk.c camlibs/ptp2/fujiptpip.c
)
SHIM_SRCS=( ios/src/ltdl_static.c ios/src/gp_ios_register.c ios/src/gp_iccamera.c )

ALL_SRCS=( "${CORE_SRCS[@]}" "${PORT_SRCS[@]}" "${PTP2_SRCS[@]}" "${SHIM_SRCS[@]}" )

INCLUDES=(
  -I"$IOS/include"                       # our config.h + stub ltdl.h
  -I"$ROOT"                              # <gphoto2/...> core headers + "libgphoto2/..." internals
  -I"$ROOT/libgphoto2_port"              # <gphoto2/gphoto2-port*.h>
  -I"$ROOT/camlibs/ptp2"                 # ptp.h, ptp-private.h, ...
)
DEFINES=( -DHAVE_CONFIG_H -D_GPHOTO2_INTERNAL_CODE )
WARN=( -Wno-unused-parameter -Wno-unused-variable -Wno-deprecated-declarations
       -Wno-pointer-sign -Wno-incompatible-function-pointer-types )
COMMON_CFLAGS=( -O2 -fPIC -fvisibility=default "${DEFINES[@]}" "${INCLUDES[@]}" "${WARN[@]}" )

obj_name() { echo "$1" | sed 's#/#_#g; s#\.c$#.o#'; }

# compile_slice <label> <clang-target-flags...>
compile_slice() {
  local label="$1"; shift
  local dir="$OUT/$label"
  mkdir -p "$dir"
  echo ">>> compiling slice: $label"
  local objs=()
  for src in "${ALL_SRCS[@]}"; do
    local o="$dir/$(obj_name "$src")"
    xcrun clang "$@" "${COMMON_CFLAGS[@]}" -c "$ROOT/$src" -o "$o"
    objs+=("$o")
  done
  xcrun libtool -static -o "$dir/libgphoto2.a" "${objs[@]}"
  echo ">>> built $dir/libgphoto2.a"
}

case "${1:-}" in
  test)
    compile_slice macos -arch arm64 -isysroot "$(xcrun --sdk macosx --show-sdk-path)" -mmacosx-version-min=$MAC_MIN
    echo ">>> linking + running smoke test (native)"
    xcrun clang -arch arm64 -isysroot "$(xcrun --sdk macosx --show-sdk-path)" -mmacosx-version-min=$MAC_MIN \
      "${COMMON_CFLAGS[@]}" -Iios/src \
      "$ROOT/ios/src/smoke_test.c" "$OUT/macos/libgphoto2.a" \
      -liconv -o "$OUT/smoke_test"
    "$OUT/smoke_test"
    ;;
  ios)
    compile_slice ios -arch arm64 -isysroot "$(xcrun --sdk iphoneos --show-sdk-path)" -miphoneos-version-min=$IOS_MIN
    ;;
  sim)
    # Universal simulator slice (arm64 for Apple-Silicon Macs, x86_64 for Intel Macs).
    compile_slice sim -arch arm64 -arch x86_64 -isysroot "$(xcrun --sdk iphonesimulator --show-sdk-path)" -mios-simulator-version-min=$IOS_MIN
    ;;
  xcframework)
    "$0" ios
    "$0" sim
    echo ">>> assembling headers"
    HDR="$OUT/Headers"
    rm -rf "$HDR"; mkdir -p "$HDR/gphoto2"
    cp "$ROOT"/gphoto2/*.h "$HDR/gphoto2/"
    cp "$ROOT"/libgphoto2_port/gphoto2/*.h "$HDR/gphoto2/"
    cp "$ROOT"/ios/src/gp_ios_register.h "$HDR/"
    cp "$ROOT"/ios/src/gp_iccamera.h "$HDR/"
    rm -rf "$OUT/libgphoto2.xcframework"
    xcodebuild -create-xcframework \
      -library "$OUT/ios/libgphoto2.a" -headers "$HDR" \
      -library "$OUT/sim/libgphoto2.a" -headers "$HDR" \
      -output "$OUT/libgphoto2.xcframework"
    echo ">>> created $OUT/libgphoto2.xcframework"
    ;;
  clean)
    rm -rf "$OUT"; echo "cleaned $OUT"
    ;;
  *)
    echo "usage: $0 {test|ios|sim|xcframework|clean}"; exit 2
    ;;
esac
