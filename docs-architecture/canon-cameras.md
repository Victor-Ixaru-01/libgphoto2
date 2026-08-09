# Canon Cameras in libgphoto2 — Complete Guide

A Canon-only companion to the [architecture docs](README.md). It pulls together every
Canon-relevant part of the codebase: which driver runs your camera, the two Canon PTP
dialects, the EOS capture/event model, all 102 Canon configuration settings, CHDK, and
the old native-protocol driver. Read the [overview](00-overview.md) first if you haven't
— this assumes you know the three-layer model (core → driver → transport).

Canon's USB vendor id throughout is **`0x04a9`**.

---

## 1. Which driver runs your Canon?

Canon support is split across **two independent camera drivers**, chosen automatically
by USB id at detection time:

| Your camera | Driver | Doc section |
|---|---|---|
| Any modern Canon: **EOS DSLR/mirrorless** (EOS R/RP/M, Rebel, xxD, xD), **PowerShot / IXUS / ELPH / SX** from ~2005 on | **`camlibs/ptp2`** (PTP/MTP) | §2–§7 (this is 95% of the guide) |
| Pre-2005 Canon "native protocol" bodies: PowerShot A5/Pro70 … A510, **EOS 350D** and earlier, over **serial or early USB** | **`camlibs/canon`** (legacy, standalone) | §8 |

The two never overlap: the ptp2 model table (`0x04a9` entries) and the legacy driver's
model list are disjoint. If your camera speaks PTP (all current ones do), you are in
ptp2. **For a project targeting today's mirrorless bodies, you only care about ptp2.**

### Canon coverage in ptp2

The `models[]` table in `camlibs/ptp2/library.c` lists **287 explicit Canon models**:

- **89 EOS** bodies — DSLRs (1D X, 5D, 6D, 7D, 80D, Rebel/xxxD…) and mirrorless
  (EOS R, RP, R5/R6 family, EOS M line).
- **~180 PowerShot / Digital IXUS / ELPH / SX** compacts.
- A handful of others.

Even a brand-new Canon not in the table still works: the generic PTP/MTP class match
(§1 of doc 03) picks it up as a generic PTP camera, and the EOS path activates as soon
as the camera reports the Canon vendor extension.

---

## 2. Two Canon PTP dialects inside ptp2

Canon implemented PTP **twice**, and ptp2 supports both as separate operation families.
Knowing which one your body uses explains almost every behavioral difference:

| | **EOS dialect** | **Legacy PowerShot PTP dialect** |
|---|---|---|
| Cameras | EOS DSLR + mirrorless (and recent PowerShot G/SX that share EOS firmware) | Older PowerShot / IXUS / ELPH |
| Operation prefix | `ptp_canon_eos_*` (17 ops) | `ptp_canon_*` (12 ops) |
| Capture handler | `camera_canon_eos_capture` / `camera_trigger_canon_eos_capture` | `camera_canon_capture` |
| Property model | Rich `PTPCanonEOSDeviceInfo` + push events | Poll `ptp_canon_getchanges` |
| Config codes | `PTP_DPC_CANON_EOS_*` | `PTP_DPC_CANON_*` |

`camera_capture` (`library.c:5779`) inspects the connected model and routes to the right
one. In the config tables the two dialects appear as separate rows sharing the same
`name` (e.g. `iso` has both a `PTP_DPC_CANON_ISOSpeed` PowerShot row and a
`PTP_DPC_CANON_EOS_ISOSpeed` EOS row); the driver auto-selects whichever the camera
supports, so you always just use `iso`.

---

## 3. Device flags — the Canon quirk bits

Each Canon row in `models[]` carries a `device_flags` bitmask (`ptp-bugs.h`,
`device-flags.h`) that toggles capabilities and workarounds:

| Flag | Meaning |
|---|---|
| `PTP_CAP` | Camera supports **remote capture** → advertises `GP_OPERATION_CAPTURE_IMAGE` + `CONFIG`. EOS bodies also get `TRIGGER_CAPTURE`. |
| `PTP_CAP_PREVIEW` | Supports **live-view preview** → `GP_OPERATION_CAPTURE_PREVIEW`. |
| `PTPBUG_DELETE_SENDS_EVENT` | Firmware fires a spurious event on object delete; the driver swallows it. Common on EOS Rebel/M bodies. |
| `PTP_DONT_CLOSE_SESSION` | Do **not** send CloseSession on exit — some bodies (EOS **R**, **RP**) hang otherwise. |

Example rows:

```c
{"Canon:EOS R",   0x04a9, 0x32da, PTP_CAP|PTP_CAP_PREVIEW|PTP_DONT_CLOSE_SESSION},
{"Canon:EOS RP",  0x04a9, 0x32e2, PTP_CAP|PTP_CAP_PREVIEW|PTP_DONT_CLOSE_SESSION},
{"Canon:EOS M3",  0x04a9, 0x3299, PTPBUG_DELETE_SENDS_EVENT|PTP_CAP|PTP_CAP_PREVIEW},
```

If you add a Canon model and capture misbehaves, the fix is almost always the right
combination of these flags (see §9).

---

## 4. EOS: the connection handshake

Before an EOS body accepts remote control, ptp2's `camera_init` (via the config
`remotemode`/`eventmode` actions and the EOS capture path) performs Canon's handshake:

1. `ptp_opensession` + `ptp_canon_eos_getdeviceinfo` → read the EOS device info
   (`PTPCanonEOSDeviceInfo`: supported props, events, formats).
2. **`PTP_OC_CANON_EOS_SetRemoteMode`** (config `remotemode`) — put the camera into PC /
   remote-control mode. Without this, most EOS ops are rejected.
3. **`PTP_OC_CANON_EOS_SetEventMode`** (config `eventmode`) — enable the push-event
   channel so the camera reports property changes and new objects.
4. Optionally `ptp_canon_eos_getremotemode` to confirm.

State from this lives in `PTPParams`: `canon_event_mode`, `eos_captureenabled`,
`eos_camerastatus`, `eos_uilocked`, and the event queue `eos_events`.

---

## 5. EOS: capture and the event queue

EOS capture is **event-driven**, which is why tethered Canon shooting feels different
from a simple "take picture" call. Flow of `camera_canon_eos_capture`
(`library.c:4490`):

1. Verify `PTP_OC_CANON_EOS_RemoteRelease` / `RemoteReleaseOn` is supported (else
   `GP_ERROR_NOT_SUPPORTED`, "your Canon camera does not support Canon EOS Capture").
2. `camera_trigger_canon_eos_capture` fires the shutter (`ptp_canon_eos_capture` /
   `RemoteReleaseOn`).
3. Loop: `ptp_check_eos_events` drains the camera's event queue, then
   `ptp_get_one_eos_event` yields events one at a time. The switch handles:

| EOS event (`PTP_EOSEvent_*`) | Driver action |
|---|---|
| `ObjectAdded` | New image on card → add to the filesystem, fill the returned `CameraFilePath`. |
| `ObjectTransfer` | Camera wants to hand over an image kept in RAM (host-storage mode) → fetch it, synthesize a `capt%04d` filename if none. |
| `ObjectRemoved` | Evict the handle from the object cache and reset the fs. |
| `PropertyChanged` | A setting changed on the camera (e.g. user turned a dial) — update the property cache. |
| `CameraStatus` / `FocusInfo` / `FocusMask` / `ObjectInfoChanged` / `ObjectContentChanged` | Bookkeeping / ignored as appropriate. |

The same events surface through `gp_camera_wait_for_event` (via
`camera_wait_for_event`) as the generic `GP_EVENT_FILE_ADDED` /
`GP_EVENT_CAPTURE_COMPLETE`, so a frontend's tether loop is portable across brands.

**Where the image lands** is controlled by the `capturetarget` config: `card`
(write to SD/CF, you download afterward) vs. `sdram`/host (streamed back via
`ObjectTransfer` without ever touching the card).

### Live view (EOS)

With `PTP_CAP_PREVIEW`, `gp_camera_capture_preview` calls
`ptp_canon_eos_get_viewfinder_image` (or the handler-streaming variant) —
`PTP_OC_CANON_EOS_GetViewFinderData` — returning a JPEG frame. The config action
`viewfinder` (`eosviewfinder`) toggles the stream; `eoszoom`/`eoszoomposition` drive
the live-view digital magnifier for focus checking.

### Bulb (EOS)

Long exposures use the paired ops `ptp_canon_eos_bulbstart` /
`ptp_canon_eos_bulbend` (`PTP_OC_CANON_EOS_BulbStart` / `BulbEnd`), exposed as the
`bulb` config action — set it on to open the shutter, off to close.

---

## 6. The Canon PTP operations (reference)

### EOS operations — `ptp_canon_eos_*` (`ptp.c`)

| Function | Purpose |
|---|---|
| `ptp_canon_eos_getdeviceinfo` | Read `PTPCanonEOSDeviceInfo` (EOS-specific capabilities). |
| `ptp_canon_eos_setremotemode` / `getremotemode` | Enter/confirm PC remote mode. |
| `ptp_canon_eos_setdevicepropvalue` / `setdevicepropvalueex` | Write an EOS setting. |
| `ptp_canon_eos_getdevicepropdesc` | Describe an EOS setting (type/range/choices). |
| `ptp_canon_eos_capture` | Fire the shutter. |
| `ptp_canon_eos_bulbstart` / `bulbend` | Open/close bulb exposure. |
| `ptp_canon_eos_getevent` | Pull the EOS push-event queue. |
| `ptp_canon_eos_get_viewfinder_image` / `_handler` | Live-view frame. |
| `ptp_canon_eos_getstorageids` / `getstorageinfo` | Storage enumeration. |
| `ptp_canon_eos_getobjectinfoex` | Object metadata. |
| `ptp_canon_eos_getpartialobject` / `getpartialobjectex` | Ranged image download. |
| `ptp_canon_eos_905f` | Vendor init/keepalive quirk op. |

### Legacy PowerShot operations — `ptp_canon_*` (`ptp.c`)

| Function | Purpose |
|---|---|
| `ptp_canon_getchanges` | Poll for changed properties (no push events). |
| `ptp_canon_checkevent` | Check the (older) event channel. |
| `ptp_canon_getviewfinderimage` | Live-view frame (PowerShot). |
| `ptp_canon_get_directory` / `gettreeinfo` / `gettreesize` | Walk the on-camera file tree. |
| `ptp_canon_getobjectinfo` / `getpartialobjectinfo` / `getpartialobject` | Object metadata + ranged download. |
| `ptp_canon_get_objecthandle_by_name` | Resolve a filename to a handle. |
| `ptp_canon_get_customize_data` | Fetch theme/customization blobs. |
| `ptp_canon_get_mac_address` | Wireless MAC. |
| `ptp_canon_getpairinginfo` | Wireless pairing info. |

---

## 7. Canon configuration settings (all 102)

Every Canon-applicable setting exposed through `config.c`, grouped by the section it
appears under in `gphoto2 --list-config`. **Applies to** shows whether the setting is
served by the EOS dialect, the PowerShot dialect, or both. **Access**: R = read-only
(status), R/W = settable. Use with `--get-config <name>` / `--set-config <name>=<value>`
or `gp_camera_get_single_config` / `_set_single_config`.

### 7.1 Camera Actions (`actions`) — momentary triggers

| Setting (`name`) | Label | Access | Applies to |
|---|---|:--:|---|
| `autofocusdrive` | Drive Canon DSLR Autofocus | R/W | EOS |
| `bulb` | Bulb Mode | R/W | EOS |
| `cancelautofocus` | Cancel Canon DSLR Autofocus | R/W | EOS |
| `chdk_script` | CHDK Script | R/W | PowerShot |
| `disablemodedial` | Canon Disable Mode Dial | R/W | PowerShot |
| `eosmoviemode` | Movie Mode | R/W | EOS |
| `eosremoterelease` | Canon EOS Remote Release | R/W | EOS |
| `eoszoom` | Canon EOS Zoom | R/W | EOS |
| `eoszoomposition` | Canon EOS Zoom Position | R/W | EOS |
| `focuslock` | Focus Lock | R/W | PowerShot |
| `manualfocusdrive` | Drive Canon DSLR Manual focus | R/W | EOS |
| `popupflash` | Popup Flash | R/W | EOS |
| `syncdatetime` | Synchronize camera date and time with PC | R/W | EOS |
| `syncdatetimeutc` | Synchronize camera date and time with PC (UTC) | R/W | EOS |
| `uilock` | UI Lock | R/W | EOS |
| `viewfinder` | Canon EOS Viewfinder | R/W | EOS |

### 7.2 Camera Settings (`settings`)

| Setting (`name`) | Label | Access | Applies to |
|---|---|:--:|---|
| `artist` | Artist | R/W | EOS |
| `autopoweroff` | Auto Power Off | R/W | EOS |
| `beep` | Beep Mode | R/W | PowerShot |
| `capture` | Capture | R/W | PowerShot |
| `capturetarget` | Capture Target | R/W | PowerShot |
| `chdk` | CHDK | R/W | PowerShot |
| `copyright` | Copyright | R/W | EOS |
| `customfuncex` | Custom Functions Ex | R/W | EOS |
| `datetime` | Camera Date and Time | R/W | EOS + PowerShot |
| `datetimeutc` | Camera Date and Time | R/W | EOS |
| `depthoffield` | Depth of Field | R/W | EOS |
| `eventmode` | Event Mode | R/W | EOS |
| `evfmode` | EVF Mode | R/W | EOS |
| `flashcharged` | Flash Charging State | R | EOS |
| `focusarea` | Focus Area | R/W | EOS |
| `focusinfo` | Focus Info | R | EOS |
| `movierecordtarget` | Recording Destination | R/W | EOS |
| `nickname` | Nickname | R/W | EOS |
| `oneshotrawon` | One Shot Raw On | R | EOS |
| `output` | Camera Output | R/W | EOS + PowerShot |
| `ownername` | Owner Name | R/W | EOS + PowerShot |
| `remotemode` | Remote Mode | R/W | EOS |
| `reviewtime` | Quick Review Time | R/W | EOS |
| `strobofiring` | Strobo Firing | R | EOS |
| `testolc` | Test OLC | R/W | EOS |

### 7.3 Camera Status (`status`) — read-only telemetry

| Setting (`name`) | Label | Access | Applies to |
|---|---|:--:|---|
| `availableshots` | Available Shots | R | EOS |
| `batterylevel` | Battery Level | R | EOS |
| `dpofversion` | DPOF Version | R | EOS |
| `eosmovieswitch` | Movie Switch | R | EOS |
| `eosserialnumber` | Serial Number | R | EOS |
| `firmwareversion` | Firmware Version | R | PowerShot |
| `lensname` | Lens Name | R | EOS |
| `mirrordownstatus` | Mirror Down Status | R | EOS |
| `mirrorlock` | Mirror Lock | R/W | EOS |
| `mirrorlockstatus` | Mirror Lock Status | R | EOS |
| `model` | Camera Model | R | EOS + PowerShot |
| `orientation` | Camera Orientation | R | PowerShot |
| `ptpversion` | PTP Version | R | EOS |
| `shuttercounter` | Shutter Counter | R | EOS |

### 7.4 Image Settings (`imgsettings`)

| Setting (`name`) | Label | Access | Applies to |
|---|---|:--:|---|
| `colorspace` | Color Space | R/W | EOS |
| `colortemperature` | Color Temperature | R/W | EOS |
| `imageformat` | Image Format | R/W | EOS + PowerShot |
| `imageformatcf` | Image Format CF | R/W | EOS |
| `imageformatexthd` | Image Format Ext HD | R/W | EOS |
| `imageformatsd` | Image Format SD | R/W | EOS |
| `imagequality` | Image Quality | R/W | PowerShot |
| `imagesize` | Image Size | R/W | PowerShot |
| `iso` | ISO Speed | R/W | EOS + PowerShot |
| `photoeffect` | Photo Effect | R/W | PowerShot |
| `whitebalance` | WhiteBalance | R/W | EOS + PowerShot |
| `whitebalanceadjusta` | WhiteBalance Adjust A | R/W | EOS |
| `whitebalanceadjustb` | WhiteBalance Adjust B | R/W | EOS |
| `whitebalancexa` | WhiteBalance X A | R | EOS |
| `whitebalancexb` | WhiteBalance X B | R | EOS |

### 7.5 Capture Settings (`capturesettings`)

| Setting (`name`) | Label | Access | Applies to |
|---|---|:--:|---|
| `aeb` | Auto Exposure Bracketing | R/W | EOS |
| `aebexpcompensation` | AEB Exposure Compensation | R/W | PowerShot |
| `afdistance` | AF Distance | R/W | PowerShot |
| `afmethod` | AF Method | R/W | EOS |
| `alomode` | Auto Lighting Optimization | R/W | EOS |
| `aperture` | Aperture | R/W | EOS + PowerShot |
| `aspectratio` | Aspect Ratio | R/W | EOS |
| `assistlight` | Assist Light | R/W | PowerShot |
| `autoexposuremode` | Canon Auto Exposure Mode | R/W | EOS |
| `autoexposuremodedial` | Canon Auto Exposure Mode Dial | R/W | EOS |
| `autorotation` | Rotation Flag | R/W | PowerShot |
| `avmax` | AV Max | R/W | PowerShot |
| `avopen` | AV Open | R/W | PowerShot |
| `bracketmode` | Bracket Mode | R/W | EOS |
| `continuousaf` | Continuous AF | R/W | EOS |
| `drivemode` | Drive Mode | R/W | EOS |
| `exposurecompensation` | Exposure Compensation | R/W | EOS + PowerShot |
| `flashcompensation` | Flash Compensation | R/W | PowerShot |
| `flashmode` | Flash Mode | R/W | PowerShot |
| `focusingpoint` | Focusing Point | R/W | PowerShot |
| `focusmode` | Focus Mode | R/W | EOS |
| `highisonr` | High ISO Noise Reduction | R/W | EOS |
| `liveviewsize` | Live View Size | R/W | EOS |
| `meteringmode` | Metering Mode | R/W | EOS + PowerShot |
| `movieservoaf` | Movie Servo AF | R/W | EOS |
| `picturestyle` | Picture Style | R/W | EOS |
| `selftimer` | Self Timer | R/W | PowerShot |
| `shootingmode` | Canon Shooting Mode | R/W | PowerShot |
| `shutterspeed` | Shutter Speed | R/W | EOS + PowerShot |
| `storageid` | Storage Device | R/W | EOS |
| `zoom` | Zoom | R/W | EOS + PowerShot |
| `zoomspeed` | Zoom Speed | R/W | EOS |

> Choice values (e.g. `picturestyle`, `drivemode`, `autoexposuremode`, `aeb`,
> `whitebalance`) come from the `canon_*` / `canon_eos_*` value tables in `config.c`
> (`canon_eos_picturestyle`, `canon_eos_drive_mode`, `canon_eos_autoexposuremode`,
> `canon_shutterspeed`, `canon_isospeed`, …). See
> [05-config-reference.md §4](05-config-reference.md#4-value-translation-choice-tables)
> for how to read them.

---

## 8. CHDK — Canon Hack Development Kit (`chdk.c`)

CHDK is third-party firmware (loaded from the SD card, non-permanent) for many
**PowerShot** compacts. When present it exposes a scripting + raw-access channel over a
single PTP opcode (`PTP_OC_CHDK`), which ptp2 wraps in `camlibs/ptp2/chdk.c`
(~1,376 lines). Sub-commands include `PTP_CHDK_Version`, `PTP_CHDK_ExecuteScript`
(run Lua on the camera), `PTP_CHDK_ScriptStatus` / `ScriptSupport`,
`PTP_CHDK_ReadScriptMsg` / `CallFunction`, `PTP_CHDK_UploadFile` / `DownloadFile` /
`TempData`, and live-view. It surfaces through two config entries:

- `chdk` (camera_settings) — enable/route CHDK mode.
- `chdk_script` (camera_actions) — execute a Lua script string on the camera.

CHDK only applies to CHDK-flashed PowerShots; EOS bodies never use it. Treat it as
optional/niche unless your project specifically targets scripted PowerShots.

---

## 9. The legacy native-protocol driver (`camlibs/canon`)

Before Canon adopted PTP, its cameras spoke a proprietary "native" protocol. The
standalone **`camlibs/canon`** driver (12,890 lines) implements it and is **completely
separate from ptp2**. Per its own about text, it drives *"Canon PowerShot, Digital IXUS,
IXY Digital, and EOS Digital cameras in their native protocol … over 70 models as old as
the PowerShot A5 / Pro70 (1998) up to the PowerShot A510 and EOS 350D (2005)."*

Its structure mirrors any camlib but with a native protocol core:

| File | Lines | Role |
|---|---:|---|
| `canon.c` | 4137 | Native protocol logic (commands, file ops, capture). |
| `usb.c` | 2608 | Native-protocol-over-USB transport. |
| `serial.c` | 1534 | Native-protocol-over-serial transport (RS-232). |
| `library.c` | 2583 | The camlib entry points (`camera_id`/`abilities`/`init`) + model table; advertises both `GP_PORT_USB` and `GP_PORT_SERIAL`. |
| `crc.c`, `util.c` | ~550 | Checksums and helpers. |

**You almost certainly do not need this** for a mirrorless/modern project — it exists
only for 1998–2005 hardware. It's listed here so you know why there are two "Canon"
things in the tree and don't wire against the wrong one. Everything from roughly the
EOS 400D / PowerShot A5xx generation onward is ptp2.

---

## 10. Canon-specific tips & gotchas

- **macOS steals the camera.** Canon EOS bodies are grabbed by Apple's `ptpcamerad` /
  Photos on connect. `killall PTPCamera` (or set Image Capture to open no app) before
  `gp_camera_init`, or you'll get a claim/`GP_ERROR_IO_USB_CLAIM` error. (General, but
  bites Canon hardest.)
- **EOS R / RP need `PTP_DONT_CLOSE_SESSION`.** If you add such a body and it hangs on
  exit, that flag is the fix.
- **Set `remotemode`/`eventmode` first.** On EOS, remote capture and property writes are
  rejected until the PC-remote handshake (§4) has run — ptp2 does this for you on the
  capture path, but if you're calling ptp ops directly, replicate it.
- **Choose `capturetarget` deliberately.** `sdram`/host capture streams the image back
  without writing the card (fast tether); `card` keeps it on the camera for later
  download. Some ops (bulb, certain M/PowerShot modes) only behave in one mode.
- **`PTPBUG_DELETE_SENDS_EVENT`.** If deleting a file on a Canon produces a stray event
  that confuses your event loop, this flag on the model row absorbs it.
- **Two dialects, one `name`.** Never hard-code `PTP_DPC_CANON_*` vs
  `PTP_DPC_CANON_EOS_*` in a frontend — always go through the config `name` and let the
  driver pick the dialect.

---

## 11. Extending Canon support

Follow the general recipes in [04-extending.md](04-extending.md), specialized for Canon:

1. **New Canon body that "mostly works":** add one row to `models[]` in
   `camlibs/ptp2/library.c` — `{"Canon:EOS <x>", 0x04a9, 0x<pid>, PTP_CAP|PTP_CAP_PREVIEW}`
   — adjusting flags (§3). Find the product id from `lsusb` / the device descriptor.
2. **New EOS operation** (an opcode ptp2 doesn't issue yet): add the `PTP_OC_CANON_EOS_*`
   constant in `ptp.h`, a `ptp_canon_eos_<op>` marshaller in `ptp.c`, and call it from
   the relevant `camera_canon_eos_*` in `library.c`.
3. **New Canon setting:** add a `struct submenu` row (EOS and/or PowerShot variant) in
   `config.c` with the property code and a value table if it's enumerated — see
   [04-extending.md §A.2](04-extending.md#a2-add-a-config-setting) and
   [05-config-reference.md](05-config-reference.md).
4. **Debug with the wire trace:** `gp_log_add_func(GP_LOG_DATA, …)` to see the exact
   Canon PTP packets; the EOS event stream is the first thing to inspect when capture
   stalls.

---

_Back to [README.md](README.md). Canon lives almost entirely in
[03-ptp2-driver.md](03-ptp2-driver.md) (protocol) and
[05-config-reference.md](05-config-reference.md) (settings); this guide is the
Canon-filtered synthesis of both._
