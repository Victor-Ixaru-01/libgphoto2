# Canon battery level — why it read 100% forever

The `batterylevel` config node reported **`100%` permanently** on the EOS R50 and R50 V,
regardless of actual charge. Both bodies are now fixed in `camlibs/ptp2/config.c`; this note
covers what changed and what the Swift side has to do differently.

## The two properties

Canon EOS bodies carry **two** battery properties, and they do not agree:

| Property | Kind | R50 / R50 V | R6 / R6 Mark III |
|---|---|---|---|
| **`0x5001`** `BatteryLevel` (standard PTP) | `UINT8`, enum form `[100,0,75,0,50]` | **pinned at `100`** — never moves | real percentage (`28`, `24`) |
| **`0xD111`** `EOS_BatteryPower` (Canon vendor) | `UINT16`, 5-state indicator | `1` → half | `1` → half |

Neither is trustworthy alone. On the R50 family `0x5001` is a stub that always answers 100; on
the R6 family it carries a genuine fine-grained percentage that `0xD111` only buckets coarsely.

libgphoto2 was returning `0x5001` on every body, because its row sits above the Canon row in the
submenu table — so `have_prop()` matched it first and the vendor property was never consulted.
The value was visible only in the raw event stream (`gphoto2 --wait-event`), never through the API.

A second, related defect: `_get_config()`'s Canon-EOS branch had no duplicate-name guard (the
standard branch has one), so **two widgets both named `batterylevel`** were created under
`/main/status`. Which one a caller got depended on insertion order.

## What changed

`camlibs/ptp2/config.c`:

- **`_canon_eos_batterypower_percent()`** ([config.c:8127](../camlibs/ptp2/config.c#L8127)) — new
  shared mapping for `0xD111`. Returns the percentage, `0` for `"Low"`, `-1` for unknown.
  `_get_Canon_EOS_BatteryLevel()` now routes through it so the two readers can't drift.
  Output strings are byte-identical to before.
- **`_get_BatteryLevel()`** ([config.c:8139](../camlibs/ptp2/config.c#L8139)) — on Canon EOS bodies
  exposing `EOS_BatteryPower`, reads `0xD111` as well and **reports the lower of the two**.
  The R50's pinned `100` loses to Canon's `50`; an R6 Mark III's genuine `24` beats `0xD111`'s
  coarse `50`, so fine-grained bodies keep their precision. Reading low is a nuisance; reading
  100% on a half-empty battery is the bug.
- **Duplicate-name guard** in the Canon-EOS branch of `_get_config()`, for both `MODE_GET` and
  `MODE_LIST`. `batterylevel` now appears exactly once.
- `gp_widget_set_name` hoisted in `_get_BatteryLevel` — the third code path never set a name, so
  on any camera reaching it the widget was unnamed and unfindable by name.

## Swift-facing contract

There is **no dedicated bridge accessor** — battery goes through the generic config call:

```c
int gp_iccamera_get_config(gp_iccamera *, const char *name,
                           char *value, int vlen, char *choices, int clen);
```

Call it with `name = "batterylevel"`. `choices` comes back empty (the widget is read-only TEXT).

### The value is a string, and not always a number

| Returned | Meaning |
|---|---|
| `"100%"` `"75%"` `"50%"` `"25%"` | a bucket — see resolution note below |
| `"Low"` | `0xD111` says Low; **no percentage exists** — do not parse as a number |
| `"Unknown value"` | `0xD111` returned a code we don't have a mapping for |
| `"broken"` | degenerate range form on the standard property (not seen on Canon) |

```swift
enum BatteryLevel {
    case percent(Int)   // coarse on R50-family — see below
    case low
    case unknown

    init(gphotoValue raw: String) {
        let s = raw.trimmingCharacters(in: .whitespaces)
        if s.hasSuffix("%"), let n = Int(s.dropLast()) { self = .percent(n) }
        else if s == "Low" { self = .low }
        else { self = .unknown }
    }
}
```

### Resolution: this is a 5-state gauge, not a percentage

On the R50 and R50 V the only usable source is `0xD111`, which has **five states**:
`Low, 25%, 50%, 75%, 100%`. The number you get back is the label of a bucket, not a measurement.

- **Do not** render a continuous progress bar or a "62%" style readout — the value will jump in
  25-point steps and sit still for a long time between jumps.
- **Do** render discrete segments (a 4-bar icon plus a Low state), which is what the camera's own
  display shows.
- **Do not** compute time-remaining or drain-rate estimates from it.

Older/other bodies (R6, R6 Mark III) can return arbitrary integers like `24` — so still parse the
full 0–100 range, just don't *assume* granularity.

### Behaviour changes to account for

1. **`batterylevel` now returns a real value where it used to return `100%`.** Any UI that
   effectively never updated will start moving. If anything special-cased "always 100", drop it.
2. **`gp_iccamera_list_config` no longer emits `batterylevel` twice.** If the Swift side
   deduplicated the config-name list, or indexed settings positionally, that assumption changes —
   node count on the R50 drops by one.
3. **`"Low"` is a new reachable value.** Previously unreachable on the R50 family, since the
   pinned `0x5001` never produced it. A parser that force-unwraps an `Int` will crash on it.

## Verification status

Measured on hardware, same physical battery in both bodies (camera's own indicator: 2 bars):

| | R50 V | R50 |
|---|---|---|
| `0x5001` | `100` (pinned) | `100` (pinned) |
| `0xD111` | `1` → 50% | `1` → 50% |
| before | **100%**, 2 widgets | **100%**, 2 widgets |
| after | **50%**, 1 widget | **50%**, 1 widget |

Also: `make check` 13/13; config-node list diffed before/after against the camera, only delta is
the removed duplicate.

**Caveats worth carrying forward:**

- The lower-of-the-two rule's *other* branch — where `0x5001` carries the finer value and wins — is
  **not hardware-verified**. Both R50 bodies pin `0x5001` at 100, so neither exercises it. That
  branch is reasoned from committed capture dumps (R6 `28`, R6 Mark III `24`) whose real battery
  state nobody recorded. An older EOS (70D / 5D era) would test it directly.
- The `0xD111` mapping is still partly upstream guesswork: it covers `0, 1, 2, 4, 5` with `3`
  missing and in non-monotonic order. Only `1 → 50%` is confirmed on hardware. A body reporting
  `3` renders as `"Unknown value"`.
- Neither R50 exposes `BatteryInfo` (`0xD1A6`) or `BatteryInfoEx` (`0xD21C`), so 5 states is the
  best resolution obtainable from these cameras over PTP. There is no finer number to go get.
- The fix is compiled into both xcframework slices (verified by symbol), but the **bridge path was
  not re-run against hardware** — the iOS bridge is ICCameraDevice-based and can't be driven from
  the command line. The `config.c` code it contains is byte-identical to what was verified natively.
