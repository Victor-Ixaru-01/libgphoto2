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
| DoEvfAf / EvfAf_ON/OFF | `ptp_canon_eos_afdrive` / `afcancel` | 🔨 | `gp_iccamera_af(on)`; app auto-releases AF 2 s after triggering (debounced) — ON must be followed by OFF |
| DriveLensEvf (Near1‑3/Far1‑3) | `ptp_canon_eos_drivelens(amount)` | 🔨 | `gp_iccamera_drivelens(amount)` |
| ExtendShutDownTimer | `ptp_canon_eos_keepdeviceon` | ✅ | in liveview_start |
| RequestRollPitchLevel | `ptp_canon_eos_setrequestrollingpitchinglevel(1)` | 🔨 | `gp_iccamera_rollpitch(on)` |
| SetRemoteShootingMode | `ptp_canon_eos_setremotemode` | ✅ | in new/capture |
| DrivePowerZoom | `PTP_OC_CANON_EOS_DrivePowerZoom` (0x914D) | 🔨 | `gp_iccamera_drive_powerzoom` (0 stop, 1 wide, 2 tele); +`PowerZoomPosition`/`Speed` props readable |
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
| Battery level | config `batterylevel` / deviceprop | ✅ read — driver fix, it reported `100%` forever. **5-state gauge on R50/R50 V, not a percentage**; can also return `"Low"`. See [canon-battery-level.md](canon-battery-level.md). |
| White Balance | config `whitebalance` | ✅ (curated; read/set/update) |
| PictureStyle | config `picturestyle` (+ `PictureStyleDesc` detail) | ⏳ partial (base style via full list; sub-params TODO) |
| AE Mode | config `autoexposuremode` | ✅ (curated) |
| Bracket | config `aeb`/`bracketmode` | ⏳ |
| EVF Output Device [Flag] | deviceprop `Evf_OutputDevice` | 🔨 (set TFT+PC in liveview) |
| EVF Zoom | `ptp_canon_eos_zoom` + `zoomposition` | 🔨 (`gp_iccamera_evf_zoom` / `_zoomposition`) |
| EVF AF Mode | config `afmethod` (`PTP_DPC_CANON_EOS_LvAfSystem`; 0–14 Quick/Live/LiveFace/… = EDSDK `Evf_AFMode`) | 🔨 (in the live panel as a picker) |
| Evf Histogram (Y/R/G/B) | **parsed from the EVF image blob** (same buffer as the LV JPEG) | 🔨 | Corrected from ❌: the histogram rides in the buffer `ptp_canon_eos_get_viewfinder_image` already returns; ptp2 just drops every non-JPEG record. `gp_iccamera_liveview_frame` now extracts 256-bucket uint32 channels by **size heuristic** (records of 1024 / 4096 bytes) → drawn as `HistogramView`. `gp_iccamera_liveview_inspect` + the **Inspect EVF** button dump all record type/len/bytes to confirm or correct the layout on the R50. |
| Drive Lens | `ptp_canon_eos_drivelens` | 🔨 (`gp_iccamera_drivelens`) |
| Drive PowerZoom | `PTP_OC_CANON_EOS_DrivePowerZoom` (0x914D) | 🔨 (`gp_iccamera_drive_powerzoom`; corrected from ❌ — the opcode does exist) |
| Depth of Field Preview | deviceprop `Evf_DepthOfFieldPreview` (0xD1B2) | 🔨 (`gp_iccamera_dof_preview`) |
| FocusShiftSet | focus-bracket deviceprops | ❓ (limited ptp2 support) |
| ApertureLockSetting | — | ❓/❌ (no known ptp2 prop) |
| Roll/Pitch live values (electronic level) | `setrequestrollingpitchinglevel` sends the request; ptp2 does **not** parse the returned level | ❓ **discovery wired** — the *request* works (`gp_iccamera_rollpitch` / "Level" button); the values aren't parsed by ptp2 (no OLC mask bit, no device prop). Added a debug surface: unhandled EOS events now carry their code + raw bytes (ptp-pack.c patch) and `poll_events` emits them as `RAW …`. On the R50, turn Level on + tilt the camera and read the `RAW`/`PROP` lines to find the code/byte layout, then parse `EdsCameraPos` (4×int32). Same class as histograms/focal-length. |

---

## EVF buffer record map (R50, observed via Inspect EVF)

Records are `[u32 len][u32 type][payload]`; `len` below is payload size. From a real R50 dump
(24 records). This is the key to the whole EVF feature set:

| type | payload | meaning (inferred) | notes |
|---|---|---|---|
| 1 | ~159 KB | **JPEG preview** | `ff d8 ff …` ✅ extracted |
| 17 | **4096** | **Histogram** | 4 channels × 256 × u32 (RGBY) — caught by our size heuristic ✅ |
| 14 | 8 | **Coordinate system size = 6000×4000** | `70 17` = 6000, `a0 0f` = 4000 (sensor space) |
| 18 | 16 | image position rect `(0,0,6000,4000)` | full-frame |
| 19 | 16 | visible/clip rect `(0,0,6000,4000)` | full-frame |
| 4 | 4 | Evf_Zoom (=1, fit) | |
| 5 | 8 | Evf_ZoomPosition `(2137,1665)` | top-left of magnified area |
| 13 | 16 | **movable AF/zoom frame** `(x,y,1200,800)` | **confirmed tracks the AF box** — moved `(2137,1665)`→`(318,2830)` between dumps. This is the R50's AF-frame source (FocusInfoEx `0xD1D3` returns nothing here). `gp_iccamera_get_evf_frame` → cyan overlay. |
| 8 | 244 | params `[10,32,1,2500,…]` | tbd |
| 10 | 8 | counter/timestamp | tbd |
| 24 / 28 / 35 | 20 / 176 / 84 | **all-zero here → likely AF-frame / focus records** | need a dump *with AF active* |
| 31 | 152 | structured `[1120,4,20,25,35,100,…]` | AF/metering config? |
| **16** | 16 | **roll/pitch level** ✅ | `[2][0][roll×100][pitch×100]`, each mod 36000 (>180°=neg). Confirmed against 4 held orientations. `gp_iccamera_get_level` → `levelText`. Always present (no Level toggle needed on R50). |
| 20,21,22,29,32,33,7 | small | misc state | tbd (29 = `ff ff` sentinel, not level) |
| 0xFFFFFFFF | 52 | header/marker (`"P:2"…`) | |
| 0 | 0 | end marker | |

**Confirmed:** histogram = **type 17** (4096 B); **coordinate space = 6000×4000** (type 14/18/19)
— that's the denominator for normalizing AF reticles *and* touch-AF taps. **Still needed:** a dump
taken *while an AF point is locked* to identify which zero record (24/28/35/31) carries the AF frames.

## Touch-AF & click-WB coordinate mapping (plan)

Goal: tap the live-view image on the phone → focus / set-WB at that point on the camera.
The finger position must be inverse-mapped into the camera's coordinate space.

**Pipeline**
1. ✅ **Screen → normalized image point.** `LiveTapView` uses `aspectRatio(.fit)` so the view == image content; the overlay `GeometryReader` gives `(nx, ny)` in `[0,1]` directly. (Built.)
2. ✅ **Normalized → camera coordinates.** The EVF **coordinate system = 6000×4000** (record **type 14**), captured per frame by `gp_iccamera_liveview_frame` → `gp_iccamera_get_coordsize` → Swift `coordW/coordH`. `handleLiveTap`: `cam = (nx·coordW, ny·coordH)`, logged. (Built.)
3. 🔨 **Send it.** `gp_iccamera_set_af_frame` → `SetLiveAfFrame` (0x915A, send-data). **EXPERIMENTAL payload** (x,y as 2×u32) — ptp2 doesn't define it; the PTP response code is logged so it can be corrected on the R50. Click-WB (`DoClickWBEvf`) still TODO.
4. **Feedback.** ✅ `FocusInfoEx` (0xD1D3) — ptp2 parses it (`ptp_unpack_EOS_FocusInfoEx`) and exposes it as the `focusinfo` config string; we patched it to prepend the AF coordinate-space size, parse the selected AF-point rects, and overlay them on the live view (`afFrames`). Draw convention (centre vs corner / origin) pending R50 calibration — the raw `focusinfo:` string is logged for that.

**Remaining unknown:** only `SetLiveAfFrame`'s exact payload (step 3) — everything else is wired.
_(older note kept for context below)_
**Blocked on discovery (do not guess the bytes):** the coordinate records' types/layout (step 2)
and `SetLiveAfFrame`'s data format (step 3). One R50 `Inspect EVF` pass + a small reverse-eng
step unblocks both; step 1 (screen math) can be built anytime.

## Implementation plan (order)
1. ✅ **Expose full config list** in the app (read/set for most property categories). Swift-only (reuses `list_config`): `LibGPhoto2Tester.loadAllSettings()` + a "Load all settings" panel in ContentView. 🔨 built.
2. ✅ **Command shims** in `gp_iccamera.c`: `af`, `bulb`, `drivelens`, `uilock`, `evf_zoom`, `evf_zoomposition`, `dof_preview`, `popupflash`, `rollpitch`. Wired to a command bar in ContentView. 🔨 built.
3. ✅ **Event poller** `gp_iccamera_poll_events` returns typed property/object/state events; Swift `pollEvents` (0.5 s loop) replaces the fixed 1.5 s settings poll — a property-changed event triggers `refreshSettings` (event-driven update), object/status/focus events go to the log. 🔨 built.
4. ✅ **State-code handling + auto-download** — `STATUS <n>` → auto `gp_iccamera_keepalive`; `OBJECT` (ObjectAdded *and* ObjectTransfer) → `gp_iccamera_download` → saved to Documents + shown if JPEG (deduped). Body-fired shots now auto-pull. 🔨 built. (Save-to-Photos + exact status-code semantics still TODO.)
5. ✅ **EVF props** — EVF AF Mode via config (`afmethod`) added to the live panel; full config list now auto-updates on property events (perf-gated ≤ once/2 s). **Histograms now parsed** from the EVF blob (size heuristic) + `HistogramView`; `Inspect EVF` confirms layout on hardware. 🔨 built.
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
- _(session 5)_ Implemented step 5: EVF AF Mode via config (`afmethod`) added to the live panel along with picturestyle/drivemode/meteringmode; full config list now auto-updates on property events (perf-gated ≤ once/2 s). Histograms marked ❌ (ptp2 doesn't parse EVF histogram blobs). **Step 6 discarded** per user — enforced the "photos stay on camera" policy: removed all device persistence from capture + auto-download (in-memory JPEG preview only; RAW body-shots left on card). 🔨 (built + compile; pending R50 run).
- _(session 6)_ Fixed dropdown choices for ISO/shutter/aperture/WB/exp-comp (and the other enum props). Root cause: the camera's available-value list arrives via `AvailListChanged` a beat after connect, which updates the ptp2 cache **silently** (no `PropertyChanged` event), so the app never re-read to pick up the choices. Fix (Swift-only): `pollEvents` no longer early-returns on empty summary, and re-reads settings during a 12 s post-connect warm-up while any enum prop still lacks choices — so `GENERIC*TABLE` choices (human-readable, from the camera list) populate and the RADIO widgets render as dropdowns. All 5 props were already in the curated panel. 🔨 (built + compile; pending R50 run).
- _(session 7)_ Roll/pitch live values: added a **debug surface** to discover them on hardware. Patched `ptp-pack.c` so unhandled EOS events keep their code + raw bytes in `u.info` (previously discarded to the debug log); `gp_iccamera_poll_events` emits these as `RAW …`; Swift logs them; added a **"Level: On/Off"** toggle (`toggleLevel` → `gp_iccamera_rollpitch`). Next R50 run: turn Level on, tilt, capture the `RAW`/`PROP` lines → then parse `EdsCameraPos`. 🔨 (built + compile).
- _(session 8)_ Fixed exposure props not showing (choices/props absent at first read): warm-up now re-reads on any event + scheduled re-reads; added exposure **sliders** (snap through the camera's values) + a diagnostic log when a setting first appears.
- _(session 9)_ **Histogram**: `gp_iccamera_liveview_frame` now also extracts histogram channels from the EVF buffer (size heuristic) and `HistogramView` draws them; `gp_iccamera_liveview_inspect` + **Inspect EVF** button dump all EVF sub-records for layout confirmation. 🔨 (built + compile; pending R50 run — the inspect output will confirm the histogram record size/order).
- _(session 10)_ **EVF/focus/zoom scalar props**: generic `gp_iccamera_get_eosprop` / `set_eosprop` (by Canon EOS code) + a "Read EVF props" panel covering EVFSharpness, EVFWBMode, EVFColorTemp, EVFRecordStatus, PowerZoomPosition, FocusMode, LV_AF_EyeDetect, AFSelectFocusArea, RefocusState, DepthOfField. **Drive PowerZoom** wired (`gp_iccamera_drive_powerzoom`, opcode 0x914D — corrected from ❌). 🔨 built. **Still discovery-gated (not done, not guessed):** `SetLiveAfFrame` (touch-AF payload), focus-drive opcodes (0x9200–02), and the EVF coordinate records (`CoordinateSystem`/`VisibleRect`/`ImagePosition`) needed to invert a screen touch → camera coords for touch-AF & click-WB. Plan below.
- _(hw)_ Identified EOS event **0xc1f6** on the R50 = shooting-settings screen open/close (payload uint32: 9 open, 0 closed). Handled Swift-side in `handleRawEvent` (parses the `RAW` debug line) → `shootingScreenOpen` + a log line. Extensible: add future discovered codes there, no C rebuild.
- _(hw)_ **Visual level indicator** added: `LevelOverlay` (artificial horizon) on the live view — fixed white centre wings + a horizon line that rolls (rotates −roll) and pitches (vertical shift), green when within 1° of level. Driven by `levelRoll`/`levelPitch`.
- _(hw)_ **Roll/pitch SOLVED** = EVF record **type 16** (not 29). Offsets 8/12 = roll×100, pitch×100 (mod 36000; >180°=negative). Verified against 4 held orientations (level/roll+pitch/forward/back). `gp_iccamera_get_level` reads type 16; Swift `levelDeg` → signed degrees → `levelText` ("level  roll +X.X°  pitch +Y.Y°"). Present even without the Level toggle. Widened Inspect EVF to 64 bytes/record (which made this findable).
- _(hw)_ **Memory crash fixed** (5 GB high-watermark). Cause: `liveViewLoop` rescheduled on the work queue as fast as frames arrived and fired several `DispatchQueue.main.async` per frame, each retaining a full-res `UIImage` → main queue flooded with frames. Fix: always `freebuf`, one coalesced main update, **reschedule the next frame only after the current is consumed** (backpressure) with a ~25 fps cap; also bounded the log buffer to 500 lines.
- _(hw)_ Confirmed the R50 **doesn't report FocusInfoEx** (`Read AF` → `rc=-1`), but its **AF/zoom box is EVF record type 13** — two dumps showed it move `(2137,1665)`→`(318,2830)` with the box. Wired `gp_iccamera_get_evf_frame` (type 13) → `evfFrame` → **cyan overlay** on the live view (distinct from the yellow FocusInfoEx path). Also: type-17 histogram confirmed live (non-zero buckets). 🔨 pending visual confirm that cyan tracks + convention (top-left vs centre).
- _(hw)_ `Read AF` returned `rc=-2 (unavailable)` = config-tree gating (the `focusinfo` widget isn't built unless the camera advertises `FocusInfoEx`). Fix: `gp_iccamera_get_focusinfo` reads `0xD1D3` **directly** from the cache (bypasses gating); refreshAFFrames now uses it. rc now = 0 (string, once camera reports it during LV/AF) or -1 (not reported yet).
- _(session 12)_ Decoded a real R50 **EVF record map** (see section above): histogram = type 17 (pinned the parser to it), coordinate space = 6000×4000 (type 14). Wired **touch-AF coordinate math**: `gp_iccamera_get_coordsize` (from type 14) → `handleLiveTap` maps tap→camera coords and calls `gp_iccamera_set_af_frame` (`SetLiveAfFrame` 0x915A, **experimental payload**, response logged). AF reticles read `focusinfo` (prompted via `get_eosprop(0xD1D3)`); still need an active-AF sample to confirm the reticle draw convention + the SetLiveAfFrame payload. 🔨 (built + compile; pending R50).
- _(session 11)_ **AF reticles displayed** (correcting session 10 — `FocusInfoEx` is *not* discovery-gated; ptp2 already parses it). Patched `ptp_unpack_EOS_FocusInfoEx` to prepend the AF coordinate-space size to the `focusinfo` string; Swift `refreshAFFrames`/`parseFocusInfo` read it during LV and overlay the selected AF-point rectangles (`afFrames`) on the live image (yellow boxes). Also built touch-AF **step 1** (tap → normalized point, logged). Both share the coordinate space → one R50 calibration pass fixes the draw convention (centre/corner/origin) and unblocks touch-AF send. 🔨 (built + compile; pending R50).
