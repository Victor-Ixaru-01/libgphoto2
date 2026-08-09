# EDSDK → libgphoto2/ptp2 Capability Map (iOS)

**Purpose.** Canon's official EDSDK is macOS-only and cannot ship on iOS (App-Store
rules + it isn't built for arm64-iOS). We use its header (`Canon Test/Header/EDSDKTypes.h`,
`EDSDK.h`) purely as the **capability spec** — the authoritative list of what a Canon body
can do — and implement each capability in our **iOS stack** instead: the `ptp2` engine driven
over `ICCameraDevice.requestSendPTPCommand` via the `gp_iccamera` shim
(`ios/src/gp_iccamera.c`) + `LibGPhoto2Tester.swift`.

This file is the **resumable tracker**: if a session is interrupted, start here.

> **Policy — photos stay on the camera.** The app must NEVER persist captured photos to the
> iOS device (no Documents write, no Photos library). Image bytes may be pulled into memory
> for a transient **preview** only (live view; a JPEG shown after a shot); the master always
> stays on the camera card. RAW body-shots aren't transferred at all (no preview possible).

**Legend**
- ✅ **Done** — implemented and verified on hardware (R50).
- 🔨 **Built** — implemented + compiles; not yet run on hardware.
- ⏳ **Feasible / TODO** — ptp2 supports it; not implemented yet.
- ❓ **Unknown** — needs investigation on hardware or in ptp2.
- ❌ **Not feasible** — no ptp2/iOS path (EDSDK-only, or needs libusb/desktop).

**Two mechanisms** for properties:
1. **Config tree** — `gp_iccamera_{list,get,set}_config` → libgphoto2 `config.c` widgets
   (human-readable names, choices, value translation). Best for the settings UI.
2. **Raw device prop** — `ptp_canon_eos_{getdevicepropdesc,setdevicepropvalue}` with a
   `PTP_DPC_CANON_EOS_*` code, when config.c has no widget for it.

`ptp_check_eos_events` + `ptp_get_one_eos_event` drain the **one** Canon event queue that
carries property-changed, object-added, and camera-status events (the "update" path).

---

## Current baseline (already implemented in earlier phases)
- Generic property **read/set/list** over config tree: `gp_iccamera_get_config` /
  `set_config` / `list_config`. ✅
- Property **update** (camera-side changes reflected): 1.5 s poll in `refreshSettings`
  (each `get_config` drains `ptp_check_eos_events`). ✅
- **Capture** (= EDSDK TakePicture) + download: `gp_iccamera_capture`. ✅ (preview only — not saved to device, per policy above)
- **Live view** frames (EVF image): `gp_iccamera_liveview_{start,frame,stop}`. 🔨
- **EVF Output Device** set to TFT+PC: inside `liveview_start`. 🔨
- **Keep-alive** (= ExtendShutDownTimer): `ptp_canon_eos_keepdeviceon` in `liveview_start`. 🔨

---

## Property categories (read / set / update)

The overwhelming majority of EDSDK property IDs are exposed by libgphoto2 `config.c` as
named widgets. Exposing the **full `list_config` set** (instead of the curated 7) covers
read+set+update for most of these in one move. Status below = per-category feasibility.

| EDSDK category | ptp2 mechanism | Read | Set | Update | Notes |
|---|---|---|---|---|---|
| Camera Settings (ProductName, OwnerName, DateTime, FirmwareVersion, BodyID, SaveTo, Artist, Copyright, storage/folder) | config tree | ⏳ | ⏳ | ⏳ | mostly config widgets; SaveTo/Artist/Copyright settable |
| Image (ImageQuality, WhiteBalance, ColorTemperature, ColorSpace, PictureStyle, Orientation) | config tree | ✅* | ✅* | ✅* | *WB/imageformat already in curated set; rest via full list |
| Image GPS (lat/long/alt/timestamp/status/…) | raw deviceprop (rational arrays) | ❓ | ❌ | ❓ | usually host-set on desktop; ptp2 has no Canon EOS GPS write path — investigate |
| Capture (AEMode, DriveMode, ISOSpeed, MeteringMode, AFMode, Av, Tv, ExposureComp, Bracket, LensName, NoiseReduction, AEModeSelect) | config tree | ✅* | ✅* | ✅* | ISO/Tv/Av/exp-comp/AE-mode in curated set; rest via full list |
| EVF (OutputDevice, Mode, WhiteBalance, ColorTemperature, DoF preview, Zoom, ZoomPosition, AFMode, Histograms, rects, PowerZoom pos) | raw deviceprop + commands | 🔨/⏳ | 🔨/⏳ | ⏳ | OutputDevice done; Zoom/ZoomPos/AFMode/DoF = commands below; histograms come in the EVF image blob |
| Limited (TempStatus, MirrorLockUp, Aspect, AutoPowerOff, media, screen timers, ContinuousAfMode, AFEyeDetect) | config tree | ⏳ | ⏳ | ⏳ | many are config widgets; some read-only |
| Flash (FlashOn, FlashMode, RedEye, Flash_Firing, Flash_Target, FEBracket) | config tree + `popupflash` | ⏳ | ⏳ | ⏳ | built-in flash popup = `ptp_canon_eos_popupflash` |
| DC (DC_Zoom, DC_Strobe, DigitalZoom, LensBarrelStatus) | config tree | ❓ | ❓ | ❓ | PowerShot-class props; R50 is EOS — likely N/A on this body |

\* = already working for the curated subset; "full list" = expose `list_config` in the app.

---

## Commands (`EdsSendCommand`) → ptp2

| EDSDK command | ptp2 function | Status | Planned shim |
|---|---|---|---|
| TakePicture | `ptp_canon_eos_capture` / remoterelease seq | ✅ | `gp_iccamera_capture` |
| ShutterButton Halfway/Completely + OFF | `ptp_canon_eos_remotereleaseon(1|2)` / `off` | ✅ | inside capture |
| BulbStart / BulbEnd | `ptp_canon_eos_bulbstart` / `bulbend` | 🔨 | `gp_iccamera_bulb(start)` |
| DoEvfAf / EvfAf_ON/OFF | `ptp_canon_eos_afdrive` / `afcancel` | 🔨 | `gp_iccamera_af(on)` |
| DriveLensEvf (Near1‑3/Far1‑3) | `ptp_canon_eos_drivelens(amount)` | 🔨 | `gp_iccamera_drivelens(amount)` |
| ExtendShutDownTimer | `ptp_canon_eos_keepdeviceon` | ✅ | in liveview_start |
| RequestRollPitchLevel | `ptp_canon_eos_setrequestrollingpitchinglevel(1)` | 🔨 | `gp_iccamera_rollpitch(on)` |
| SetRemoteShootingMode | `ptp_canon_eos_setremotemode` | ✅ | in new/capture |
| DrivePowerZoom | (no ptp2 fn) | ❌ | EVF PowerZoom position is read-only via props |
| DoClickWBEvf | (no direct fn; deviceprop) | ❓ | investigate `Evf_ClickWBCoeffs` |
| RequestSensorCleaning | (no ptp2 fn) | ❌ | — |
| SetModeDialDisable | (deviceprop?) | ❓ | investigate |
| MovieSelectSw ON/OFF | (deviceprop?) | ❓ | investigate |
| PopupBuiltinFlash | `ptp_canon_eos_popupflash` | 🔨 | `gp_iccamera_popupflash` |

## Status commands (`EdsSendStatusCommand`) → ptp2

| EDSDK status command | ptp2 function | Status | Planned shim |
|---|---|---|---|
| UILock | `ptp_canon_eos_setuilock` | 🔨 | `gp_iccamera_uilock(1)` |
| UIUnLock | `ptp_canon_eos_resetuilock` | 🔨 | `gp_iccamera_uilock(0)` |
| EnterDirectTransfer / ExitDirectTransfer | object transfer path | 🔨 | effectively covered by the auto-download path (`getpartialobject` + `transfercomplete`) on ObjectTransfer events |

---

## Events (`EdsSet*EventHandler`) → ptp2 event queue

All three EDSDK handler kinds map onto the single Canon EOS event queue drained by
`ptp_check_eos_events` → `ptp_get_one_eos_event`.

| EDSDK event | ptp2 event type | Status | Notes |
|---|---|---|---|
| Property event (PropertyChanged / DescChanged) | `PTP_EOSEvent_PropertyChanged` | 🔨 | `gp_iccamera_poll_events` → Swift `pollEvents` drains ~2×/s; a PROP event triggers `refreshSettings` (true event-driven update, no blind timer) |
| Object event (DirItemCreated / RequestTransfer) | `PTP_EOSEvent_ObjectAdded` (card) + `ObjectTransfer` (host) / `ObjectRemoved` | 🔨 | **auto-downloads** the shot: `OBJECT` event → `gp_iccamera_download(handle,fmt,size)` → saved to Documents + shown if JPEG. Deduped by handle. Covers shots fired on the camera body. |
| State event (Shutdown / WillSoonShutDown / AfResult / JobStatus) | `PTP_EOSEvent_CameraStatus` / `FocusInfo` | 🔨 | `STATUS <n>` → auto `gp_iccamera_keepalive` (keep-awake); `FOCUS` logged as AF result. Exact status-code meanings need HW verification. |

---

## Specific items from the request

| Item | ptp2 path | Status |
|---|---|---|
| Battery level | config `batterylevel` / deviceprop | ⏳ (read + update) |
| White Balance | config `whitebalance` | ✅ (curated; read/set/update) |
| PictureStyle | config `picturestyle` (+ `PictureStyleDesc` detail) | ⏳ partial (base style via full list; sub-params TODO) |
| AE Mode | config `autoexposuremode` | ✅ (curated) |
| Bracket | config `aeb`/`bracketmode` | ⏳ |
| EVF Output Device [Flag] | deviceprop `Evf_OutputDevice` | 🔨 (set TFT+PC in liveview) |
| EVF Zoom | `ptp_canon_eos_zoom` + `zoomposition` | 🔨 (`gp_iccamera_evf_zoom` / `_zoomposition`) |
| EVF AF Mode | config `afmethod` (`PTP_DPC_CANON_EOS_LvAfSystem`; 0–14 Quick/Live/LiveFace/… = EDSDK `Evf_AFMode`) | 🔨 (in the live panel as a picker) |
| Evf Histogram (Y/R/G/B) | EVF image blob (ptp2 parses JPEG only) | ❌ | EDSDK exposes these via the EvfImageRef; ptp2 doesn't parse the histogram sub-blobs — would need reverse-engineering the blob format on hardware |
| Drive Lens | `ptp_canon_eos_drivelens` | 🔨 (`gp_iccamera_drivelens`) |
| Drive PowerZoom | — | ❌ (no ptp2 fn; position props read-only) |
| Depth of Field Preview | deviceprop `Evf_DepthOfFieldPreview` (0xD1B2) | 🔨 (`gp_iccamera_dof_preview`) |
| FocusShiftSet | focus-bracket deviceprops | ❓ (limited ptp2 support) |
| ApertureLockSetting | — | ❓/❌ (no known ptp2 prop) |

---

## Implementation plan (order)
1. ✅ **Expose full config list** in the app (read/set for most property categories). Swift-only (reuses `list_config`): `LibGPhoto2Tester.loadAllSettings()` + a "Load all settings" panel in ContentView. 🔨 built.
2. ✅ **Command shims** in `gp_iccamera.c`: `af`, `bulb`, `drivelens`, `uilock`, `evf_zoom`, `evf_zoomposition`, `dof_preview`, `popupflash`, `rollpitch`. Wired to a command bar in ContentView. 🔨 built.
3. ✅ **Event poller** `gp_iccamera_poll_events` returns typed property/object/state events; Swift `pollEvents` (0.5 s loop) replaces the fixed 1.5 s settings poll — a property-changed event triggers `refreshSettings` (event-driven update), object/status/focus events go to the log. 🔨 built.
4. ✅ **State-code handling + auto-download** — `STATUS <n>` → auto `gp_iccamera_keepalive`; `OBJECT` (ObjectAdded *and* ObjectTransfer) → `gp_iccamera_download` → saved to Documents + shown if JPEG (deduped). Body-fired shots now auto-pull. 🔨 built. (Save-to-Photos + exact status-code semantics still TODO.)
5. ✅ **EVF props** — EVF AF Mode via config (`afmethod`) added to the live panel; full config list now auto-updates on property events (perf-gated ≤ once/2 s). Histograms = ❌ (ptp2 doesn't parse the EVF histogram sub-blobs; EDSDK-only). 🔨 built.
6. ~~Save to Photos~~ — **DISCARDED by user.** Policy: captured photos ALWAYS remain on the camera; the app never persists them to the device (no Documents write, no Photos). In-memory preview only. Capture + auto-download were updated to match; RAW body-shots are left on the card (not transferred).
7. ⏳ Investigate the ❓ items on hardware (GPS write, DoClickWB, mode-dial disable, movie SW, focus-shift, ApertureLock), and confirm `STATUS` code meanings. **← next**
   Run **`R50-TEST-CHECKLIST.md`** first to flip the 🔨 items to ✅.

## What's implemented in code (files)
- `ios/src/gp_iccamera.c` / `.h` — command shims (§ "Canon EOS commands (EDSDK-equivalent)"), `gp_iccamera_poll_events` (§ "Event-driven update"), `gp_iccamera_download` (auto-pull by handle), `gp_iccamera_keepalive`.
- `Canon Test/Canon Test/LibGPhoto2Tester.swift` — `loadAllSettings()` + `refreshAllSettings()` (perf-gated full-tree auto-update), `allSettings`, command methods (`autoFocus`, `bulb`, `driveLens`, `uiLock`, `evfZoom`, `evfZoomPosition`, `depthOfFieldPreview`, `popupFlash`, `requestRollPitch`), the event loop (`startEventLoop`/`pollEvents`) driving `refreshSettings` on property changes, and `downloadObject`/`keepAlive` reacting to object/status events. Curated live panel now includes `afmethod` (EVF AF Mode), `picturestyle`, `drivemode`, `meteringmode`. Capture + `downloadObject` are **preview-only** (no device persistence).
- `Canon Test/Canon Test/ContentView.swift` — `commandBar` + `allSettingsSection`.
- xcframework rebuilt (`ios/build.sh xcframework`) and copied to `Canon Test/Vendor/`; app compiles + links (device symbols present, iOS-sim build OK).

## Progress log
- _(init)_ Investigation complete; map created. Baseline = phases 0–5 (see `PROJECT-STATUS.md`).
- _(session 2)_ Implemented plan steps 1–2: full config-tree read/set exposure + 9 EDSDK-equivalent command shims (AF, bulb, drive-lens, UI lock, EVF zoom/zoom-pos, DoF preview, popup flash, roll/pitch). All 🔨 (built + compile; pending R50 run).
- _(session 3)_ Implemented plan step 3: `gp_iccamera_poll_events` + Swift event loop. Replaced the fixed 1.5 s settings poll with a 0.5 s EOS event drain — property-changed events trigger a settings re-read (event-driven update), object/status/focus events are logged. 🔨 (built + compile; pending R50 run).
- _(session 4)_ Implemented plan step 4: `gp_iccamera_download` (auto-pull by handle) + `gp_iccamera_keepalive`; `poll_events` now also emits `OBJECT` for `ObjectTransfer` (SaveTo=host). Swift `downloadObject` handles body-fired shots (deduped); `keepAlive` fires on status events. 🔨 (built + compile; pending R50 run).
- _(session 5)_ Implemented step 5: EVF AF Mode via config (`afmethod`) added to the live panel along with picturestyle/drivemode/meteringmode; full config list now auto-updates on property events (perf-gated ≤ once/2 s). Histograms marked ❌ (ptp2 doesn't parse EVF histogram blobs). **Step 6 discarded** per user — enforced the "photos stay on camera" policy: removed all device persistence from capture + auto-download (in-memory JPEG preview only; RAW body-shots left on card). 🔨 (built + compile; pending R50 run). Next: step 7 (hardware investigation of ❓ items).
