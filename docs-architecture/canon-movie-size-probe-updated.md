# Canon movie-size discovery probe

libgphoto2 does **not** decode Canon EOS movie resolution/framerate — the data lives in
`MovSize` (0xD1BB), `MovieParam`/`MovieParam2..5`, and `VariableMovieRecSetting` (0xD215),
but the driver only parses them as raw 32-bit values (see `ptp-pack.c`, "yet unknown 32bit
props"). `gp_iccamera_movie_size_probe()` dumps those candidate properties so the
resolution/framerate encoding can be reverse-engineered on a specific body (e.g. the R50) —
after which a labelled resolver + set/update helpers get built, mirroring the stills
[canon-imagesize-resolver.md](canon-imagesize-resolver.md).

## API

```c
/* Newline-separated dump of the candidate movie properties. 0 on success, <0 on usage error. */
int gp_iccamera_movie_size_probe(gp_iccamera *, char *out, int outlen);
```

Each line looks like:

```
D1BB MovSize     dt=UINT32 rw cur=0x00000003 enum[6]={0x0,0x1,0x2,0x3,0x4,0x5}
D1BE MovieParam  dt=UINT32 ro cur=0x0000000a (no form / single value)
D215 VarMovieRec dt=UINT32 rw cur=0x00000001 enum[2]={0x0,0x1}
...
```

- `dt` — datatype the driver assigned.
- `rw`/`ro` — whether the camera reports it as settable.
- `cur` — the current raw code.
- `enum[...]` — the **valid codes** the camera advertises (the gold for building the table);
  `range=[…]` for range-form props; `(no form)` when the camera doesn't enumerate.

Call it on the **background** camera thread, in **movie mode**, with live view running.

## Discovery procedure

1. Put the R50 in **video** mode (`gp_iccamera_get_movie_mode` → VIDEO) and start live view.
2. Call the probe and capture the whole dump — this is your **baseline**.
3. On the camera, change **only the resolution** (e.g. 4K → FHD). Probe again. Note which
   line's `cur` changed, and to what.
4. Back to the first resolution, then change **only the frame rate** (e.g. 25p → 50p). Probe
   again. Note which line's `cur` changed.
5. Repeat across the combinations you care about, recording `setting → (code, on which prop)`.

Send me that table (which property moved, and the `cur` code for each resolution/fps). Then
I'll add `gp_canon_moviesize` (code → `{width, height, fps}` per body) plus
`gp_iccamera_get_movie_size()` / `set_movie_size()` and the `poll_events` wiring, with a
smoke-test guard and a doc.

## How to read the result

- **One property's `cur` changes for both resolution and fps** → a single combined enum code.
  A scalar `set_eosprop(0xD1BB, code)` is enough; the resolver is a flat code→label table.
- **Resolution and fps move *different* properties** → we map both (e.g. size on `MovSize`,
  rate on a `MovieParam`), and the set path writes both.
- **The moving property has no `enum[...]`** (`(no form)`) → the camera doesn't advertise the
  list; the table is built purely from your observed `cur` values, and setting may be gated
  (some bodies only accept movie-size changes in specific states).
- **`ro` on the property you need** → it can't be set over PTP on this body; you'd change it
  on-camera only, and the app just reads/reflects it.

## Findings so far (R50 / R50 V)

The seven guessed movie DPCs are **not usefully populated** on either body:

```
# Canon EOS R50 (photo body, photo/video switch)     # Canon EOS R50 V (video body)
D1BB MovSize      (not reported)                      D1BB MovSize      (not reported)
D1BE MovieParam   (not reported)                      …all (not reported)…
…                                                     D215 VarMovieRec dt=UNDEF rw cur=0x0 (no form)
D20D MovieParam5 dt=UNDEF rw cur=0x0 enum[0]={}
D215 VarMovieRec dt=UNDEF rw cur=0x0 enum[0]={}
```

`dt=UNDEF` + `cur=0x0` means the driver has a descriptor but no datatype for it, so it never
decodes the value — and UNDEF props don't retain their raw bytes in the cache, so their value
can't be read back at all. Net: the movie recording size/fps is **not** under any of these
codes on the R50 family. Either it lives under a different (unguessed) code, only populates in
a state we weren't in, or Canon simply doesn't expose it over PTP on these consumer bodies
(plausible — even the video-focused R50 V shows nothing).

## Decisive next step: full property dump + diff

`gp_iccamera_eos_props_dump()` lists **every** property in the driver's EOS cache (compact,
code-sorted), so you're no longer guessing codes:

```
D102 dt=UINT16  rw cur=0x00000002 enum[4]
D108 dt=UINT32  rw cur=0x00000000
D1C2 dt=UINT32  rw cur=0x00000000 enum[2]
...
```

Procedure — run each dump on the **background thread, in movie mode** (confirm
`gp_iccamera_get_movie_mode()` == VIDEO; on the R50 flip the photo/video switch first):

1. Dump → **baseline**.
2. Change **only the movie resolution** on the camera. Dump again. Diff: which line's `cur`
   changed (or which new code appeared)?
3. Back, then change **only the frame rate**. Dump, diff again.

Send me both dumps (baseline + after each change). Whatever code's `cur` tracks the setting is
the target — I'll wire `get_movie_size`/`set_movie_size` + a resolver to it. If **nothing** in
the full dump moves when you change movie size on-camera, that's the definitive answer that
these bodies don't expose it over PTP (an EDSDK-only / on-camera-only setting), and we stop
chasing it.

```swift
func dumpEosProps(_ cam: OpaquePointer) -> String {
    var buf = [CChar](repeating: 0, count: 16384)
    let n = gp_iccamera_eos_props_dump(cam, &buf, 16384)   // returns prop count, or <0
    return n < 0 ? "(dump failed)" : String(cString: buf)
}
```

## Best tool: watch the event stream (`gp_iccamera_watch_prop_changes`)

The dump reads *cached, decoded* values — so an `UNDEF` property's value stays `0x0` even when
it changes, and a resolution change looks invisible. But the camera still emits a
property-changed **event** carrying the property CODE, decoded or not. `watch_prop_changes`
listens for a window and reports which codes fired — this is what caught (or ruled out) the
movie-size property after the full dump came back static.

```c
/* Watch the EOS event queue for `ms`; returns the count of distinct prop codes seen. */
int gp_iccamera_watch_prop_changes(gp_iccamera *, int ms, char *out, int outlen);
```

Output:

```
watched 15000 ms: 2 distinct prop code(s)
PROP d219  x1
PROP d20e  x1
RAW d2xx 00 00 00 …        (any unhandled event with a raw payload, verbatim)
```

Procedure (background thread, in stable movie record-standby):

1. Call `watch_prop_changes(cam, 15000, …)`.
2. **While it's running**, change the movie recording size via the **Movie rec quality menu**
   (not the mode dial — that fires unrelated events and can land in the dial-guide overlay).
3. Read the summary:
   - **a `PROP <hex>` fires** → that code is the movie-size property (even if `UNDEF`). Send it
     to me and I'll add a decode/raw reader for exactly that code, then build the resolver.
   - **only `RAW …` fires** → paste it; the hex payload is decodable.
   - **nothing fires** → definitive: the R50 doesn't signal movie rec size over PTP.

```swift
func watchPropChanges(_ cam: OpaquePointer, ms: Int32 = 15000) -> String {
    var buf = [CChar](repeating: 0, count: 4096)
    let n = gp_iccamera_watch_prop_changes(cam, ms, &buf, 4096)   // count, or <0
    return n < 0 ? "(watch failed)" : String(cString: buf)
}
// print("▶︎ change movie rec size now…"); print(watchPropChanges(cam))
```

## Located: 0xD20D (MovieParam5) is the movie rec-size property

The prop-change watch on the R50 (changing movie recording size in record-standby) fired
**`PROP d20d ×4`** — far more than anything else; the rest (`d120/d121/d122` image format,
`d138` mode dial, `d1c5` AEModeMovie, …) is the normal mode-change cascade. So
**`MovieParam5` (0xD20D)** carries the movie recording size. It was invisible in the dump only
because the driver left it `UNDEF`.

Fix applied: `0xD20D` (and its movie siblings `0xD209`/`0xD20E`/`0xD215`/`0xD218`/`0xD219`)
are now decoded as `UINT32` in `camlibs/ptp2/ptp-pack.c` (same list as `MovSize`), so their
values read out. Rebuild the xcframework and they'll show a real `cur=` in the dump and via
`gp_iccamera_get_eosprop(0xD20D, …)`.

### Collect the code → resolution/fps mapping

Read `0xD20D` (and note the others in case fps lives in a sibling) at each setting:

```swift
func readMovieParam5(_ cam: OpaquePointer) -> UInt32 {
    var v: UInt32 = 0, dt: UInt32 = 0, nch: Int32 = 0
    var choices = [UInt32](repeating: 0, count: 64)
    _ = gp_iccamera_get_eosprop(cam, 0xD20D, &v, &dt, &choices, 64, &nch)
    return v   // also inspect `choices[0..<nch]` — that's the list of valid codes, if advertised
}
```

In movie record-standby, for each **movie recording size** (and each **frame rate**) the R50
offers, record `setting → 0xD20D value`. Send me that table. Two cases:

- **the `0xD20D` value changes distinctly per size/fps** → I build `gp_canon_moviesize`
  (code → `{width, height, fps}`) + `get_movie_size`/`set_movie_size` + the `poll_events`
  update wiring, exactly like the stills resolver.
- **it only partly changes** (e.g. tracks size but not fps) → check whether a sibling
  (`0xD215` VariableMovieRec, etc.) carries the rest, and we map both. `MovieParam5` may pack
  fields in one `u32` (e.g. high/low bytes) — the observed values will show the bit layout.

## Swift usage

```swift
/// Dump the movie-size candidate properties. Call on the camera's background queue.
func probeMovieSize(_ cam: OpaquePointer) -> String {
    var buf = [CChar](repeating: 0, count: 4096)
    guard gp_iccamera_movie_size_probe(cam, &buf, 4096) == 0 else { return "(probe failed)" }
    return String(cString: buf)
}

// Wire to a debug button; print before/after each on-camera change and diff:
let before = probeMovieSize(cam)
// … change resolution/fps on the R50 …
let after  = probeMovieSize(cam)
print(before, "\n---\n", after)
```

Once you paste a couple of these dumps (baseline + a few resolution/fps changes), I can turn
the raw codes into a proper resolution + framerate resolver with set and update-on-change.
