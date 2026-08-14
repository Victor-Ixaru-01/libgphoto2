# Canon image-size resolver — Swift integration guide

Maps a Canon EOS **image-format label** (`RAW`, `cRAW`, `L`, `cRAW + L`, …) to the
**output pixel dimensions / megapixels**, per camera body and aspect ratio.

libgphoto2 / ptp2 only ever reports the still size **symbolically** — `L / M / S / M1 /
M2 / S1 / S2 / S3` for JPEG and `RAW / cRAW / mRAW / sRAW` for raw. It never reports the
pixel count for a class, and Canon EOS bodies usually leave `ImagePixWidth/Height == 0`
in the captured object's `ObjectInfo` (the real dimensions live only in the file's EXIF,
which libgphoto2 does not parse). This module carries a **hardcoded per-body table** so
your app can show megapixels **before** a frame exists.

- Source: [`ios/src/gp_canon_imagesize.h`](../ios/src/gp_canon_imagesize.h) ·
  [`ios/src/gp_canon_imagesize.c`](../ios/src/gp_canon_imagesize.c)
- Test: [`ios/src/smoke_test.c`](../ios/src/smoke_test.c) → `check_imagesize()`
- Ships inside `libgphoto2.xcframework` (both slices' `Headers/`).

---

## 1. Build & location

```bash
./ios/build.sh xcframework    # both iOS slices + package
```

Output:

```
ios/build/libgphoto2.xcframework
├── Info.plist
├── ios-arm64/                        # device
│   ├── libgphoto2.a
│   └── Headers/  gp_canon_imagesize.h  gp_iccamera.h  gp_ios_register.h  gphoto2/…
└── ios-arm64_x86_64-simulator/       # simulator (arm64 + x86_64)
    ├── libgphoto2.a
    └── Headers/  gp_canon_imagesize.h  …
```

Absolute path in this checkout:

```
/Volumes/Extended-2TB/GitHub/libgphoto2/.claude/worktrees/libgphoto2-docs-architecture-a36109/ios/build/libgphoto2.xcframework
```

Fast validation without packaging (runs the resolver unit checks natively):

```bash
./ios/build.sh test           # prints "SMOKE TEST OK" only if every resolver case passes
```

---

## 2. C API

```c
typedef struct { int width; int height; } gp_canon_pixsize;

/* Resolve every file a Canon "imageformat" setting produces.
 *   model   camera model ("Canon EOS R50"); "Canon"/"EOS"/punctuation ignored.
 *   label   the imageformat value, single ("RAW") or dual ("cRAW + L").
 *   aspect  the aspectratio value ("3:2","4:3","16:9","1:1","1.6x"); NULL/"" = native.
 *   out     caller array >= cap; up to 2 filled (RAW+JPEG dual).
 * Returns file count (0..2); each out[i] is {0,0} when body/class is unknown.
 * Returns negative only on a usage error (NULL args / cap < 1). */
int gp_canon_imagesize_resolve(const char *model, const char *label, const char *aspect,
                               gp_canon_pixsize *out, int cap);

/* Non-zero if this body is in the table (i.e. resolve() can return real numbers). */
int gp_canon_imagesize_known_body(const char *model);

/* Output megapixels (width*height / 1e6). Inline in the header. */
static inline double gp_canon_pixsize_megapixels(gp_canon_pixsize s);
```

It is **pure computation** — no camera transport, no `gp_iccamera` handle, no threading
constraints. Call it from any thread.

---

## 3. Wiring it up in Swift

### 3a. Make the header visible

The header is inside the xcframework's `Headers/`. Expose the C symbols to Swift via your
**Objective-C bridging header** (or a module map if you already have one):

```objc
// YourApp-Bridging-Header.h
#import "gp_canon_imagesize.h"
#import "gp_iccamera.h"     // if not already imported
```

### 3b. The three inputs

| Argument | Where it comes from |
|---|---|
| `model` | `ICCameraDevice.name` — already exactly `"Canon EOS R50"` etc. |
| `imageFormatLabel` | `gp_iccamera_get_config(cam, "imageformat", …)` |
| `aspect` | `gp_iccamera_get_config(cam, "aspectratio", …)` — may be absent on some bodies → pass `nil` |

`gp_iccamera_get_config` must be called on your **background** camera thread (see
`gp_iccamera.h`). The resolver call itself has no such restriction.

### 3c. A small Swift wrapper

```swift
import Foundation

struct CanonImageSize: Equatable {
    let width: Int
    let height: Int
    var megapixels: Double { Double(width * height) / 1_000_000 }
    var isKnown: Bool { width > 0 && height > 0 }
    var display: String { isKnown ? "\(width)×\(height)" : "—" }
    var mpDisplay: String { isKnown ? String(format: "%.1f MP", megapixels) : "—" }
}

enum CanonImageSizeResolver {

    /// Resolve the output size(s) for a given imageformat label.
    /// - model: ICCameraDevice.name, e.g. "Canon EOS R50"
    /// - imageFormatLabel: the "imageformat" config value, e.g. "cRAW + L"
    /// - aspect: the "aspectratio" config value, e.g. "4:3"; nil = the body's native aspect
    /// Returns one entry per file (1 for single, 2 for RAW+JPEG). An entry with
    /// isKnown == false means the body/class isn't in the table → fall back to EXIF.
    static func resolve(model: String,
                        imageFormatLabel: String,
                        aspect: String?) -> [CanonImageSize] {
        var out = [gp_canon_pixsize](repeating: .init(width: 0, height: 0), count: 2)
        let n = gp_canon_imagesize_resolve(model, imageFormatLabel, aspect, &out, 2)
        guard n > 0 else { return [] }
        return (0..<Int(n)).map {
            CanonImageSize(width: Int(out[$0].width), height: Int(out[$0].height))
        }
    }

    static func isKnownBody(_ model: String) -> Bool {
        gp_canon_imagesize_known_body(model) != 0
    }
}
```

### 3d. End-to-end from the config tree

```swift
// `cam` is your OpaquePointer wrapping gp_iccamera*; call on the camera's background queue.
func currentImageSizes(cam: OpaquePointer, model: String) -> [CanonImageSize] {
    let format = readConfigValue(cam, "imageformat")   // e.g. "cRAW + L"
    let aspect = readConfigValue(cam, "aspectratio")    // e.g. "4:3" (or nil)
    guard let format else { return [] }
    return CanonImageSizeResolver.resolve(model: model,
                                          imageFormatLabel: format,
                                          aspect: aspect)
}

/// Thin wrapper over gp_iccamera_get_config that returns just the current value.
func readConfigValue(_ cam: OpaquePointer, _ name: String) -> String? {
    var value = [CChar](repeating: 0, count: 256)
    var choices = [CChar](repeating: 0, count: 1024)
    let rc = gp_iccamera_get_config(cam, name, &value, 256, &choices, 1024)
    guard rc == 0 else { return nil }                    // setting absent on this body
    let s = String(cString: value)
    return s.isEmpty ? nil : s
}
```

Usage in UI:

```swift
let sizes = currentImageSizes(cam: cam, model: device.name ?? "")
// "cRAW + L" @ 4:3 on an R50 →
//   sizes[0] = 6000×4000  24.0 MP   (RAW, full sensor)
//   sizes[1] = 5328×4000  21.3 MP   (JPEG, cropped to 4:3)
let summary = sizes.map { "\($0.display) (\($0.mpDisplay))" }.joined(separator: "  +  ")
```

---

## 4. Behavior you need to know

1. **Compression never changes dimensions.** `cRAW ≡ RAW`, `cL ≡ L`, `cS2 ≡ S2`, … The
   leading `c` (Normal vs Fine JPEG) and RAW-vs-cRAW are compression only — same pixels,
   different bytes. So the resolver reports identical sizes for `RAW` and `cRAW`.

2. **RAW is aspect-invariant.** Canon records the **full sensor** for `RAW/cRAW/mRAW/sRAW`
   regardless of the aspect-ratio setting; the aspect is stored as a crop hint inside the
   CR3, and only the **JPEG** is actually cropped. Verified on the R50: at 4:3, `RAW` is
   `6000×4000` while `L` is `5328×4000`. (A true **sensor** crop like the R5's `1.6x`
   *does* shrink RAW — that's handled by an explicit override, not derivation.)

3. **Non-native aspect ratios are derived by cropping** the native dimensions, except
   where an exact override exists. Derived values can be a few pixels off Canon's own
   rounding; measured overrides are exact. `1.6x` (sensor crop) has no derivation — it
   needs an override or it returns `{0,0}`.

4. **Unknown body/class → `{0,0}`.** Use that (or `isKnownBody`) as the signal to fall
   back to reading the file's EXIF, or to the reliably-populated `ObjectSize` (bytes) from
   the capture event.

5. **Dual formats** (`RAW + JPEG`) return two entries, in label order — `out[0]` for the
   first component, `out[1]` for the second.

---

## 5. The body table & how to extend it

Bodies currently seeded (in `gp_canon_imagesize.c`):

| Body | Native | Notes |
|---|---|---|
| EOS 5D Mark III | 3:2 | full mRAW/sRAW + S1/S2/S3 (spec values) |
| EOS R5 | 3:2 | + `1.6x` sensor-crop override |
| EOS R50 | 3:2 | 3:2 = spec; **4:3 measured on-body** |
| EOS 90D | 3:2 | APS-C (spec values) |

> ⚠️ Values marked "spec" are Canon's published numbers and carry a
> **`VERIFY BEFORE SHIPPING`** banner in the source — confirm on-body where you can.
> Only the R50 **4:3** row is on-body verified so far.

To add a body: normalize its model the way `normalize_model()` does (`"Canon EOS 90D"` →
`"90d"`), add a `sizerow[]` (list the base classes at the native aspect — `cRAW`/`cL`/…
reuse them automatically), optionally an `overriderow[]` for measured/crop aspects, then
one line in `kBodies[]`. Native ratio is inferred from the `SC_L` (or `SC_RAW`) row.

### R50 — measured reference (4:3, on-body)

| Class | 4:3 | MP | | Class | 3:2 (spec) | MP |
|---|---|---|---|---|---|---|
| RAW / cRAW | 6000×4000 | 24.0 | | RAW / cRAW | 6000×4000 | 24.0 |
| L | 5328×4000 | 21.3 | | L | 6000×4000 | 24.0 |
| M | 3552×2664 | 9.5 | | M | 3984×2656 | 10.6 |
| S1 | 2656×1992 | 5.3 | | S1 | 2976×1984 | 5.9 |
| S2 | 2112×1600 | 3.4 | | S2 | 2400×1600 | 3.8 |

(3:2 column is Canon spec, not yet on-body verified — shoot the R50 at 3:2 to confirm.)

---

## 6. Regression guard

`check_imagesize()` in `smoke_test.c` asserts the R50 4:3 numbers, the RAW-invariance
rule, a dual-format case, a derived 16:9 crop, the R5 `1.6x` override, and known-body
detection. It runs on every `./ios/build.sh test`; a mismatch fails the build (non-zero
exit under `set -e`) instead of printing `SMOKE TEST OK`.
