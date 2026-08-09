# iOS Phase 0 Runbook — proving `requestSendPTPCommand` on a Canon EOS

This is the concrete, runnable version of Phase 0 from
[ios-framework-plan.md](ios-framework-plan.md). It answers the one make-or-break
question before any framework work: **does iOS let your app drive Canon EOS *vendor*
PTP opcodes through `ICCameraDevice.requestSendPTPCommand`, or does it refuse with
`ICReturnPTPNotAuthorizedToSendCommand` (-21249)?**

The deliverable is `PTPPhase0Tester.swift` (added to the `Canon Test` project). It runs a
scripted sequence against a connected Canon and prints a one-line **VERDICT**.

---

## 1. What it does

On session-ready it runs, in order, stopping early with a verdict if iOS blocks a command:

| Step | Opcode | Purpose |
|------|--------|---------|
| A. GetDeviceInfo | `0x1001` | Baseline read. Confirms the transport works and the camera is Canon EOS (vendor ext id `0x0000000B`). |
| B. EOS SetRemoteMode | `0x9114` (param 1) | **The Risk-A test.** First vendor opcode — the one most likely to be refused. |
| C. EOS SetEventMode | `0x9115` (param 1) | Enable the EOS push-event channel. |
| — EOS KeepDeviceOn | `0x911D` | Stop the camera from auto-idling during the test. |
| D. EOS GetEvent | `0x9116` | Drain the EOS event queue (events arrive by *polling this command*, not the USB interrupt endpoint). |
| E. Capture | `0x9128`/`0x9129` | Press sequence: `RemoteReleaseOn(1,0)` → `RemoteReleaseOn(2,0)` → `RemoteReleaseOff(2)` → `RemoteReleaseOff(1)`, then poll `GetEvent` for `ObjectAddedEx (0xC181)`. |
| F. Live view *(stretch)* | `0x9110` + `0x9153` | Set `EVFOutputDevice=PC` (prop `0xD1B0`=2), then grab one `GetViewFinderData` frame (param `0x00200000`). |

Every command logs whether it returned **OK (`0x2001`)**, a camera-level error
(e.g. **OperationNotSupported `0x2005`**), or was **blocked by iOS (`-21249`)** — the
three outcomes that matter.

## 2. Wire it up (2 minutes)

The project uses file-system-synchronized groups, so `PTPPhase0Tester.swift` is already
in the target. In `ContentView.swift`:

```swift
@State private var phase0 = PTPPhase0Tester()
```

Then either **(a)** point the existing device-tap at it — in `deviceList`, change
`ptp.connect(to: device)` to `phase0.connect(to: device)` — or **(b)** add a button and a
log view bound to `phase0.lines`:

```swift
Button("Run Phase 0 spike") { phase0.connect(to: device) }
    .disabled(phase0.isRunning)
ScrollView {
    VStack(alignment: .leading, spacing: 2) {
        ForEach(Array(phase0.lines.enumerated()), id: \.offset) { _, line in
            Text(line).font(.system(.caption2, design: .monospaced))
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}.defaultScrollAnchor(.bottom)
```

`NSCameraUsageDescription` is already set in the project, which iOS requires before it
will deliver PTP requests. Run **on a real iPhone/iPad** (not the simulator) with the
Canon connected by USB and powered on, its card inserted, and **not** already claimed by
the Photos import sheet.

## 3. Reading the verdict

The last lines of the log are the answer:

- **✅ PASS — iOS forwarded Canon EOS vendor opcodes.**
  `requestSendPTPCommand` works for control. Build the framework on **USB Path A** —
  static-link libgphoto2 core + ptp2 and route its `PTPParams` transport through
  `requestSendPTPCommand` (plan §2–§4).

- **❌ RISK A CONFIRMED — iOS returned -21249** (on a vendor opcode, or even on
  GetDeviceInfo). Wired full control isn't available to your app. **Pivot to WiFi/PTP-IP**
  (plan §7) — the *same* libgphoto2 ptp2 engine runs over the `ptpip` transport, which
  Apple doesn't restrict, and it's how commercial iOS camera apps already do tethering.

- **🟡 PARTIAL — authorized, but SetRemoteMode unsupported.** Commands are getting
  through, but this body didn't accept the EOS handshake — verify vendor ext id is
  `0x0000000B` and the camera isn't in a Mass-Storage/PTP-import mode.

The distinction the tester draws for you:

| Outcome in log | Meaning |
|---|---|
| `error … code=-21249 «PTP NOT AUTHORIZED»` | **iOS blocked it** before it reached the camera → Risk A. |
| `response 0x2005 (OperationNotSupported)` | Command **reached the camera**; the camera declined it. iOS is *not* blocking. |
| `response 0x2001 (OK)` | Success. |

## 4. Why your earlier table couldn't fire the shutter

The opcode map in the first tester was shifted against the real Canon EOS opcodes
(authoritative source: libgphoto2 `camlibs/ptp2/ptp.h`). The corrected values the spike
uses:

| Opcode | Earlier table | Correct (EOS) |
|--------|---------------|---------------|
| `0x9114` | — | **SetRemoteMode** |
| `0x9115` | — | **SetEventMode** |
| `0x9116` | SetRemoteMode ❌ | **GetEvent** |
| `0x9128` | BulbEnd ❌ | **RemoteReleaseOn** |
| `0x9129` | RequestDevicePropValue ❌ | **RemoteReleaseOff** |
| `0x9153` | RequestSensorCleaning ❌ | **GetViewFinderData** |
| `0x9159` | RemoteCapture/TakePicture ❌ | **ZoomPosition** |

The last row was the shutter bug: the old capture fallback sent `0x9159` believing it
meant "take picture," but `0x9159` is **ZoomPosition**. EOS bodies don't capture via
`InitiateCapture (0x100E)` either — they need the `RemoteReleaseOn/Off` press sequence
the spike now performs.

Also note: **EOS capture events do not arrive on the USB interrupt endpoint** (so
`didReceivePTPEvent` stays quiet for captures). They're returned by *polling*
`EOS GetEvent (0x9116)` and parsing its packed record blob — which the spike does
(`ObjectAddedEx 0xC181`, handle at payload offset +8). ImageCaptureCore may *also*
surface the new file via its own `didAdd:` delegate, so the log watches both.

## 5. After Phase 0

- **PASS →** proceed to plan Phase 1 (build the libgphoto2 xcframework) and Phase 2 (the
  transport shim). The shim reuses ptp2's own container packing — the byte layout the
  spike builds by hand is exactly what `ptp2/usb.c` produces.
- **BLOCKED →** proceed to the WiFi/PTP-IP backend (plan §7). Keep the spike; it becomes
  your regression harness for whichever transport wins.

See [ios-framework-plan.md](ios-framework-plan.md) for the full phased plan, and
[canon-cameras.md](canon-cameras.md) for the Canon protocol details the shim relies on.
