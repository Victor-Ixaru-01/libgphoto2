# Canon movie recording size — MovieParam5/6

The Canon EOS **movie recording size** (resolution + frame rate) is carried by a body-specific
device property holding a **struct** (not a scalar), which libgphoto2 shipped as `UNDEF` and never
decoded:

| Body | Carrier | Struct |
|---|---|---|
| **EOS R50** | `MovieParam5` / **`0xD20D`** | 40 bytes; fps at word 1 |
| **EOS R50 V** | `MovieParam6` / **`0xD29E`** | 32 bytes; **actual** fps at word 7 |

Both **read and set are on-body verified** (from-source libgphoto2 build + `gphoto2 --debug`,
raw property read at multiple settings, then set over USB with a `moviesize` test config widget).
The resolver packs both into one `u32` for the config/bridge layer — `(rescode<<16)|(actfps*100)` —
so the app side is identical for both bodies.

## The struct

`0xD20D` is 40 bytes = ten little-endian `u32` words. Captured on the R50:

| Setting | w0 (size) | **w1** | **w2** | w3 | w4 | w7 | w8 |
|---|---|---|---|---|---|---|---|
| 4K UHD 25.00p | 40 | **2500** | **5** | 3 | 1 | 2 | 10 |
| FHD 50.00p | 40 | **5000** | **0** | 3 | 1 | 2 | 10 |
| FHD 25.00p | 40 | **2500** | **0** | 3 | 1 | 2 | 10 |
| FHD 100.00p | 40 | **10000** | **0** | 3 | 1 | 2 | 10 |

- **w0** (0x00) = struct size (always 40 = `0x28`). *This is all a naive UINT32 decode ever saw —
  which is why `0xD20D` looked constant and got wrongly dismissed as a false lead.*
- **w1** (0x04) = **frame rate × 100** — generic (`fps = w1/100`).
- **w2** (0x08) = **resolution code** — `0` = 1920×1080, `5` = 3840×2160.
- w3,w4 = `{3,1}` — constant across every recording size (codec/audio, not size).
- **w7,w8 track HDR PQ**, not recording size. Toggling HDR PQ (`0xD20C`) on an R50 with resolution
  and frame rate held fixed moves them `{2,10}` → `{1,8}`: **w8 is the bit depth** (10-bit for
  HDR PQ, 8-bit without) and w7 looks like a gamma / colour-space flag (2 = PQ, 1 = standard).
  Note the camera's own option list carries `w7=1, w8=8` on *every* entry — the option list does
  not enumerate the HDR variant, only the current value reflects it. So do not read w7/w8 from the
  options. (w8 as bit depth is inference from one 10→8 transition matching HDR PQ's known depths.)
  See [canon-movie-remaining-time.md](canon-movie-remaining-time.md).
- The earlier `enum[6]={0x28,0x9c4,0x5,0x3,0x1,0x0}` was just the first 6 words misread as an
  options list — ignore it.

### R50 V — `0xD29E`, 32 bytes (8 words)

| Setting | w0 (size) | w1 (nom fps) | **w2** | w3 | w4 | **w7** |
|---|---|---|---|---|---|---|
| FHD 23.98p | 32 | 2400 | **0** | 3 | 1 | **2398** |
| FHD 59.94p | 32 | 6000 | **0** | 3 | 1 | **5994** |
| 4K 23.98p | 32 | 2400 | **5** | 3 | 1 | **2398** |

- **w2** (0x08) = **resolution code** — *same codes as the R50* (0 = FHD, 5 = 4K).
- **w7** (0x1C) = **actual fps × 100** — the R50 V (NTSC) reports fractional rates (2398 = 23.98,
  5994 = 59.94); w1 is the rounded nominal (2400, 6000). The decode uses **w7**.
- On set, the driver rebuilds the struct and computes `nom = round(actfps/100)*100` for w1.
  Confirmed working for rates never captured (e.g. 29.97p → nominal 3000).

## Driver + bridge changes

- **`ptp-pack.c`** (`ptp_unpack_EOS_events`, special-handling switch): decode `MovieParam5` into a
  `u32` the config layer can use — `packed = (w2 << 16) | (w1 & 0xffff)`.
- **`ptp.c`** (`ptp_canon_eos_setdevicepropvalue`): reverse it — rebuild the 40-byte struct from
  `packed`, filling the constant words `{3,1,2,10}` from the R50 template.
- **`ios/src/gp_canon_moviesize.{c,h}`**: `packed` ↔ `{width,height,fps_x100}`, per body
  (resolution-code table; fps is generic).
- **`gp_iccamera_get_movie_size()` / `gp_iccamera_set_movie_size()`**: read/select over the bridge.

> These edit shared driver code (`ptp-pack.c`, `ptp.c`), not just the iOS shim — keep in mind on
> upstream rebases.

## API (Swift)

```c
uint16_t gp_canon_moviesize_prop(const char *model);   /* 0xD20D on R50, 0xD29E on R50 V; 0 = unknown */
int gp_canon_moviesize_decode(const char *model, uint32_t packed, gp_canon_movsize *out);
int gp_canon_moviesize_encode(const char *model, int w, int h, int fps_x100, uint32_t *packed);

int gp_iccamera_get_movie_size(gp_iccamera *, uint32_t *code, int *w, int *h, int *fps_x100,
                               char *label, int labellen, uint32_t *choices, int cap, int *nch);
int gp_iccamera_set_movie_size(gp_iccamera *, uint32_t code);   /* code = packed value */
```

```swift
// READ
var code: UInt32 = 0, w: Int32 = 0, h: Int32 = 0, fps: Int32 = 0
var lbl = [CChar](repeating: 0, count: 64)
if gp_iccamera_get_movie_size(cam, &code, &w, &h, &fps, &lbl, 64, nil, 0, nil) == 0 {
    print("\(w)×\(h) @ \(Double(fps)/100)p — \(String(cString: lbl))")   // e.g. 3840×2160 @ 25.0p
}

// LIST the valid sizes for a picker (the camera's own availlist, per body + region)
var choices = [UInt32](repeating: 0, count: 32)
var n: Int32 = 0
_ = gp_iccamera_get_movie_size(cam, &code, nil, nil, nil, nil, 0, &choices, 32, &n)
let sizes: [(w: Int, h: Int, fps: Double, packed: UInt32)] = (0..<Int(n)).compactMap {
    var m = gp_canon_movsize(); let p = choices[$0]
    guard gp_canon_moviesize_decode(model, p, &m) != 0 else { return nil }  // known resolution
    return (Int(m.width), Int(m.height), Double(m.fps_x100)/100, p)
}
// e.g. R50 V → [(3840,2160,59.94), (3840,2160,29.97), (3840,2160,23.98),
//               (1920,1080,119.88), (1920,1080,59.94), (1920,1080,29.97), (1920,1080,23.98)]

// SET (pick a `packed` from the list above, or build one from a desired size)
var packed: UInt32 = 0
gp_canon_moviesize_encode(model, 1920, 1080, 5000, &packed)   // FHD 50.00p
let rc = gp_iccamera_set_movie_size(cam, packed)              // 0 = ok, -2 = body TBD

// UPDATE: poll_events fires "PROP d20d" (R50) or "PROP d29e" (R50 V) when it changes on-camera.
```

## The available-sizes list

`get_movie_size`'s `choices` return the camera's **own list of valid recording sizes** — parsed
from the property's AvailList event, so it's always correct for the connected body *and region*
(the R50 in PAL vs the R50 V in NTSC advertise different rates). Each availlist entry is one struct
(same layout as the value); the driver packs each to `(rescode<<16)|(fps*100)`, so a `choices[i]`
decodes with `gp_canon_moviesize_decode` exactly like the current value.

Verified on the **R50 V** (7 modes): `4K 59.94/29.97/23.98p`, `FHD 119.88p (HFR)`,
`FHD 59.94/29.97/23.98p`. (libgphoto2 previously misparsed this list as 6–7 scalar "options" that
were really the first words of the first struct — now fixed for `MovieParam5/6`.)

## Status

Both bodies: **read + set DONE, on-body verified** over USB (from-source build + a `moviesize` test
config widget). Each set builds the body's struct, the camera returns `RC_OK` and pushes a
`value changed` event, and a read-back reflects it.

- **R50** (`0xD20D`): FHD 100p → 4K 25p → FHD 50p → FHD 25p, each confirmed.
- **R50 V** (`0xD29E`): 4K 23.98p ⇄ FHD 59.94p and 4K 23.98p → FHD 29.97p (a rate never captured —
  the nominal-rounding encode built the right struct), each confirmed.

### Caveat — HFR locks the recording size (camera-side)

On the R50, **FHD 100.00p is High-Frame-Rate (HFR) mode**, and while HFR is active the camera
**refuses to change** to any other resolution/frame rate. A `set_movie_size` in that state returns
`RC_OK` but is a **silent no-op** — the camera stays in HFR. This is a Canon constraint, not a
driver bug. **Always read back after a set** (`get_movie_size`) to confirm it took effect, and treat
"unchanged" as "blocked by the current mode" (HFR, or recording in progress). To leave HFR you must
turn it off on the camera first.

## How the R50 V carrier was found

`0xD20D` is absent on the R50 V, so its carrier was discovered by diffing the **full property
state** (scalars *and* struct hex) across changes — the same technique that decoded `0xD20D`:

1. Capture full state (fps-only change: FHD 23.98p → 59.94p) → `d29e` word 7 moved 2398 → 5994
   (and `d210`/scalars, red herrings). 
2. Capture full state (resolution-only change: FHD 23.98p → 4K 23.98p) → `d29e` word 2 moved 0 → 5,
   and `d210` did **not** move. Only `d29e` tracks both axes → it's the carrier.

Then the same wiring as the R50: a `MovieParam6` decode/set case in `ptp-pack.c`/`ptp.c`, and the
`"r50 v"` row in `gp_canon_moviesize.c` pointed at `0xD29E`. Both are now done and verified.

For a **third body**, repeat: `gp_iccamera_watch_prop_changes` (or the full-state diff) while
changing the movie rec size, find the property that tracks *both* resolution and fps, add its
struct decode/set case, and point its `gp_canon_moviesize` row at it.
