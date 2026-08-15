# Canon camera compatibility — what works on any body, what's per-body

How much of what you can do with the R50 / R50 V transfers to *other* Canon cameras? Most of it.
The split is: a large **generic layer** that works on any Canon EOS out of the box, and a small
**per-body layer** (semantic decoding of a few opaque struct properties) that needs discovery.

## The generic layer — works on ANY Canon EOS

libgphoto2's `ptp2` driver supports the entire Canon EOS line. Connect any EOS body and you
immediately get, with **no per-body code**:

- **Full identity / DeviceInfo** — model, firmware, serial, vendor extension, and the complete
  list of supported **operations** (commands) and properties. (R50 V: **169 operations**.)
- **The named config tree** — read *and* write. (R50 V: **85 settings** — `iso`, `aperture`,
  `shutterspeed`, `whitebalance`, `imageformat`, `drivemode`, `focusmode`, `colortemperature`,
  `meteringmode`, `exposurecompensation`, `picturestyle`, `capturetarget`, `moviesize`, …) via
  `gp_iccamera_list_config` / `get_config` / `set_config`.
- **All EOS device properties** — raw read/write by code. (R50 V: **272 properties**) via
  `gp_iccamera_get_eosprop` / `set_eosprop` / `eos_props_dump`.
- **Capture, live view (EVF), autofocus, the event stream** — the EOS logic in `ptp2` is largely
  model-agnostic (`gp_iccamera_capture`, `liveview_*`, `af`, `poll_events`, …).
- **Every discovery tool** built here works on any EOS: `capabilities`, `eos_props_dump`,
  `watch_prop_changes`, `poll_events`, `get/set_eosprop`.

So the answer to "can I sift through any other Canon's details, settings, and commands the way I
can with the R50?" is **yes** for EOS bodies — the same functions, no new code.

## The per-body layer — needs discovery

A few settings are opaque **struct** properties whose code and layout Canon varies across bodies.
These are the only things that needed reverse-engineering, and a new body may differ:

| Feature | R50 | R50 V | A new body might… |
|---|---|---|---|
| Movie recording size | `0xD20D` (40-byte) | `0xD29E` (32-byte) | reuse one of these, or use a third property |
| Still size in pixels (L/M/S → WxH) | per-body table | per-body table | need its own table |

The **resolution codes** (0=FHD, 5=4K) and the **packing** happened to match across R50/R50 V, so
the app-facing API is identical — but the *carrier property* differs. `gp_iccamera_capabilities`
detects which carrier a body uses (or reports "carrier UNKNOWN"), and if it's new, the same
workflow that cracked the R50/R50 V handles it (see `canon-movie-recording-size.md`).

## The probe: `gp_iccamera_capabilities`

One call self-describes *any* connected camera — no per-body knowledge required:

```c
typedef enum { GP_ICCAMERA_CLASS_UNKNOWN=0, GP_ICCAMERA_CLASS_CANON_LEGACY=1,
               GP_ICCAMERA_CLASS_CANON_EOS=2 } gp_iccamera_class;
int gp_iccamera_capabilities(gp_iccamera *, char *out, int outlen);  /* returns the class */
```

It writes: identity (model/firmware/serial/vendor), **class**, **feature detection** (remote
capture, live view, AF, movie switch, image format, and which movie-size carrier), and the full
**operations** list. Returns the class enum.

```swift
var buf = [CChar](repeating: 0, count: 32768)
let cls = gp_iccamera_capabilities(cam, &buf, 32768)
print(String(cString: buf))
switch cls {
case 2: break            // Canon EOS — full support
case 1: break            // PowerShot / legacy Canon — config tree only
default: break           // non-Canon / generic PTP
}
```

Example report shape (R50 V):

```
== Canon EOS R50 V ==
manufacturer: Canon.Inc   firmware: 3-1.4.0
vendor ext: 0x0000000b   ops: 169   events: 20   std props: 5   EOS props: 272

CLASS: Canon EOS (full support: config tree, capture, live view, events)

FEATURES:
  remote capture ......... yes
  live view (EVF) ........ yes
  autofocus (DoAf) ....... yes
  image format (RAW/…) ... yes (0xD120)
  movie recording size ... yes: 0xD29E (MovieParam6, R50 V-style 32-byte struct) — decoded

OPERATIONS (169):
  1001  GetDeviceInfo
  9110  EOS_SetDevicePropValueEx
  9153  EOS_GetViewFinderData
  ...
```

## Per class — what to expect

- **Canon EOS** (`class 2`) — everything above. The vast majority of modern Canon (R-series, most
  EOS DSLRs/mirrorless). Fully self-describing; only the opaque struct props may need per-body mapping.
- **Canon legacy / PowerShot** (`class 1`) — older non-EOS bodies use a different driver path (no
  EOS event model). The named config tree still works (`imagequality`, `imageformat`, `imagesize`),
  but the EOS-specific tools (`eos_props_dump`, `get_eosprop`, EVF) don't apply.
- **non-Canon PTP** (`class 0`) — DeviceInfo + libgphoto2's generic/other-vendor config tree work;
  the Canon tools don't.

## Workflow for an unknown Canon

1. `gp_iccamera_capabilities` → class + features + which movie carrier (or "unknown").
2. `list_config` / `get_config` → its 80-ish named settings, ready to read/drive immediately.
3. `eos_props_dump` → its full raw property set.
4. Only if a specific opaque setting (e.g. movie size on a body with a new carrier) matters:
   `watch_prop_changes` + the full-state diff, then add a decode case — the exact process used for
   the R50 V.
