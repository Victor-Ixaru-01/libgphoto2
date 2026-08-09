# Plan — an iOS Framework Bridging libgphoto2 to a Canon Camera over USB

**Goal:** an iOS/iPadOS framework (Swift API, C core) that gives an app **full control**
of a **Canon** camera over a **USB cable** — browse/download, tethered capture, live
view, and settings — distributable on the **App Store**, on **iPhone and iPad**.

**Your situation:** you can already get an iPhone to *connect* to the Canon and see
bytes, but you can't parse them. That's because what you're seeing is **PTP** (Picture
Transfer Protocol) wire data — and on Canon EOS, a proprietary event/property layer on
top. **Parsing that is precisely what libgphoto2's `ptp2` driver does.** This plan uses
libgphoto2 as the *protocol engine* and Apple's ImageCaptureCore as the *transport*.

---

## 0. TL;DR — the recommended architecture

> **Use Apple's `ICCameraDevice` for discovery + the raw-PTP transport, and libgphoto2's
> core + `ptp2` driver (compiled statically into the framework) for all protocol logic.
> Bridge them at the PTP-transaction level.**

The key enabling fact (verified against current Apple docs, see [References](#references)):

- **`ICCameraDevice.requestSendPTPCommand(_:outData:completion:)`** — a **public,
  App-Store-allowed** API (iOS/iPadOS **13.4+**) that sends an arbitrary **PTP command
  container** plus optional out-data and returns the **response + in-data**. Every PTP
  camera advertises `ICCameraDeviceCanAcceptPTPCommands`.
- This is the *exact granularity* libgphoto2's `ptp2` speaks internally — its
  `PTPParams` transport is four function pointers (`sendreq` / `senddata` / `getresp` /
  `getdata`). We point those at `requestSendPTPCommand` and the whole Canon EOS stack
  (capture, live view, the 102 Canon settings, the EOS event queue) runs unchanged on
  top.

This satisfies **all four** of your requirements at once — iPhone + iPad, App Store,
full control, and libgphoto2 doing the heavy lifting — which the raw-USB path cannot
(that's iPad-only; see §8).

**Two risks must be de-risked in week one** before building anything else: there is
**documented, multi-year evidence** that iOS *refuses* raw PTP commands with
`ICReturnPTPNotAuthorizedToSendCommand` even though the same code works on macOS (§3
Risk A) — so Phase 0's only job is to prove Canon opcodes actually go through on your
iOS 16 iPhone; and libgphoto2 is **LGPL-2.1**, which constrains how you link it into an
App Store binary (§3 Risk B). If USB is blocked, the same libgphoto2 engine runs over
**WiFi/PTP-IP** instead (§7) — the route commercial iOS camera apps already use.

---

## 1. Why libgphoto2 is the right engine (and which part)

libgphoto2 is three layers (see [00-overview.md](00-overview.md)). You want the top two,
not the bottom one:

| Layer | Use it? | Why |
|---|:--:|---|
| **Core** (`libgphoto2/`) — Camera object, config-widget tree, filesystem, events | ✅ | Gives you the 359-setting config tree, capture orchestration, `wait_for_event`, file browse/download **for free**. |
| **`ptp2` driver** (`camlibs/ptp2/`) — PTP + Canon EOS protocol, parsing, marshalling | ✅ **the whole point** | This is the "parse the bytes" you're missing: `ptp.c`/`ptp-pack.c` (containers, opcodes), the Canon EOS dialect (`ptp_canon_eos_*`), `config.c` (settings). See [canon-cameras.md](canon-cameras.md). |
| **Transport** (`libgphoto2_port/` → libusb) | ❌ **replace** | libusb needs IOKit USB APIs Apple forbids to iOS apps. We substitute an ImageCaptureCore-backed transport. |

**Why Canon specifically is a great fit for this approach:** Canon EOS does **events and
live view as PTP *commands***, not via the USB interrupt endpoint —
`ptp_canon_eos_getevent` is opcode `0x9116`, `GetViewFinderData` is `0x9153`. Since
`requestSendPTPCommand` is command-based, Canon EOS tethering maps onto it cleanly, with
no need for interrupt-endpoint access (which ImageCaptureCore doesn't expose). A
generic-PTP camera that relied on interrupt-endpoint events would be harder; **Canon is
the easy case.**

---

## 2. The iOS transport reality (and how it's resolved)

There is no single "USB access" story on iOS. Three paths exist; only one satisfies your
constraints:

| Path | Devices | App Store? | Gives you… | Verdict |
|---|---|:--:|---|---|
| **A. `ICCameraDevice` + `requestSendPTPCommand`** (ImageCaptureCore) | **iPhone + iPad** (iOS 13.4+) | ✅ Public API | Discovery + **raw PTP transactions** | ✅ **Recommended.** Only path meeting all four requirements. |
| **B. `USBDriverKit`** (DriverKit system extension) | **iPad only** (M-series, iPadOS 16+) | ⚠️ Gated (special entitlement) | Raw **bulk endpoints** (byte level) | Fallback / power path (§8). No iPhone. |
| **C. IOKit / libusb (jailbreak)** | any (jailbroken) | ❌ Never | Everything | Out of scope for App Store. |

> **Why not just use `ICCameraDevice`'s high-level methods** (`requestReadData`,
> `requestTakePicture`)? Because those are a *cooked import API* — file browse/download
> and a bare shutter trigger. They do **not** expose ISO/shutter/aperture control, live
> view, the EOS event stream, or bulb. To get **full control** you must drop to
> `requestSendPTPCommand` and bring your own protocol brain — which is libgphoto2.

### The integration seam

```
┌───────────────────────────────────────────────────────────────┐
│  Your app  (Swift)                                            │
└───────────────────────────────┬───────────────────────────────┘
                                 │  CanonKit Swift API (async)
┌────────────────────────────────▼──────────────────────────────┐
│  CanonKit framework                                            │
│                                                                │
│   Swift/ObjC wrapper  ──►  libgphoto2 core + ptp2  (static C)  │
│                                    │                           │
│                    PTPParams transport vtable                 │
│               sendreq / senddata / getresp / getdata          │
│                                    │                           │
│                    ▼  (this is the shim you write)            │
│   ICCameraDevice.requestSendPTPCommand(_:outData:completion:) │
└────────────────────────────────┬──────────────────────────────┘
                                  │  ImageCaptureCore (Apple)
                                  ▼
                         Canon camera over USB
```

The shim is **small** (a few hundred lines): translate each libgphoto2 PTP transaction
into one `requestSendPTPCommand` call and back. You are *not* re-implementing USB, PTP
framing, or Canon logic — libgphoto2 already has all of it.

---

## 3. The three risks to settle before committing

### Risk A — iOS may refuse raw PTP commands — ✅ **RESOLVED (PASS)**

> **Validated 2026 on a Canon EOS R50 V + iPhone (iOS 26), App Store build.** iOS
> forwarded **every** Canon EOS vendor opcode with **zero `-21249`**: `GetDeviceInfo`,
> `SetRemoteMode (0x9114)`, `SetEventMode (0x9115)`, `KeepDeviceOn`, `GetEvent (0x9116)`,
> `RemoteReleaseOn/Off (0x9128/0x9129)`, and `GetViewFinderData (0x9153)` all returned
> proper PTP response codes (mostly `0x2001 OK`; a `0x2019 DeviceBusy` on the full-press
> that clears with a retry). **USB Path A is confirmed viable — proceed to Phase 1/2.**
>
> Two integration facts learned from that run, both now baked into `PTPPhase0Tester.swift`:
> - **`requestSendPTPCommand`'s completion is `(inData, response, error)`** — the **data
>   phase is the FIRST argument, the PTP response block is the SECOND**. Reading them in
>   the wrong order was the original "can't parse the bytes" symptom. The Phase 2 shim's
>   `getdata`/`getresp` mapping must respect this order.
> - **ImageCaptureCore owns the session and drains EOS events itself**, so a fresh capture
>   usually surfaces via its `didAdd:` delegate rather than a manual `GetEvent` poll
>   (Risk C). The shim should treat ICC's item notifications as an event source, and let
>   ptp2 reuse ICC's session rather than calling `OpenSession`/`CloseSession`.

The original concern (kept for context) was **not hypothetical**. Multiple developers report `requestSendPTPCommand`
returning **`ICReturnPTPNotAuthorizedToSendCommand` (-21249)** on iOS — *"the same code
works on macOS but fails on iOS"* — and the issue sat **unresolved from 2020 to 2023**
([Apple forum 656878](https://developer.apple.com/forums/thread/656878), radar
[FB7593726](https://ww.openradar.appspot.com/FB7593726)). Likely cause: ImageCaptureCore
**owns the PTP session** on iOS and guards commands that would disrupt it — so it may
authorize benign reads but reject capture/control opcodes, or reject arbitrary commands
entirely. (Tellingly, the established iOS camera-control products drive cameras over
**WiFi/PTP-IP**, not USB — see the WiFi fallback in §7.)

Because this can sink the USB approach, **Phase 0 exists solely to answer it on current
iOS 16**, in increasing order of "how likely Apple is to block it":

- Does a benign standard read — `GetDeviceInfo` (`0x1001`) — return real data, or -21249?
- Does a **Canon vendor** command — `EOS SetRemoteMode` (`0x9114`), then
  `EOS GetEvent` (`0x9116`) — return real data, or -21249?
- Do live view (`0x9153`) and release (`0x9128`) work?
- Must you run inside ImageCaptureCore's session (skip your own `OpenSession` — Risk C)?
- Are data phases delivered intact in **both** directions?

**Decision gate:**
- All succeed → Path A is fully viable; build the framework as planned. ✅
- Reads work, control opcodes are blocked → Path A does **browse/download only** on
  iPhone; get full control via **WiFi/PTP-IP** (§7, recommended) or **iPad DriverKit**
  (§8).
- Everything returns -21249 → USB control isn't available to your app on iOS; pivot to
  **WiFi/PTP-IP** (§7), which uses the *same* libgphoto2 ptp2 brain over a transport
  Apple doesn't restrict.

### Risk B — Licensing: libgphoto2 is **LGPL-2.1** + App Store

LGPL-2.1 requires that end users be able to **relink** your app against a modified
libgphoto2. Static linking into a closed-source App Store binary is the classic LGPL
conflict (cf. VLC's App Store history). Mitigations, in order of preference:

1. **Ship libgphoto2 as a *dynamic* `.framework` inside the app** (iOS allows embedded
   dynamic frameworks). Dynamic linkage + providing your object files/build recipe is the
   accepted way to honor LGPL relinking on the App Store. Keep **your** code in a
   separate module that links *against* the dynamic libgphoto2 framework.
2. Publish your libgphoto2 build scripts + any patches (you'll patch the transport seam
   anyway — those changes are LGPL and should be published regardless).
3. Get a short legal review before the first App Store submission.

**Do not** statically bake libgphoto2 into a monolithic closed binary without addressing
this. Decide the linkage model in Phase 1, because it shapes the Xcode project layout.

### Risk C — Session ownership

ImageCaptureCore opens its **own** PTP session when it connects `ICCameraDevice`.
libgphoto2's `ptp2 camera_init` also wants to `OpenSession`. Two sessions = conflict.
Plan: configure ptp2 to **reuse ImageCaptureCore's session** — skip `ptp_opensession` /
`ptp_closesession` and reuse the transaction-id space ImageCaptureCore expects. (This is
analogous to the `PTP_DONT_CLOSE_SESSION` flag Canon EOS R already uses, see
[canon-cameras.md §3](canon-cameras.md#3--device-flags--the-canon-quirk-bits).) Confirm
in Phase 0 whether `requestSendPTPCommand` transactions share ICC's session or need their
own.

---

## 4. Phased plan

### Phase 0 — De-risk the transport *(1–2 weeks, do this before anything else)*

A throwaway Swift app, **no libgphoto2 yet**. Prove the transport before investing:

- [ ] `ICDeviceBrowser` finds the Canon; connect the `ICCameraDevice`; confirm
      `capabilities` contains `ICCameraDeviceCanAcceptPTPCommands`.
- [ ] Add `NSCameraUsageDescription` to Info.plist (required to make PTP requests).
- [ ] **Wait for `deviceDidBecomeReady(withCompleteContentCatalog:)`** before sending —
      PTP requests before the catalog is ready are rejected.
- [ ] Send **`GetDeviceInfo` (`0x1001`)** and check for real data vs. `-21249`.
- [ ] Send **Canon `EOS SetRemoteMode` (`0x9114`)** then **`EOS GetEvent` (`0x9116`)**.
- [ ] Grab one **live-view frame** via `GetViewFinderData` (`0x9153`); try a release
      via `RemoteReleaseOn` (`0x9128`).
- [ ] Note session behavior (does ICC own the session? do you need your own?).

**The `command` `Data` is the standard PTP container** (confirmed from a working iOS
implementation — same layout libgphoto2 builds):

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | Container length | total bytes (little-endian) |
| 4 | 2 | Container type | `0x0001` (command block) |
| 6 | 2 | Operation code | e.g. `0x1001` GetDeviceInfo |
| 8 | 4 | Transaction ID | increment per call |
| 12+ | 4×N | Parameters | up to 5 (omit if none) |

So `GetDeviceInfo` with no params is the 12 bytes
`0C 00 00 00  01 00  01 10  <tid:4>`; pass `outData = nil`. The camera's DeviceInfo
dataset arrives as `inData` in the callback, with the PTP response code in `response`.
All integers little-endian.

**Exit criterion / gate:** see Risk A's decision gate. In one sentence — if Canon vendor
opcodes return real data on your iOS 16 iPhone, build Path A; if they return `-21249`,
pivot to WiFi/PTP-IP (§7) with the same libgphoto2 engine.

### Phase 1 — Build libgphoto2 for iOS — ✅ **DONE**

> Built and validated. The harness lives in [`../ios/`](../ios/README.md):
> `ios/build.sh xcframework` produces `libgphoto2.xcframework` (ios-arm64 device +
> ios-arm64 simulator). A native smoke test registers **2471 models** via the static
> loader (all Canon EOS R-series, `lib=ptp2`). ltdl is replaced by a static registry
> (`ios/src/ltdl_static.c`); olympus-wrap/libxml2, libusb iolibs, libexif and gettext are
> excluded per `ios/include/config.h`. The framework is copied into `Canon Test/Vendor/`.

Produce an **`.xcframework`** (arm64 device + arm64 simulator) of a **minimal**
libgphoto2:

- [ ] Cross-compile `libgphoto2` core + `libgphoto2_port` (headers/result codes only) +
      `camlibs/ptp2`. **Omit** every other camlib and every iolib.
- [ ] **Static registration, not `dlopen`** — iOS forbids loading external code. Replace
      the ltdl plugin discovery with a tiny registry that calls `ptp2`'s
      `camera_id`/`camera_abilities`/`camera_init` directly (the `vusb` test harness shows
      the pattern — see [02-port-api.md §5](02-port-api.md#vususbvusbc--vcamerac-2546-lines--the-virtual-camera--for-you)).
- [ ] Configure the build to drop desktop deps: `--without-libxml2` (Olympus-only, not
      Canon), stub/omit `gettext` i18n, use the system `iconv`, make `libexif` optional.
- [ ] Decide **dynamic** vs static framing per Risk B (recommended: dynamic framework).
- [ ] Sanity-check by running ptp2 against the **`vcamera`** virtual camera in a host
      unit test — full stack, zero hardware.

### Phase 2 — The transport shim — ✅ **DONE & VALIDATED ON HARDWARE**

> **Ran on a Canon EOS R50 + iPhone:** libgphoto2 parsed real DeviceInfo
> (`Canon.Inc / Canon EOS R50`, 153 ops) and the EOS handshake
> (`SetRemoteMode`/`SetEventMode`) returned `0x2001 OK` — **libgphoto2's Canon EOS engine
> is driving the camera over USB on iPhone.** Finding: the R50 reports the **MTP** vendor
> extension (`0x06`), not Canon (`0x0B`), so the shim now applies ptp2's own fixup
> (manufacturer contains "Canon" → force `PTP_VENDOR_CANON`) after `ptp_getdeviceinfo`, so
> the Canon capture/config/event paths activate for Phase 3.

> Implemented as [`ios/src/gp_iccamera.c`](../ios/src/gp_iccamera.c) (compiled into the
> framework) + [`LibGPhoto2Tester.swift`](../../xCode%20Workspace/Canon%20Test) in the app.
> **Design chosen:** rather than patch `camera_init` or build a byte-level iolib, the shim
> drives ptp2 as a PTP library with a self-managed `PTPParams` whose transport vtable
> (`sendreq`/`senddata`/`getdata`/`getresp`) routes each transaction through **one C
> callback** the app implements over `requestSendPTPCommand`. It buffers the command in
> `sendreq`, fires the callback at the data phase, hands the response back in `getresp`,
> and **forges the transaction id** to what ptp2 expects (ImageCaptureCore owns the real
> session/ids). `params->data` points at a struct whose first member is a `PTPData`, so
> ptp2's `(PTPData*)params->data` view and the shim's full state coexist. iconv is wired
> for MTP/UCS-2 strings; no `OpenSession` is sent (ICC owns it). The app builds & links for
> the iOS Simulator; on-hardware run is the remaining step. Proof ops exposed:
> `gp_iccamera_get_deviceinfo` and `gp_iccamera_eos_handshake` (drive real `ptp_*` calls).

Wire libgphoto2's PTP transport to `requestSendPTPCommand`:

- [ ] Add a transport seam to `ptp2`: a small patch to `library.c`'s `camera_init` so
      that, for our custom port, it installs **our** functions into `PTPParams` instead
      of `ptp_usb_*`:
      ```c
      params->sendreq_func  = ios_ptp_sendreq;
      params->senddata_func = ios_ptp_senddata;
      params->getresp_func  = ios_ptp_getresp;
      params->getdata_func  = ios_ptp_getdata;
      params->event_check   = ios_ptp_event_check;   // Canon: polls via 0x9116
      ```
- [ ] Implement those four/five C functions. Each assembles the PTP command container
      libgphoto2 hands it, calls out (via a C↔Swift/ObjC bridge) to
      `requestSendPTPCommand`, and returns the response + data back into libgphoto2's
      buffers. `requestSendPTPCommand` is **async**; bridge it to libgphoto2's
      **synchronous** call model with a semaphore (run libgphoto2 on a background
      queue, never the main thread).
- [ ] Apply the session strategy from Risk C (skip ptp2 OpenSession/CloseSession if ICC
      owns the session).
- [ ] Map Canon `device_flags` (e.g. `PTP_DONT_CLOSE_SESSION` for EOS R/RP) — see
      [canon-cameras.md §3](canon-cameras.md#3--device-flags--the-canon-quirk-bits).

### Phase 3 — Swift API surface (`CanonKit`)

A clean async Swift wrapper hiding all C detail (sketch in §6). Threading: all
libgphoto2 calls off-main; surface results via `async`/`await` + `AsyncStream` for events
and live-view frames.

### Phase 4 — Canon feature coverage

> **Capture + download: ✅ VALIDATED ON HARDWARE.** On a Canon EOS R50, `gp_iccamera_capture`
> fired the shutter and downloaded a **15.8 MB CR3 RAW** (`fmt 0xb108`, card object
> `storage 0x00020001`) through ptp2 over `requestSendPTPCommand` — half-press → full-press
> (busy-retry) → release, `ObjectAdded` via `ptp_check_eos_events`, then
> `ptp_getpartialobject` in ≤1 MB chunks + `ptp_canon_eos_transfercomplete`. Standard
> `GetPartialObject` worked for the card object (no EOS-specific path needed). No inline
> preview only because the body shoots RAW (UIImage can't decode CR3) — solved by shooting
> JPEG (config tree) or fetching the embedded thumbnail. Next: live view + the config tree.

Turn on capabilities, each already implemented in ptp2 — you're just exposing them:

- [ ] **Browse + download** (`gp_camera_folder_list_files`, `gp_camera_file_get`,
      thumbnails via `GP_FILE_TYPE_PREVIEW`).
- [ ] **Tethered capture** (`gp_camera_trigger_capture` + `gp_camera_wait_for_event`
      loop → `GP_EVENT_FILE_ADDED`) — the EOS event model in
      [canon-cameras.md §5](canon-cameras.md#5-eos-capture-and-the-event-queue).
- [ ] **Live view** (`gp_camera_capture_preview` → JPEG frames, target ~15–30 fps;
      backed by `GetViewFinderData`).
- [ ] **Settings** (`gp_camera_get_single_config`/`set_single_config` over the Canon
      names in [canon-cameras.md §7](canon-cameras.md#7-canon-configuration-settings-all-102)
      — `iso`, `shutterspeed`, `aperture`, `autoexposuremode`, `whitebalance`,
      `capturetarget`, …).
- [ ] **Bulb** (`bulb` action), **AF drive**, **zoom/focus** for live-view focusing.

### Phase 5 — Packaging, entitlements, submission

- [ ] Info.plist: `NSCameraUsageDescription`. Confirm no private entitlements are needed
      for Path A (there aren't — ImageCaptureCore is public).
- [ ] Handle the **connect/permission** UX (iOS prompts the user to allow the app to
      access the camera).
- [ ] Resolve LGPL (Risk B) for the actual submission; publish your libgphoto2 patches.
- [ ] Test matrix across a few Canon bodies (an EOS R/RP mirrorless, an EOS DSLR, ideally
      an older PowerShot) and both an iPhone and an iPad.

### Phase 6 *(optional)* — iPad DriverKit "pro" path

If you later want the **raw-USB** ceiling on iPad (e.g. generic non-Canon PTP cameras
whose events need the interrupt endpoint, or if Apple limits Canon opcodes on iPhone):
add a **`USBDriverKit`** system extension that claims the bulk/interrupt endpoints, and a
**second** libgphoto2 transport at the *byte* level that reuses ptp2's own `usb.c`
framing (a real `GPPortOperations` iolib). Same protocol brain, lower transport. iPad-only
(§8).

---

## 5. Build specifics

- **What to compile:** `libgphoto2/*.c` (core), `libgphoto2_port/libgphoto2_port/*.c`
  (result codes, logging, info-list — but no real iolib), `camlibs/ptp2/{ptp,ptp-pack,library,config}.c`.
  Skip `usb.c`/`ptpip.c`/`fujiptpip.c`/`olympus-wrap.c`/`chdk.c` unless you want PowerShot
  CHDK or Olympus (you don't, for Canon).
- **Omit:** all other `camlibs/*`, all `libgphoto2_port/{usb,libusb1,serial,…}` iolibs,
  ltdl (replace with static registration), libxml2, most of gettext.
- **Keep:** iconv (system), optionally libexif (for EXIF mtime; skippable).
- **Output:** one `.xcframework`; recommend a **dynamic** framework for LGPL (Risk B).
- **Threading:** libgphoto2 is blocking and not thread-safe per `Camera` — run one
  camera on one serial queue; bridge the async `requestSendPTPCommand` with a semaphore.
- **Test rig:** the `vcamera` virtual camera lets CI exercise the whole stack with no
  hardware — wire it as a second transport for unit tests.

## 6. Proposed public API (Swift sketch)

```swift
public actor CanonCamera {
    public static func discover() -> AsyncStream<CanonCamera>     // wraps ICDeviceBrowser
    public func connect() async throws                            // opens ICCameraDevice + libgphoto2

    // Files
    public func listFiles(in folder: String) async throws -> [CameraFile]
    public func download(_ file: CameraFile, kind: FileKind = .full) async throws -> Data

    // Tethered capture
    public func triggerCapture() async throws
    public func events() -> AsyncStream<CameraEvent>              // .fileAdded, .captureComplete…
    public func liveView() -> AsyncStream<UIImage>               // GetViewFinderData frames

    // Settings (typed over the config tree)
    public func get<T>(_ setting: Setting<T>) async throws -> T   // .iso, .shutterSpeed, .aperture…
    public func set<T>(_ setting: Setting<T>, _ value: T) async throws
}
```

Everything above maps 1:1 onto `gp_camera_*` calls documented in
[01-core-api.md](01-core-api.md); the `Setting<T>` keys are the Canon `name`s from
[canon-cameras.md §7](canon-cameras.md#7-canon-configuration-settings-all-102).

## 7. Fallback #1 — WiFi / PTP-IP *(recommended if USB is blocked; works on iPhone)*

If Phase 0 shows Apple refuses the Canon control opcodes over USB, the **proven** iPhone
route for full tethered control is **PTP/IP over WiFi** — which is exactly how the
established iOS camera-control apps do it, because it sidesteps Apple's USB/PTP
restrictions entirely (it's just TCP sockets, no ImageCaptureCore, no entitlements).

The best part: **the same libgphoto2 engine covers it.** ptp2 already has a full PTP/IP
transport (`camlibs/ptp2/ptpip.c` + the `ptpip` iolib, see
[03-ptp2-driver.md §4](03-ptp2-driver.md#4--transports--usbc-ptpipc-fujiptpipc)). You'd
compile that transport instead of (or alongside) the USB shim, connect to the camera's
WiFi/AP, and drive the identical Canon EOS capture/config/liveview logic. Trade-offs vs.
USB: needs the camera's WiFi enabled and pairing UX; higher latency; some bodies gate
which features are exposed over WiFi. But it is **known to work on iPhone today** and
requires no USB cable.

**Recommendation:** design the framework's transport as an interface from day one so USB
(`requestSendPTPCommand`) and WiFi (`ptpip`) are interchangeable backends behind the same
`CanonCamera` API. Then Phase 0's outcome just selects the default backend.

## 8. Fallback #2 — iPad-only USBDriverKit *(if you need raw USB control)*

If you specifically need **wired** control and Path A is blocked:

- **iPad (M-series, iPadOS 16+):** ship a `USBDriverKit` system extension (needs the
  Apple-approved `com.apple.developer.driverkit.transport.usb` entitlement) exposing bulk
  + interrupt endpoints; feed those to a **byte-level** libgphoto2 iolib that reuses
  `ptp2/usb.c` unchanged. Full wired control, but **no iPhone**, and heavier App Store
  review.
- **iPhone:** without USB PTP, wired full-control isn't possible; use WiFi (§7).

---

## 9. Status & open questions

**Locked in:** iPhone, `ICCameraDevice`, iOS 16+, App Store, full control → **Path A**.
**Phase 0 is DONE and PASSED** on a Canon EOS R50 V (see Risk A above) — iOS forwards the
vendor opcodes, so USB Path A is confirmed. Next up is **Phase 1** (build the libgphoto2
xcframework) and **Phase 2** (the transport shim), using the completion-parameter-order
and session-ownership facts recorded in Risk A.

Still open:

1. **Canon bodies to test first?** Primary is EOS mirrorless (R/RP/R-series)? Any older
   PowerShot/DSLR in scope? (Affects flag/dialect testing —
   [canon-cameras.md §2](canon-cameras.md#2-two-canon-ptp-dialects-inside-ptp2).)
2. **Licensing appetite (Risk B)?** OK to ship libgphoto2 as an embedded **dynamic**
   framework and publish your patches, or does the app need a static/closed model?
3. **Do you already see `-21249`?** If your current `requestSendPTPCommand` tests are
   returning `ICReturnPTPNotAuthorizedToSendCommand`, say so — that means we plan around
   the WiFi/PTP-IP backend (§7) from the start rather than betting on USB.
4. **Want the Phase 0 spike spelled out?** I can write the exact Swift `ICCameraDevice`
   delegate flow + the precise `Data` blobs for `GetDeviceInfo`, `EOS SetRemoteMode`,
   `EOS GetEvent`, and one live-view frame, so you can settle Risk A in an afternoon.

---

## References

- [`ICCameraDevice.requestSendPTPCommand(...)` — Apple Developer Documentation](https://developer.apple.com/documentation/imagecapturecore/iccameradevice/requestsendptpcommand(_:outdata:sendcommanddelegate:didsendcommand:contextinfo:))
- [`ICCameraDevice` — Apple Developer Documentation](https://developer.apple.com/documentation/imagecapturecore/iccameradevice)
- [ImageCaptureCore — Apple Developer Documentation](https://developer.apple.com/documentation/imagecapturecore)
- [Known-issue radar: `PTPNotAuthorizedToSendCommand` (FB7593726)](https://ww.openradar.appspot.com/FB7593726)
- [Bring your driver to iPad with DriverKit — WWDC22 (USBDriverKit is iPad-only, M-series)](https://developer.apple.com/videos/play/wwdc2022/110373/)
- [DriverKit — Apple Developer Documentation (iOS 16.0+, iPad only)](https://developer.apple.com/documentation/driverkit)
- Internal: [00-overview.md](00-overview.md) · [ptp2 driver](03-ptp2-driver.md) · [Canon guide](canon-cameras.md) · [config reference](05-config-reference.md) · [extending guide](04-extending.md)

_libgphoto2 is LGPL-2.1; this plan documents an integration, not legal advice — get a
licensing review before App Store submission (Risk B)._
