# libgphoto2 for iOS — Phase 1 build

This directory builds a curated **libgphoto2 core + `libgphoto2_port` + `camlibs/ptp2`**
into an `.xcframework` for iOS (device + simulator, arm64), using **static registration
instead of `dlopen`** (iOS forbids loading app-external code). It is the engine half of
the iOS Canon framework — the transport (ImageCaptureCore) is wired in Phase 2.

See [../docs-architecture/ios-framework-plan.md](../docs-architecture/ios-framework-plan.md).

## Status: ✅ built & validated

- Native macOS smoke test passes: **2471 camera models** register via the static loader,
  all Canon EOS R-series present with `lib=ptp2`, `gp_camera_new` works.
- `ios-arm64` and `ios-arm64-simulator` static slices build clean (warnings only).
- All key symbols present in the archive: `gp_camera_new`, `camera_init`,
  `camera_abilities`, `ptp_opensession`, `ptp_canon_eos_getevent`, `gp_ios_register_all`.

## Build

```bash
./ios/build.sh test         # native macOS build + run the smoke test (fast validation)
./ios/build.sh xcframework  # build both iOS slices → ios/build/libgphoto2.xcframework
./ios/build.sh clean
```

Requires Xcode command-line tools (`xcrun clang`, `xcrun libtool`, `xcodebuild`). No
autotools/meson/gettext needed — the build compiles a fixed source set directly.

## What's in the box (and what isn't)

**Compiled in:** all of `libgphoto2/` (core), the `libgphoto2_port/` core (port API,
info-list, log, portability, result, locking — but **no iolib backends**), and `ptp2`
(`ptp.c`, `library.c`, `usb.c`, `ptpip.c`, `config.c`, `chdk.c`, `fujiptpip.c`).

**Deliberately excluded / disabled** (see `ios/include/config.h`):

| Dropped | Why |
|---|---|
| libtool `ltdl` | No `dlopen` on iOS → replaced by `ios/src/ltdl_static.c` (a name→symbol registry). |
| `olympus-wrap.c` | Needs libxml2; Olympus-only, irrelevant to Canon. |
| libusb + the port usb/serial/disk iolibs | Transport is ImageCaptureCore, not libusb (Phase 2). |
| libexif | The EXIF-mtime path in `gphoto2-filesys.c` is compiled out. |
| gettext/NLS | `i18n.h` degrades `_()`/`N_()` to identity. |

## Files

| File | Role |
|---|---|
| `build.sh` | The build driver (curated clang compile → `libtool -static` → `xcodebuild -create-xcframework`). |
| `include/config.h` | Hand-written feature macros for the iOS/Darwin arm64 target. |
| `include/ltdl.h` | Drop-in replacement for libtool's `<ltdl.h>` (static loader API). |
| `src/ltdl_static.c` | The static module registry backing `lt_dlopenext`/`lt_dlsym`/`lt_dlforeachfile`. |
| `src/gp_ios_register.{c,h}` | Registers the `ptp2` camlib at startup. **Call `gp_ios_register_all()` once before any `gp_*_load`/`gp_camera_init`.** |
| `src/smoke_test.c` | Native validation harness. |
| (generated) `../libgphoto2/gphoto2-endian.h` | From the `.in`, all-`#undef` → the portable little-endian path. |

## Integrating into an app (preview of Phase 2)

1. Link `libgphoto2.xcframework` (already copied to `Canon Test/Vendor/`). It's a **static**
   library — add it under *Frameworks, Libraries, and Embedded Content* → **Do Not Embed**,
   and add **`-liconv`** to *Other Linker Flags* (used for MTP/UCS-2 string conversion).
2. In your bridging header / an `.m`/`.c`, `#include <gphoto2/gphoto2.h>` and
   `#include "gp_ios_register.h"`.
3. Call **`gp_ios_register_all()`** once at launch.
4. (Phase 2) register a custom "iccamera" iolib whose `read`/`write` bridge to
   `ICCameraDevice.requestSendPTPCommand`, and point `PTPParams`' transport at it.

At that point `gp_camera_init` → the ptp2 Canon EOS stack runs on top of the transport,
giving you the full config tree / capture / live-view engine documented in
[../docs-architecture/canon-cameras.md](../docs-architecture/canon-cameras.md).

## Notes

- The static lib is ~1.8 MB per slice.
- `gphoto2-endian.h` is generated in-tree; the build regenerates nothing else.
- Bitcode is not emitted (deprecated by Apple); fine for current toolchains.
- To add another camlib later, compile it and add its `camera_id/abilities/init` to the
  registry in `gp_ios_register.c` (that's the whole "no-dlopen" adaptation).
