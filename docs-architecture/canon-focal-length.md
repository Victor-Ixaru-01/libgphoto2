# Canon focal length — it isn't a property, it's a live-view record

There is **no device property on any Canon EOS body that reports the current focal length.**
The value only exists inside the live-view (EVF) buffer, in a record type libgphoto2 used to
throw away. This note covers how that was established, what now decodes it, and what the Swift
side calls.

Investigated on an **EOS R50** (USB `04a9:330d`) with an **RF-S18-45mm F4.5-6.3 IS STM**.

## What the camera actually reports

Dumping every Canon EOS device property — 88 values, in live view and out — turns up **neither
`18` nor `45`**, at either end of the zoom range. The only place those numbers appear is inside
the lens *name*:

| Source | Value | Tracks zoom? |
|---|---|---|
| `0xD1D8` `EOS_LensName` → `/main/status/lensname` | `"RF-S18-45mm F4.5-6.3 IS STM"` | **No** — nominal range, baked into the model name |
| EVF record **`0x21`** (33) | `18` → `31` → `45` | **Yes** — the live value, in mm |

The lens-name property is worth ruling out explicitly, because a 63-byte payload for a 27-character
string looks like it hides something. It doesn't:

```
4b 00 00 00  89 c1 00 00  d8 d1 00 00  52 46 2d 53 31 38 2d 34 35 6d 6d ...
len=75       code=0xc189  DPC=0xD1D8   "RF-S18-45mm F4.5-6.3 IS STM"
                                       27 chars + NUL + 35 zero bytes
```

Everything after the string terminator is zero padding. Nothing numeric behind it.

Contrast Nikon, which *does* publish the range as numbers — `minfocallength` / `maxfocallength`
(`0xD0E3` / `0xD0E4`, [config.c:11947](../camlibs/ptp2/config.c#L11947)). **Canon has no
equivalent, for the current value or for the range.**

## EVF record 0x21

The live-view buffer is a sequence of `[u32 len][u32 type][payload]` records. Record type 33 is
twelve bytes holding one little-endian u32 — the focal length in mm:

```
0c 00 00 00   21 00 00 00   12 00 00 00      → 18 mm
0c 00 00 00   21 00 00 00   1f 00 00 00      → 31 mm
0c 00 00 00   21 00 00 00   2d 00 00 00      → 45 mm
len = 12      type = 0x21   u32 = mm
```

Verified on hardware at both stops **and at an intermediate 31 mm** — so it is a genuine focal
length, not a zoom-step index that happens to line up at the extremes. The record is present in
**every** frame, so it never goes stale.

### Record 0x20 is *not* a 0–100 zoom position

Worth recording, because it looks tempting. Record type 32 also moves with the zoom, but its
fields are neither millimetres nor a normalised position:

| Focal | payload | byte[1] | byte[3] | byte[5] |
|---|---|---|---|---|
| 18 mm | `00 10 ff 04 ff ff 00 00 00 14 00 00` | 16 | 4 | 255 |
| 31 mm | `00 16 ff 15 ff 06 00 00 00 14 00 00` | 22 | 21 | 6 |
| 45 mm | `00 2d ff 2c ff 16 00 00 00 14 00 00` | 45 | 44 | 22 |

A 0–100 position would have to read 0 at the wide stop and 100 at tele. Use record `0x21`.

The 0–100 range that gets associated with Canon zoom is the **power-zoom property**, `0xD055`
`EOS_PowerZoomPosition` — a different thing entirely (see below).

## Power zoom (PZ lenses) — already supported, untested

For electronically-driven lenses such as the RF 20-50mm F4L IS USM PZ, libgphoto2 **already has
both config nodes**; no new code is needed:

| Config | Property | Access |
|---|---|---|
| `/main/capturesettings/zoom` | `0xD055` `EOS_PowerZoomPosition` | get + **set** (RANGE widget) |
| `/main/capturesettings/zoomspeed` | `0xD149` `EOS_PowerZoomSpeed` | get + set |

[config.c:12100](../camlibs/ptp2/config.c#L12100) and
[config.c:12102](../camlibs/ptp2/config.c#L12102). The range is **not** hardcoded to 0–100:
`_get_Canon_EOS_ZoomRange` builds it as `0 → largest value in the camera's enumeration for
0xD055`, falling back to `0–1000` when the camera enumerates nothing.

`zoom` does **not** appear in the config tree with a manual lens mounted. The R50 sends only a
*descriptor* for `0xD055` and never a value record:

```
event 307:c18a: prop d055 options changed, type 3, count 1 (EOS_PowerZoomPosition) (unknown)
```

`have_eos_prop()` ([ptp-private.h:142](../camlibs/ptp2/ptp-private.h#L142)) matches only
properties present in `params->canon_props`, which is populated from value records — so the
widget stays hidden until a PZ lens supplies a position. `zoomspeed` *is* present today
(`Readonly: 0`, `Current: 0`).

> **Known gap.** `PTP_OC_CANON_EOS_DrivePowerZoom` (`0x914D`) **is advertised by the R50** but
> libgphoto2 never calls it — the opcode appears only in the name table at
> [ptp.c:9391](../camlibs/ptp2/ptp.c#L9391). The driver moves PZ purely by setting `0xD055`.
> Canon's own SDK uses `0x914D` for direction/speed-controlled drive. If property-setting turns
> out jerky or ignores `zoomspeed` on a real PZ lens, that opcode is the unexplored path; it would
> need a `ptp_canon_eos_drivepowerzoom()` wrapper and an action config.
>
> **None of this section is hardware-verified** — it is read from the code and from the R50's
> descriptor records. No PZ lens was available.

## What changed

### camlib — decode the record, expose a config

- **[ptp.h:4315](../camlibs/ptp2/ptp.h#L4315)** — new `PTPParams.canon_evf_focallength`
  (`uint32_t`, mm). `0` means "no frame seen yet".
- **[library.c:3595](../camlibs/ptp2/library.c#L3595)** — `canon_eos_evf_record()` decodes record
  `0x21`. Called from **both** EVF loops
  ([3756](../camlibs/ptp2/library.c#L3756) and [3788](../camlibs/ptp2/library.c#L3788)) — the
  records before the JPEG and the ones after it are walked by separate loops, so a single hook
  would miss half the buffer depending on record order.
- **[library.c:3613](../camlibs/ptp2/library.c#L3613)** — `canon_eos_evf_refresh_metadata()`
  pulls one frame purely for metadata, discarding the image. Used when the cache is cold. Bounds
  checks every record.
- **[ptp-private.h:51](../camlibs/ptp2/ptp-private.h#L51)** — declaration for the above.
- **[config.c:5133](../camlibs/ptp2/config.c#L5133)** — `_get_Canon_EOS_FocalLength()`, and the
  table row at **[config.c:12164](../camlibs/ptp2/config.c#L12164)**. The row uses `propid = 0`
  with `PTP_OC_CANON_EOS_GetViewFinderData` in the type slot, so the node only appears on bodies
  that support live view. `_put_None` makes it read-only.

Result: `/main/capturesettings/focallength`, formatted `"45 mm"`, next to the existing Olympus and
generic-PTP rows of the same name.

```
$ gphoto2 --capture-preview --get-config focallength
Label: Focal Length
Readonly: 1
Type: TEXT
Current: 45 mm
```

Without live view it reads `unknown (liveview not active)`.

### iOS bridge — because the camlib hook never runs in the app

**This is the part that matters for the app.** The bridge fetches EVF frames itself with
`ptp_canon_eos_get_viewfinder_image()` in `gp_iccamera_liveview_frame()` — it does **not** go
through `gp_camera_capture_preview()`. So the camlib's own parse hook is never reached on iOS, and
without the change below the config would latch at its first value and never follow the zoom.

- **[gp_iccamera.c:65](../ios/src/gp_iccamera.c#L65)** — `evf_focallength` on the `gp_iccamera`
  struct, alongside the other per-frame EVF caches.
- **[gp_iccamera.c:688](../ios/src/gp_iccamera.c#L688)** — record `33` branch in the frame loop.
  It sets **both** the bridge cache and `params->canon_evf_focallength`, so the generic
  `focallength` config stays fresh too.
- **[gp_iccamera.c:792](../ios/src/gp_iccamera.c#L792)** — `gp_iccamera_get_focallength()`.
- **[gp_iccamera.h:142](../ios/src/gp_iccamera.h#L142)** — declaration.

## Swift-facing contract

Preferred — the dedicated accessor. It reads the value cached from the **last live-view frame**,
so it costs no PTP round-trip and is safe to call at frame rate:

```c
int gp_iccamera_get_focallength(gp_iccamera *, uint32_t *mm);
```

Returns `0` with `*mm` set, or `-1` before the first frame has arrived.

```swift
/// Current focal length in mm, or nil until the first live-view frame lands.
var focalLengthMM: Int? {
    var mm: UInt32 = 0
    guard gp_iccamera_get_focallength(handle, &mm) == 0 else { return nil }
    return Int(mm)
}
```

Typical use — refresh it right after each frame, next to the level/orientation reads:

```swift
// inside the live-view frame loop, after gp_iccamera_liveview_frame() returns 0
if let mm = focalLengthMM {
    await MainActor.run { self.focalLength = mm }   // e.g. overlay "45mm"
}
```

**Live view must be running.** Call `gp_iccamera_liveview_start()` first; the value updates on
every subsequent `gp_iccamera_liveview_frame()`. After `gp_iccamera_liveview_stop()` the last
value is retained rather than cleared — treat it as stale, not live.

The generic config path also works and returns a display-ready string (`"45 mm"`):

```c
gp_iccamera_get_config(icc, "focallength", value, vlen, choices, clen);
```

`choices` comes back empty (read-only TEXT widget). Prefer the dedicated accessor in the frame
loop — the config call walks the widget tree and will fetch a frame of its own if the cache is
cold.

### Focal *range*

There is no API for it, because the camera does not report it. The nominal range exists only as
text in `lensname`:

```swift
// "RF-S18-45mm F4.5-6.3 IS STM" → (18, 45)
func focalRange(fromLensName name: String) -> (min: Int, max: Int)? {
    guard let m = name.range(of: #"(\d+)-(\d+)mm"#, options: .regularExpression) else { return nil }
    let parts = name[m].dropLast(2).split(separator: "-").compactMap { Int($0) }
    return parts.count == 2 ? (parts[0], parts[1]) : nil
}
```

Note this only matches zoom lenses whose name carries a range; a prime reads e.g. `"RF50mm F1.8
STM"` and yields `nil`. The robust alternative is to rack the zoom to both stops and record
`gp_iccamera_get_focallength()` at each — which is only automatable on a PZ lens.

## Rebuilding the xcframework

```bash
./ios/build.sh test          # native smoke test — fast validation
./ios/build.sh xcframework   # both slices → ios/build/libgphoto2.xcframework
```

Then refresh `Vendor/libgphoto2.xcframework` in the Canon Test Xcode project.
`ios/build/` is gitignored, so the artefact is never committed here.

## Reproducing the investigation

Homebrew's `gphoto2` is built with `--disable-debug` (it comes from Homebrew's
`std_configure_args`), which compiles out every `gp_log`/`ptp_debug` call — `--debug` writes an
**empty** log and none of the above is visible. Build from the checkout instead, where debug is on
by default:

```bash
autoreconf -is
mkdir build && cd build
../configure --prefix=$PWD/../inst --with-camlibs=ptp2 --disable-nls
make -j8 && make install
```

Then point the stock CLI at it:

```bash
DYLD_LIBRARY_PATH=<inst>/lib \
CAMLIBS=<inst>/lib/libgphoto2/2.5.34.1 \
IOLIBS=<inst>/lib/libgphoto2_port/0.12.2 \
gphoto2 --debug --debug-loglevel=data --debug-logfile=out.log --capture-preview
```

`--debug-loglevel=data` is what produces the USB hexdumps; plain `debug` gives only decoded
property lines. Useful greps: `prop [0-9a-f]{4} value ==` for the EOS property table, and
`get_viewfinder_image header: len=… type=…` for EVF records.

> If you reuse that build directory after editing `ptp.h`, run `make clean` first. Configuring
> with `--disable-dependency-tracking` (as above minus the flag, or via Homebrew's args) means
> header edits don't trigger recompiles, and a half-rebuilt tree gives **mismatched `PTPParams`
> offsets across objects** — which shows up as a garbage value, not a build error.
