# Project Status & Handoff — libgphoto2 → Canon over USB on iOS

**Goal:** an iOS app/framework that drives a Canon camera over a USB cable using
libgphoto2's `ptp2` engine, via Apple's App-Store-legal `ICCameraDevice.requestSendPTPCommand`.

**Status (all validated on a real Canon EOS R50 + iPhone unless noted):**
Phases 0–3 done and working on hardware; Phase 4 (config tree) built + compiles, pending
on-hardware run. **libgphoto2 is capturing 15 MB RAW files from a Canon over USB on an iPhone.**

Two locations in play:
- **libgphoto2 repo / iOS build:** `/Volumes/Extended-2TB/GitHub/libgphoto2/.claude/worktrees/libgphoto2-docs-architecture-a36109/` (branch `claude/libgphoto2-docs-architecture-a36109`). The engine + build harness + docs.
- **Xcode app:** `/Volumes/Extended-2TB/xCode Workspace/Canon Test/` — the test harness that drives it.

---

## What's done (by phase)

### Phase 0 — Transport proven ✅ (hardware)
iOS **does** forward Canon EOS vendor PTP opcodes through `requestSendPTPCommand` (no
`-21249`). Verified: `GetDeviceInfo`, `SetRemoteMode`(0x9114), `SetEventMode`(0x9115),
`GetEvent`(0x9116), `RemoteReleaseOn/Off`(0x9128/9), `GetViewFinderData`(0x9153).
- Key facts learned:
  - Completion order is **`(inData, response, error)`** — data phase FIRST, response block SECOND.
  - `command` Data = standard 12-byte PTP container (`len·type=1·opcode·txid·params`).
  - EOS events come by **polling** `GetEvent`, not the interrupt endpoint; ImageCaptureCore also surfaces new files via `didAdd:`.
  - Harness: `Canon Test/Canon Test/PTPPhase0Tester.swift` (hand-rolled PTP; kept for reference).

### Phase 1 — libgphoto2 built for iOS ✅
Curated **core + libgphoto2_port + camlibs/ptp2** compiled to an `.xcframework`
(ios-arm64 device + universal simulator arm64/x86_64) via a direct clang build (no
autotools/meson — they're absent on the machine).
- Build harness: **`ios/`** in the libgphoto2 repo.
  - `ios/build.sh` — `test` (native macOS build + smoke test), `xcframework`, `clean`.
  - `ios/include/config.h` — hand-written feature macros for iOS/Darwin arm64.
  - `ios/include/ltdl.h` + `ios/src/ltdl_static.c` — **static module loader replacing libtool `dlopen`** (iOS bans dynamic loading of app code).
  - `ios/src/gp_ios_register.c/.h` — registers the `ptp2` camlib. **Call `gp_ios_register_all()` once at launch.**
  - `ios/README.md` — full build notes (what's included/excluded and why).
  - Generated in-tree: `libgphoto2/gphoto2-endian.h` (from the `.in`, all-`#undef` → portable LE).
- Excluded: ltdl, olympus-wrap/libxml2, libusb + serial/disk iolibs, libexif, gettext.
- Validated natively: **2471 models register** via the static loader (all Canon EOS R-series).
- Output copied to `Canon Test/Vendor/libgphoto2.xcframework`.

### Phase 2 — Transport shim ✅ (hardware)
`ptp2` ⇆ `requestSendPTPCommand`, driving the **real** engine.
- **`ios/src/gp_iccamera.c` / `.h`** (compiled INTO the framework, so it has `ptp.h`/`ptp-private.h`).
  - Implements ptp2's four transport funcs (`icc_sendreq/senddata/getdata/getresp`) over ONE app callback (`gp_iccamera_transact_cb`).
  - Buffers the command in `sendreq`, fires the callback at the data phase, returns response in `getresp`, and **forges the transaction id** to what ptp2 expects (ICC owns the real session/ids).
  - `params->data` points at a struct whose FIRST member is a `PTPData`, so ptp2's `(PTPData*)params->data` view and the shim's state share one pointer.
  - Sends **no OpenSession** (ICC owns it). iconv wired for MTP/UCS-2 strings.
  - **Vendor-id fixup:** R50 reports MTP ext `0x06`; shim forces `PTP_VENDOR_CANON` when Manufacturer contains "Canon" (mirrors ptp2's own fixup) → Canon code paths activate.
- App side: **`Canon Test/Canon Test/LibGPhoto2Tester.swift`** — implements the callback via `requestSendPTPCommand` + a semaphore, runs libgphoto2 on a background queue.
- Verified on R50: parsed real DeviceInfo (`Canon.Inc / Canon EOS R50`, 153 ops), EOS handshake `0x2001`.

### Phase 3 — Capture + download ✅ (hardware)
`gp_iccamera_capture()` in `gp_iccamera.c`: ensures EOS remote mode, half-press → full-press
(retry on `0x2019` DeviceBusy) → release, polls `ptp_check_eos_events`/`ptp_get_one_eos_event`
for `ObjectAdded`, downloads via `ptp_getpartialobject` (≤1 MB chunks) + `ptp_canon_eos_transfercomplete`.
- Verified on R50: fired shutter, downloaded a **15.8 MB CR3** (card object `storage 0x00020001`) — standard `GetPartialObject` worked for card objects.
- App: **Capture** button → saves file to Documents, shows JPEG inline (CR3/RAW won't render → no preview when the body shoots RAW).

### Phase 4 — Config tree 🔨 (built + compiles; NOT yet run on hardware)
The config.c functions (`camera_get_single_config`/`set_single_config`/`list_config`) are
**extern** (declared in `ptp-private.h`), so the shim calls them directly — full config.c
tree with human-readable value translation, no library.c patch.
- `gp_iccamera_list_config` / `get_config(name)` / `set_config(name,value)` in `gp_iccamera.c`.
- `gp_iccamera_new` now primes Canon EOS: `SetRemoteMode`+`SetEventMode`+`eos_captureenabled` then `ptp_check_eos_events` ×3 (fills `params->canon_props` so config reads find current values).
- App: `LibGPhoto2Tester` reads a curated set (`imageformat, iso, shutterspeed, aperture, whitebalance, autoexposuremode, exposurecompensation`) after engine-ready; ContentView shows each as a `Menu` picker; picking calls `gp_iccamera_set_config`.
- **Next run should:** show the settings panel; changing `imageformat` to a JPEG option would make future captures preview inline. If a setting reads "(unavailable)" or set fails, the log shows it → likely needs more `ptp_check_eos_events` priming or the value string must exactly match a choice.

---

## What still needs doing

1. **Run Phase 4 on hardware** — verify settings read/write; confirm `imageformat`→JPEG then Capture shows a preview.
2. **Live view** — `gp_iccamera_liveview_frame()` calling `ptp_canon_eos_get_viewfinder_image` (needs EVF output device set to PC, prop `0xD1B0`=2); stream frames to a SwiftUI `Image`. (Opcode `0x9153`, param `0x00200000`.)
3. **Capture polish** — embedded-thumbnail preview for RAW (`ptp_getthumb`), RAW+JPEG, save-to-Photos (needs `NSPhotoLibraryAddUsageDescription`), burst.
4. **Config polish** — expose the full `list_config` set (not just curated), typed widgets (toggle/range/date), section grouping.
5. **Turn the test app into a reusable framework** — wrap `gp_iccamera` in a clean Swift package/`CanonKit` API (`discover/connect/capture/liveView/settings`), separate from the test UI.
6. **Licensing (BLOCKER for App Store):** libgphoto2 is **LGPL-2.1**. Currently linked as a **static** `.a` inside the app — that conflicts with LGPL relink requirements. Ship libgphoto2 as an **embedded dynamic framework** + publish the iOS patches, and get a legal review before submission. (See `ios-framework-plan.md` Risk B.)
7. **macOS target caveat:** the xcframework is iOS-only; if the Mac/EDSDK target is built, filter the framework to iOS in *Build Phases → Link Binary → Filters* (the bridging header already gates the includes with `#if TARGET_OS_IOS`).
8. **iPad + non-Canon:** the same stack should work for other PTP brands (Nikon/Sony) — untested. iPad works via the same ICCameraDevice path.

---

## How to build & run

**Rebuild the engine after editing `ios/…`:**
```bash
cd /Volumes/Extended-2TB/GitHub/libgphoto2/.claude/worktrees/libgphoto2-docs-architecture-a36109
bash ios/build.sh test          # native smoke test (fast sanity)
bash ios/build.sh xcframework   # produces ios/build/libgphoto2.xcframework
cp -R ios/build/libgphoto2.xcframework "/Volumes/Extended-2TB/xCode Workspace/Canon Test/Vendor/"
```
**App:** open `Canon Test.xcodeproj`, build to a real iPhone (USB access needs hardware;
the Simulator can't see a real camera). Tap the camera → engine opens → **Capture** /
settings appear. `gp_ios_register_all()` is called in `MyApp.init()` (iOS only).

**Project wiring already in place:** xcframework linked; `-liconv` in `OTHER_LDFLAGS`;
`NSCameraUsageDescription` set; bridging header `Canon Test/Header/EDSDK-Bridging-Header.h`
includes `<gphoto2/gphoto2.h>`, `gp_ios_register.h`, `gp_iccamera.h` under `#if TARGET_OS_IOS`.

---

## Key gotchas (so they aren't rediscovered)

- **Completion arg order** is `(inData, response, error)` — getting it wrong = "can't parse the bytes".
- **Forge the transaction id** in `getresp` — ICC owns real ids; ptp2's sequence check fails otherwise.
- **Vendor-id fixup** required — R50 reports MTP `0x06`; force `PTP_VENDOR_CANON` or all Canon logic is skipped.
- **EOS config/capture need `ptp_check_eos_events` priming** after `SetEventMode` (fills the property cache).
- **`isConnected`** must be set from the `requestOpenSession` completion + `deviceDidBecomeReady` — the `didOpenSessionWithError` delegate does NOT fire with the completion-based API (this hid the Capture button once).
- **No `dlopen` on iOS** — camlibs/iolibs are statically registered via `ltdl_static.c` + `gp_ios_register.c`.
- **Can't run library.c `camera_init`** as-is (it does OpenSession, port timeouts, device reset); the shim drives ptp2 functions directly instead.
- Static lib per slice ≈ 1.8 MB; warnings during build are `%ld`/`%lld` format nits only.

---

## Reference docs (in `docs-architecture/`)
- `ios-framework-plan.md` — the phased plan with per-phase status + risks (LGPL, -21249).
- `ios-phase0-runbook.md` — the PTP-over-ICCameraDevice probe + corrected Canon opcode map.
- `canon-cameras.md` — Canon protocol details (EOS vs PowerShot dialects, opcodes, config).
- `03-ptp2-driver.md`, `05-config-reference.md` — ptp2 internals + the 359-setting config reference.
- `ios/README.md` — the iOS build harness.
