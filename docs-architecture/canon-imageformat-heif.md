# Canon `imageformat` — HEIF / HDR PQ and the JPEG size fixes

The Canon EOS **image format** setting (`imageformat` → `PTP_DPC_CANON_EOS_ImageFormat` /
**`0xD120`**) could not be written at all while **HDR PQ was on**, and had four further
defects that silently selected the *wrong* JPEG size class on some bodies. All five are
fixed in the ptp2 driver; this note records what was wrong, what changed, and what still
needs on-body confirmation.

- Source: [`camlibs/ptp2/ptp-pack.c`](../camlibs/ptp2/ptp-pack.c) ·
  [`camlibs/ptp2/config.c`](../camlibs/ptp2/config.c) ·
  [`camlibs/ptp2/ptp.h`](../camlibs/ptp2/ptp.h)
- Related: [`canon-imagesize-resolver.md`](canon-imagesize-resolver.md) maps the resulting
  labels (`L`, `cRAW + S1`, …) to pixel dimensions.

---

## 1. The symptom

> With HDR PQ **On**, setting the JPEG/HEIF size and the quality both fail.
> With HDR PQ **Off**, everything works.

Reading the setting looked healthy in both cases — the choice list showed plausible
`L / M / S1 / S2` entries. Only the write failed.

## 2. Why — the type is thrown away

`0xD120` is not a scalar. On the wire it is a sequence of `u32` records, one per generated
file:

```
n, [ 0x10, type, size, compression ] × n
        │      │      └─ 0=L 1=M 2=S 5=M1 6=M2 0xe=S1 0xf=S2 0x10=S3
        │      └──────── 1 = JPG, 6 = RAW, other = HEIF
        └─────────────── entry length, always 0x10
```

`ptp_unpack_EOS_ImageFormat()` condenses this into a **`uint16`** so that the ordinary
enumeration/look-up-table machinery can be used — four nibbles:

```
entry1 size | entry1 type+compression | entry2 size | entry2 type+compression
```

Every bit is spoken for. There is no room for the **type**, so it was kept only as a single
RAW flag (`c |= 8` when `type == 6`) — and on the way back out it was reinvented from that
one bit:

```c
htod32a(data+=4, value & 0x0800 ? 6 : 1);   /* 1 == JPG, unconditionally */
```

With HDR PQ on, the camera's records carry the **HEIF** type. Unpack classified them as
JPEG (anything that isn't 6), and pack then asked the camera for a **JPEG** file while it
was in HEIF mode. The camera rejects the property value, so
`ptp_canon_eos_setdevicepropvalue` fails and the config layer reports

```
The property 'Image Format' / 0xd120 was not set (0x….)
```

**"Quality" fails for the same reason.** An EOS has no separate quality property —
`imagequality` is `PTP_DPC_CANON_ImageQuality`, a PowerShot-only property. The fine/coarse
`c` prefix (`cL`, `cS1`, …) is just the compression nibble of the same `0xD120` value, so it
travels through the identical pack path.

### The fix

The type is now remembered rather than invented. `ptp_unpack_EOS_ImageFormat()` records the
non-RAW type in `params->canon_eos_nonraw_filetype`, and `ptp_pack_EOS_ImageFormat()` writes
that back:

```c
uint32_t nonraw = params->canon_eos_nonraw_filetype ? params->canon_eos_nonraw_filetype : 1;
…
htod32a(data+=4, value & 0x0800 ? 6 : nonraw);
```

The driver therefore **never needs to know HEIF's numeric code** — it echoes whatever the
body reported. `0` (nothing seen yet) falls back to `1`, so nothing changes on a path that
has not read the property first. HDR PQ is a global mode, so the type is uniform across
entries and last-seen-wins is correct.

> **HDR PQ itself is still not exposed.** `PTP_DPC_CANON_EOS_HDRSetting` (**`0xD13D`**) and
> `0xD20C` exist as codes in `ptp.h` but have no entry in the config menu — only a name
> string in `ptp.c`. To toggle HEIF mode from the app, go through the raw property path
> (`gp_iccamera_set_eosprop`), and **confirm the code on the body first** by diffing an
> `eos_props_dump` with HDR PQ on vs off. It has never been exercised.

---

## 3. Four further defects in the same path

| # | Defect | Effect |
|---|---|---|
| 1 | An empty second entry was detected as `size == 0 && compression == 0` | A **real** `L` JPEG at compression 0 is indistinguishable from "no second file" → `cRAW + L` collapsed to `cRAW`, and packing emitted only the RAW |
| 2 | The second component was searched only among the **high** bytes of the enum | The JPEG half of a dual lives in the **low** byte → not found, or found with the wrong compression |
| 3 | The assembled `uint16` was never validated against the enum | A combination the body does not offer went on the wire |
| 4 | `strncmp(label, name, n)` prefix-matched | `"S"` matched `"S1"`, `"M"` matched `"M1"` → **a different size than requested was set, with no error** |

### 3a. Empty second entry

Only `n` can tell you the second entry is absent:

```c
if (n == 1)                 /* was: if (s2 == 0 && c2 == 0) */
    s2 = c2 = 0xF;
```

Bodies that use "user/custom" compression (`compression == 0`, e.g. 1DX, R5m2) encode
`L` as `0x00`, which the old test swallowed.

### 3b–3d. One fix for the other three

`_put_Canon_EOS_ImageFormat()` used to resolve the two components independently and hope the
combination was legal. It now **searches the enumeration for the entry that renders to the
requested label**:

```c
_EOS_ImageFormat_label (val, buf, sizeof(buf));
if (strcmp (buf, label) == 0) { propval->u16 = val; return GP_OK; }
```

`_single_EOS_ImageFormat_value()` is gone. `_get_` and `_put_` now share one label renderer
(`_EOS_ImageFormat_label()`), so the choice list and the reverse look-up cannot disagree, the
match is exact, and the result is always a value the camera actually offered.

That renderer also fixes a latent aliasing bug: `_single_EOS_ImageFormat_name()` returns a
**shared static buffer** for unknown values, and `_get_` used to hold two pointers into it at
once — `RAW + <unknown>` would have printed the second name twice.

When the enum is missing entirely (see §5), `_get_` offers the current value as the only
choice and `_put_` now accepts it as a no-op instead of erroring.

### 3e. Transposed R5m2 rows

```c
/* user/custom compression, e.g. R5m2 */
{ N_("S1"),   0xd0, 0 },   /* were 0xe0 */
{ N_("S2"),   0xe0, 0 },   /* were 0xd0 */
```

> ⚠️ **`VERIFY BEFORE SHIPPING`.** This is the one change that is an *inference*, not a
> measurement. The size-nibble mapping documented in `ptp_unpack_EOS_ImageFormat()`
> (`0xe`→S1, `0xf`→S2, decremented to `0xd`/`0xe`) agrees with the 5Dm3 rows
> `S1 = 0xd3 / S2 = 0xe3`; the four R5m2 rows were added in one commit
> (`042c4f380`, upstream) and never revisited, and the transposition showed up as an
> out-of-order choice list (`'L' 'M' 'S2' 'S1'`). Confirm on an R5 II / R1 class body —
> it is a two-line revert.

---

## 4. Verification

Both changed layers were tested by extracting the functions **verbatim** into standalone
harnesses (stubbed `PTPParams` / widget API) and round-tripping them.

**Wire layer** — for each case, unpack the camera's bytes, then pack the result and require
the output to be **byte-identical** to what the camera sent:

| Case | Result |
|---|---|
| `cRAW + cS1`, `L`, `RAW`, `S3` (nibble squeeze) | identical |
| `cRAW + L` at user compression 0 | identical *(was: JPEG half dropped, 20 bytes instead of 36)* |
| `L` and `cRAW + S2` with a HEIF type | identical *(was: type written back as `01`)* |

**Config layer** — for each simulated body, every choice `_get_` offers must be accepted by
`_put_` and map back to the exact value it was rendered from:

```
                                   OLD                          NEW
R5m2 choice list   'L' 'M' 'S2' 'S1' … 'cRAW + S2'   'L' 'M' 'S1' 'S2' … 'cRAW + S1'
set "S"   (5Ds)    accepted → 0xd3ff  (that's S1!)   rejected
set "M"   (5Ds)    accepted → 0x53ff  (that's M1!)   rejected
set "cRAW + L"     → 0x0b03, not offered by body     → 0x0b00, the body's own value
```

`./ios/build.sh test` passes (`SMOKE TEST OK`) and both xcframework slices build clean.

**None of this is on-body verified yet** — it is wire-format and look-up logic only. See §6.

---

## 5. If a set still fails

The enum for `0xD120` is populated **only** by the `PTP_EC_CANON_EOS_AvailListChanged`
event, and only when `dpd_type == 3` and the property is already in the cache. If the app
calls `set_config("imageformat", …)` before pumping `ptp_check_eos_events()` in remote/event
mode, `FORM.Enum.NumberOfValues` is 0 and no value can be resolved.

The tell-tale is on the **read** side: the widget shows **exactly one choice — the current
value**. That is an app-side sequencing problem, not a driver bug.

Also grep the debug log for `parsing EOS ImageFormat property failed`. If HEIF records ever
use an entry length other than `0x10`, unpack bails at the `l != 0x10` check and the whole
enum comes back as zeros.

---

## 6. On-body checklist

- [ ] HDR PQ **On** → set `L` / `M` / `S1` / `S2` → each succeeds, files land as `.HIF`
- [ ] HDR PQ **On** → set the `c` variants (quality) → each succeeds
- [ ] HDR PQ **On** → set `RAW` only → succeeds (this worked before the fix too)
- [ ] HDR PQ **Off** → full regression of the same list, files land as `.JPG`
- [ ] Dual `cRAW + L` in both HDR PQ states → **two** files recorded, not just the RAW
- [ ] R5 II / R1 class body → the `S1` / `S2` labels match what the camera's own menu shows (§3e)

---

## 7. Residual

If a body ever enumerated two values rendering to the same label (say `0x03ff` and `0x00ff`
both as `"L"`), the first would win. That needs a body mixing two compression schemes for the
same size class, which is not believed to exist — and it is now a harmless tie between two
spellings of the same size rather than a wrong-value bug.
