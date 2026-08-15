# Canon movie remaining recording time

**The camera never transmits its remaining-time figure.** The number on the R50 / R50 V screen is
computed on-body and does not appear in any device property, event, or OLC record. The app has to
compute it — this note gives the formula, the properties that feed it, and the measured constants.

Verified on an **EOS R50** and an **EOS R50 V** over USB, against each camera's own display.

## The formula

```
seconds_remaining  =  free_bytes × 8 ÷ nominal_bitrate × k        k = 0.9499
```

`k` is the camera being conservative — container muxing, audio, filesystem overhead, safety
margin. It reproduces the camera's own readout to **within ~2 seconds on a 42-minute estimate**.

### Measured samples

| Body | Mode | Container | Nominal | Camera shows | `k` |
|---|---|---|---|---|---|
| R50 | 4K 29.97p IPB Standard, HDR PQ **on** | MP4 / H.264 | 170 | 47:04 | 0.9493 |
| R50 | 4K 29.97p IPB Light, HDR PQ on | MP4 / H.264 | 85 | 94:20 | 0.9514 |
| R50 | 4K 29.97p IPB Standard, HDR PQ **off** | MP4 / H.264 | 120 | 66:43 | 0.9499 |
| R50 V | 4K 25p XF-HEVC 422 10bit | H.265 | 135 | 42:12 | 0.9491 |
| R50 V | 4K 25p XF-HEVC 420 10bit | H.265 | 100 | 57:01 | 0.9499 |
| R50 V | 4K 25p XF-AVC 420 8bit | H.264 | 100 | 57:01 | 0.9499 |
| R50 V | 4K 25p XF-AVC 422 10bit | H.264 | 150 → **154.2** | 36:58 | 0.9499\* |

Six measured samples agree to **0.24%** across two bodies, H.264 and H.265, three container
formats, HDR PQ on and off, and a 2× bitrate range. The HDR-on/off pair is the strongest single
check: same body, same resolution, same frame rate, same compression — only the bitrate column
changes, and both land on `k`.

\* **The XF-AVC 422 10bit row is the one soft spot.** At the displayed 150 Mbps it yields
`k = 0.9238`, 2.8% off the others. Storing **154.2 Mbps** instead makes it fit the common `k`, on
the theory that "150" is a rounded display figure — it is the only round-to-ten value in the set,
and every other format matches its published number exactly. The alternative reading (150 is
correct and XF-AVC 422 carries ~3% more overhead) fits the same single data point equally well.
Both give identical predictions, so nothing breaks either way; revisit if that mode is ever
measured at another resolution and drifts.

## Properties that feed it

### Free space
`gp_camera_get_storageinfo` → `free` (KB). Not a device property.

### `0xD257` — recording format (R50 V)

`PTP_DPC_CANON_EOS_MovieRecordingFormat`. 4-value enum. **The codes are NOT in menu order** —
reading them as a menu index silently swaps 3 and 4.

| Code | Format | Bitrate (4K 25p) |
|---|---|---|
| 1 | XF-HEVC S YCC422 10bit | 135 Mbps |
| 2 | XF-HEVC S YCC420 10bit | 100 Mbps |
| 3 | XF-AVC S YCC420 8bit | 100 Mbps |
| 4 | XF-AVC S YCC422 10bit | 150 (store 154.2) |

All four confirmed on hardware by matching the camera's displayed bitrate. Note codes 2 and 3 are
both 100 Mbps and give identical times, so remaining time alone cannot distinguish them.

### `0xD20D` / `0xD29E` — movie size

Resolution + frame rate. See [canon-movie-recording-size.md](canon-movie-recording-size.md) for the
struct layout. R50 = `MovieParam5` (40 bytes, fps at word 1); R50 V = `MovieParam6` (32 bytes,
actual fps at word 7). Word 2 is the resolution code (`0` = 1920×1080, `5` = 3840×2160) on both.

On the **R50**, word 5 selects the compression variant (IPB Standard vs IPB Light) — inferred, not
measured: it is the only field that differs between the two option-list entries for a given
resolution+fps, and compression is the only remaining axis.

### `0xD20C` — HDR PQ (R50)

`EOS_2GHDRSetting`, `1` = on, `0` = off. **This roughly doubles the movie bitrate** and must be
part of the lookup key: 4K 29.97p IPB Standard is 120 Mbps HDR-off but **170 Mbps HDR-on**.
Ignoring it overstates remaining time by ~40%.

**Confirmed by direct toggle** on an EOS R50: switching HDR PQ off in the camera menu flipped
`d20c` 1 → 0, and the displayed time moved 47:04 → 66:43, exactly the predicted 120 Mbps figure.

The same toggle also moved, all with resolution and frame rate held fixed:

| Property | HDR PQ on | off | Meaning |
|---|---|---|---|
| `d11b` `EOS_AvailableShots` | 4656 | 5240 | **stills shrink** — HDR PQ shoots 10-bit HEIF, off shoots 8-bit JPEG |
| `d13b` `EOS_HighlightTonePriority` | 1 | 2 | coupled — HDR PQ forces HTP |
| `d20d` `MovieParam5` w7, w8 | 2, 10 | 1, 8 | gamma flag + **bit depth** |
| `d210` (unnamed) w2 | 8 | 1 | tracks HDR PQ, meaning unknown |

The `AvailableShots` change is the important one: a movie-only setting cannot change the remaining
**stills** count, so **HDR PQ on Canon spans both photo and video**, not video alone.

**Availability:**

| Body | Mode | Allowed values | Note |
|---|---|---|---|
| R50 | movie | `{0,1}` | selectable |
| R50 | photo | `{0,1}` | selectable |
| R50 V | movie | `{0}` | not offered — video is natively 10-bit, so redundant |
| R50 V | photo | `{0,1}` | selectable, observed on |

The only genuine restriction is R50 V movie mode: its XF-HEVC / XF-AVC formats already carry
10-bit, so a separate HDR PQ toggle would be meaningless there.

> An earlier revision claimed availability was "mode-dependent and inverted between the bodies",
> with the R50 not offering HDR PQ in photo mode. **That was an artifact of a camera firmware
> bug**, not a protocol fact — see below. After a camera reset the R50 offers `{0,1}` in photo
> mode, exactly like the R50 V.

### `0xD208` `EOS_FocusShiftSetting` — focus bracketing, and a spurious restriction

The EOS R50 has a known firmware bug where HDR PQ is greyed out with *"Restricted by the following
settings or conditions: Focus Bracketing"* **even when Focus Bracketing is disabled**. A camera
reset clears it.
([Canon Community](https://community.usa.canon.com/t5/EOS-DSLR-Mirrorless-Cameras/HDR-PQ-greyed-out-on-the-EOS-R50/td-p/596846))

While the restriction is active the camera advertises `0xD20C` as `{0}` — it reports HDR PQ as
*unavailable* rather than refusing a write.

Decoded `0xD208` across three measured states:

| State | HDR PQ | w2 | w3 |
|---|---|---|---|
| Pre-reset, bug active, FB shown *off* | `{0}` blocked | 0 | **3** |
| Post-reset, FB off | `{0,1}` | 0 | 10 |
| Post-reset, **FB on** | `{0,1}` | **1** | 10 |

- **word 2 = Focus Bracketing enabled** (`0` off, `1` on). Confirmed by toggling it directly.
- **word 3 = 3 marks the stuck restriction**, `10` otherwise.

**Enabling Focus Bracketing does not block HDR PQ.** With FB genuinely on (`w2 = 1`), `0xD20C`
stays `{0,1}` and reads `1`, confirmed both over PTP and on the camera's own display. So Canon's
"Restricted by Focus Bracketing" message is **spurious** — the two features do not actually
conflict on this firmware, and `w3 = 3` is a symptom of the stuck state rather than of focus
bracketing.

An app can still read `w3` as a **restriction indicator** (`3` → HDR PQ will be refused), but must
not present it as "focus bracketing is blocking you" — that attribution is the camera's bug, not a
real dependency. Note `w3 = 10` alone does not guarantee availability either: R50 V movie mode
reads 10 and is still `{0}`, for the unrelated reason above.

What actually sets `w3 = 3` is still unknown; it was only ever observed in the pre-reset stuck
state, which cannot now be reproduced on demand.

`0xD13D` `EOS_HDRSetting` — the older stills backlit-HDR function — is **never emitted** by either
body, in either mode. Zero occurrences across six full property captures, including two with ~2×
the event coverage. These bodies do not implement it; it belongs to older EOS generations.

## Bitrate tables

Nominal figures are Canon's published specs. The app must ship these as data — there is no way to
read a bitrate from the camera.

### EOS R50 (MP4 / H.264, HDR PQ on)

| Resolution | fps | Compression | Mbps |
|---|---|---|---|
| 4K UHD | 29.97 / 23.98 | IPB Standard | 170 |
| 4K UHD | 29.97 / 23.98 | IPB Light | 85 |
| Full HD | 59.94 | IPB Standard | 90 |
| Full HD | 59.94 | IPB Light | 50 |
| Full HD | 29.97 / 23.98 | IPB Standard | 45 |
| Full HD | 29.97 / 23.98 | IPB Light | 28 |

HDR PQ **off** roughly halves these (4K IPB Standard 120, IPB Light 60, FHD 59.94p 60/35,
FHD 29.97p 30/12).

### EOS R50 V (4K UHD 25p — measured)

| `0xD257` | Format | Store |
|---|---|---|
| 1 | XF-HEVC S YCC422 10bit | 135.0 Mbps |
| 2 | XF-HEVC S YCC420 10bit | 100.0 Mbps |
| 3 | XF-AVC S YCC420 8bit | 100.0 Mbps |
| 4 | XF-AVC S YCC422 10bit | 154.2 Mbps |

Other R50 V modes, from Canon's specs, **unmeasured**: 4K crop 59.94/50p 225 (HEVC 422) / 250
(AVC 422); Full HD 119.88/100p 100; Full HD 59.94–23.98p 50.

## Gotcha: stale property replay

**When reading EOS properties from the event stream, take the LAST event for a property code, not
the first.** The camera replays a cached value before the current one — after a format change the
`0xD257` stream reads `01, 02, 02`. Parsing the first event returns the value from *before* the
change, which looks exactly like "the setting didn't apply."

This cost a wrong conclusion during this work: the mapping was briefly declared unconfirmed
because a first-event parser kept reporting the old code. It was only caught by diffing the whole
property set against the previous state.

`ptp_canon_eos_getdevicepropdesc()` and `gp_iccamera_get_eosprop()` read the **cache**, which the
unpacker updates per event in order — so the cache holds the final value and is not affected.
This gotcha applies only to code parsing the event/log stream directly.

## Swift

```swift
enum RecordingFormat: UInt32 {           // 0xD257 — codes are not in menu order
    case xfHevcYcc422_10 = 1
    case xfHevcYcc420_10 = 2
    case xfAvcYcc420_8   = 3
    case xfAvcYcc422_10  = 4
}

/// EOS R50 V, 4K UHD 25p. Store 154.2 for AVC 422 — the camera displays a rounded 150.
let bitrate: [RecordingFormat: Double] = [
    .xfHevcYcc422_10: 135.0e6,
    .xfHevcYcc420_10: 100.0e6,
    .xfAvcYcc420_8:   100.0e6,
    .xfAvcYcc422_10:  154.2e6,
]

/// Matches the camera's own readout to ~2 s.
func secondsRemaining(freeBytes: Int64, format: RecordingFormat) -> Double? {
    guard let rate = bitrate[format] else { return nil }
    return Double(freeBytes) * 8 / rate * 0.9499
}
```

Read `0xD257` with `gp_iccamera_get_eosprop(cam, 0xD257, &value, &datatype, ...)`, movie size with
`gp_iccamera_get_movie_size()`, free space from the storage info.

## What is not verified

- **Every R50 V sample is 4K 25p.** Full HD, 4K crop and high-frame-rate modes are unmeasured on
  that body. `k` held across resolution on the R50, which is reassuring but is two points in one
  codec.
- **154.2 Mbps is derived, not observed** — see the note above.
- **`w5` as the R50 compression flag** is inference from the option list, never read back in a
  known-Light state.
- **`w8` as bit depth** in `MovieParam5` rests on a single 10→8 transition that matches HDR PQ's
  known bit depths. `w7` (2→1) is assumed to be a gamma/colour flag on the same basis. `d210` moves
  with HDR PQ but is entirely unidentified.
- **`0xD13D` `EOS_HDRSetting` was never observed** in either mode on either body — this is now a
  settled negative, not an untested gap.
- **The R50 photo-mode `{0}` reading** was taken with HDR PQ already off; mode-restriction and
  state-restriction are not separated. See the caveat above.
- **`0xD257` is R50 V-specific** as far as this work goes. Not checked on any other body, and the
  code has a Sony collision (`PTP_DPC_SONY_MediaSLOT2RemainingShots`), so it must only be read
  when the vendor is Canon.
- Nothing here is checked against bodies with **ALL-I or RAW** modes, where bitrates run 20×
  higher (R5 8K RAW ≈ 2600 Mbps). `k` may not survive those.
