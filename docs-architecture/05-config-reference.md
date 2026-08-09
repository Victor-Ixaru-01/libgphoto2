# 05 — PTP2 Configuration Reference (`config.c`)

This is the full reference for **every camera setting** libgphoto2 exposes through the
PTP/MTP driver. It expands doc [03 §6](03-ptp2-driver.md#6--configc--the-settings-tree-13559-lines).
`config.c` (13,559 lines) is the single largest file in the project and *is* the
camera-configuration surface: it declares the `CameraWidget` tree returned by
`gp_camera_get_config()` and consumes the edited tree in `gp_camera_set_config()`.

The tables below are generated directly from the source, so they list the actual
`name` keys you pass to the API / CLI (`gphoto2 --get-config <name>` /
`--set-config <name>=<value>`, or `gp_camera_get_single_config`/`_set_single_config`).

**Totals:** 6 top-level sections (5 setting tables + WiFi profiles), **359 distinct
setting names** (367 counting the few names that recur across sections), backed by
**502 declarations** — one setting often has several vendor-specific implementations —
plus **219 value-translation tables** for the enumerated choices, and 13 per-model
Nikon override tables.

---

## 1. How a setting is declared

Every setting is one row in a `struct submenu[]` array (`config.c:684`):

```c
struct submenu {
    char     *label;    // human label, e.g. N_("ISO Speed")  — translatable
    char     *name;     // stable programmatic key, e.g. "iso" — what you pass to the API
    uint32_t  propid;   // PTP device-property code (PTP_DPC_*) or an opcode (PTP_OC_*)
    uint16_t  vendorid; // which vendor this row applies to (PTP_VENDOR_*), or 0 = generic
    uint32_t  type;     // PTP data type (PTP_DTC_UINT16, _STR, _INT32, …)
    get_func  getfunc;  // builds the CameraWidget from the camera's current value
    put_func  putfunc;  // writes the widget's new value back to the camera
};
```

- **`get_func`** signature: `(Camera*, CameraWidget **widget, struct submenu *menu, PTPDevicePropDesc *dpd)`.
  It creates the widget (radio/toggle/range/text/date), fills in current value + choices
  from the device-property descriptor `dpd`, and returns it.
- **`put_func`** signature: `(Camera*, CameraWidget *widget, PTPPropValue *propval, PTPDevicePropDesc *dpd, int *alreadyset)`.
  It reads the user's edited widget value and marshals it into `propval` for a
  `ptp_setdevicepropvalue` (or fires an opcode directly for action-style settings).

**Multiple rows, one `name`.** The same setting name (e.g. `iso`) appears once per
vendor, each with that vendor's `propid`/`getfunc`/`putfunc`. At runtime the driver
picks the row whose `vendorid` matches the connected camera *and* whose `propid` the
camera actually supports. That's why the tables show, for example, `iso` "Supported by:
Canon, Fuji, Nikon, Olympus/OM, Panasonic, Sony ×11" — eleven implementations behind one
key. This is invisible to you as an API user: you always just get/set `iso`.

Rows where **`putfunc` is `_put_None`** are **read-only** (status/telemetry). Marked
**R** in the tables below; everything else is **R/W**.

## 2. How settings are grouped — the `menus[]` table

The top of the tree is `struct menu menus[]` (`config.c:12464`), an array of sections.
Each `menu` names a section and either points to a `submenu[]` array or supplies custom
get/put callbacks (WiFi profiles). Sections carry an optional `(usb_vendorid,
usb_productid)` filter so a **specific body can override a whole section** — this is how
per-model Nikon tables are swapped in (see §8):

| Section (`name`) | Label | Contents |
|---|---|---|
| `actions` | Camera Actions | One-shot triggers: autofocus, bulb, capture, movie, viewfinder, manual-focus drive, opcode. |
| `settings` | Camera Settings | Device-level prefs: clock, owner/artist/copyright, capture target, beep, output. (Nikon D7000/D7100/D90/1-series get model-specific variants.) |
| `status` | Camera Status Information | Read-only telemetry: battery, lens, shutter count, focus/AE/AF lock, orientation. |
| `imgsettings` | Image Settings | "Film" properties: ISO, white balance, image format/size, color space, video/audio. |
| `capturesettings` | Capture Settings | The big one: exposure mode, aperture, shutter, metering, bracketing, flash, focus, HDR, live view, Pro Capture, etc. (many per-model Nikon overrides). |
| `wifiprofiles` | WIFI profiles | Nikon WLAN profile management (custom get/put, not a submenu table). |

The generic (`vendorid 0`, `productid 0`) row is always **last** in each section so
model-specific rows win first.

## 3. Generic get/put helpers

Most rows reuse a small set of shared callbacks instead of bespoke code:

| Helper | Meaning |
|---|---|
| `_get_INT` / `_put_INT` | Plain integer property (range or free value). |
| `_get_STR` / `_put_STR` | String property. |
| `_get_Range_INT8` / `_put_Range_INT8` | Signed 8-bit range (e.g. light meter). |
| `_put_None` | **Read-only** — no write path. |
| `_get_STR_ENUMList` | String value chosen from the device's enumerated list. |
| `_get_Nikon_OnOff_UINT8` / `_get_Nikon_OffOn_UINT8` | Boolean toggle (on/off) as UINT8. |
| `_get_UINT32_as_time` / `_as_localtime`, `_get_STR_as_time` | Clock settings ↔ `time_t`. |
| `_get_AUINT8_as_CHAR_ARRAY` | Byte-array string (owner name). |
| `_get_BatteryLevel` | Battery percentage. |
| `_get_GenericU16Table` / `_put_GenericU16Table` (+ U8/I16/16/8 variants) | **The workhorse:** map a numeric property value ↔ a human label via a `deviceproptable*` choice table (§4). |

## 4. Value-translation (choice) tables

Enumerated settings show words, not raw numbers, via ~219 `struct deviceproptable*`
tables. Each entry is `{ label, numeric_value, vendor_id }`; the generic table helper
walks the device's supported-values list and renders the matching labels (unknown values
appear as `Unknown value 0xNNNN`). Representative tables and their choices:

- **`whitebalance`** (45 entries): Manual, Automatic, One-push Automatic, Daylight,
  Fluorescent, Tungsten, Flash, Cloudy, Shade, Color Temperature, Preset, Natural light
  auto, Fluorescent Lamp 1–7, Underwater, …
- **`focusmodes`** (16): Manual, Automatic, Automatic Macro, AF-S, AF-C, AF-A, AF-F,
  Single/Continuous-Servo AF, DMF, AF-D, Preset Focus, …
- **`capture_mode`** (64): Single Shot, Burst, Timelapse, Continuous Low/High Speed,
  Mirror Up, Remote, Quick/Delayed Response Remote, Quiet Release, Selftimer 2/5/10s, …
- **`exposure_program_modes`** (44): P/A/S/M, Auto, Portrait, Landscape, Macro, Sports,
  Night Portrait/Landscape, Children, U1/U2/U3, Intelligent Auto, Scene, …
- **`canon_eos_drive_mode`** (14): Single, Continuous, Video, Continuous high/low speed,
  Silent shooting, Timer 10s/2s, Super high speed continuous, …
- **`flash_mode`** (26): Automatic Flash, Flash off, Fill flash, Red-eye auto/fill,
  External sync, Slow Sync, Rear/Front-curtain sync, Red-eye reduction, …

There are per-vendor variants of most of these (`olympus_whitebalance`,
`fuji_filmsimulation`, `canon_eos_picturestyle`, `nikon_scenemode`, `sony_aspectratio`,
etc.). To see the exact numeric mapping for any setting, open its table in `config.c`
(they're grouped between lines 1453 and ~11550) — the label→value pairs are the
authoritative choice list.

---

## 5. Camera Actions — `actions`

One-shot triggers and momentary controls (autofocus, shutter, bulb, movie, live-view
toggles, focus drive). Setting the widget *performs* the action.

| Setting name (`name`) | Label | Access | Supported by |
|---|---|:--:|---|
| `autofocus` | Auto-Focus | R/W | Sony |
| `autofocusdrive` | Drive Nikon DSLR Autofocus | R/W | Canon, Fuji, Nikon ×3 |
| `autofocusdrivemanual` | Drive Fuji Autofocus in manual | R/W | Fuji |
| `bulb` | Bulb Mode | R/W | Canon, Fuji, Nikon, Olympus/OM, Panasonic, Sony ×6 |
| `cancelautofocus` | Cancel Canon DSLR Autofocus | R/W | Canon |
| `capture` | Capture | R/W | Sony |
| `changeafarea` | Set Nikon Autofocus area | R/W | Nikon |
| `chdk_script` | CHDK Script | R/W | Canon |
| `controlmode` | Set Nikon Control Mode | R/W | Nikon |
| `disablemodedial` | Canon Disable Mode Dial | R/W | Canon |
| `eosmoviemode` | Movie Mode | R/W | Canon |
| `eosremoterelease` | Canon EOS Remote Release | R/W | Canon |
| `eoszoom` | Canon EOS Zoom | R/W | Canon |
| `eoszoomposition` | Canon EOS Zoom Position | R/W | Canon |
| `focuslock` | Focus Lock | R/W | Canon |
| `focusmagnifier` | Focus Magnifier | R/W | Sony ×2 |
| `focusmagnifierexit` | Focus Magnifier Cancel | R/W | Sony |
| `focuspoint` | Get Fuji focuspoint | R/W | Fuji |
| `focuspoints` | Fuji FocusPoint Grid dimensions | R/W | Fuji |
| `manualfocus` | Manual-Focus | R/W | Sony |
| `manualfocusdrive` | Drive Olympus OMD Manual focus | R/W | Canon, Fuji, Nikon, Olympus/OM, Panasonic ×5 |
| `movie` | Movie Capture | R/W | Nikon, Panasonic, Sony ×5 |
| `opcode` | PTP Opcode | R/W | generic (any PTP) |
| `popupflash` | Popup Flash | R/W | Canon |
| `powerdown` | Power Down | R/W | generic (any PTP) |
| `remotekeydown` | Remote Key Down | R/W | Sony |
| `remotekeyleft` | Remote Key Left | R/W | Sony |
| `remotekeyright` | Remote Key Right | R/W | Sony |
| `remotekeyup` | Remote Key Up | R/W | Sony |
| `spotfocusarea` | Spot Focus Area | R/W | Sony |
| `syncdatetime` | Synchronize camera date and time with PC | R/W | Canon |
| `syncdatetimeutc` | Synchronize camera date and time with PC (UTC) | R/W | Canon |
| `uilock` | UI Lock | R/W | Canon |
| `viewfinder` | Panasonic Viewfinder | R/W | Canon, Nikon, Panasonic ×3 |
| `zoompos` | Fuji Zoom Position | R | Fuji |

> Notes: `bulb` starts/stops a long exposure (vendor-specific start/stop opcodes);
> `manualfocusdrive` nudges focus by steps (sign/magnitude = direction/amount);
> `viewfinder` toggles the live-view stream on/off; `opcode` is an escape hatch to issue
> an arbitrary raw PTP opcode.

## 6. Camera Status Information — `status`

Read-only telemetry (all **R**). Useful for a tethering UI's status bar.

| Setting name (`name`) | Label | Access | Supported by |
|---|---|:--:|---|
| `acpower` | AC Power | R | Nikon |
| `activefolder` | Active Folder | R | Nikon |
| `aelocked` | AE Locked | R | Nikon |
| `aflocked` | AF Locked | R | Nikon |
| `apertureatmaxfocallength` | Maximum Aperture at Focal Length Maximum | R | Nikon |
| `apertureatminfocallength` | Maximum Aperture at Focal Length Minimum | R | Nikon |
| `availableshots` | Available Shots | R | Canon, Fuji, Nikon ×3 |
| `batterylevel` | Battery Level | R | Canon, Sony ×3 |
| `cameramodel` | Camera Model | R | generic (any PTP) |
| `continousshootingcount` | Continuous Shooting Count | R | Nikon |
| `deviceversion` | Device Version | R | generic (any PTP) |
| `dpofversion` | DPOF Version | R | Canon |
| `eosmovieswitch` | Movie Switch | R | Canon |
| `eosserialnumber` | Serial Number | R | Canon |
| `externalflash` | External Flash | R | Nikon |
| `externalflashready` | External Flash Ready | R | Nikon |
| `firmwareversion` | Firmware Version | R | Canon |
| `flashcharged` | Flash Charged | R | Nikon |
| `flashopen` | Flash Open | R | Nikon |
| `focusindication` | Focus Indication | R | Sony |
| `fvlocked` | FV Locked | R | Nikon |
| `lensname` | Lens Name | R | Canon, Fuji, Nikon ×3 |
| `lightmeter` | Light Meter | R | Nikon ×2 |
| `liveviewprohibit` | Liveview Prohibit Condition | R | Nikon |
| `lowlight` | Low Light | R | Nikon |
| `manufacturer` | Camera Manufacturer | R | generic (any PTP) |
| `maxfocallength` | Focal Length Maximum | R | Nikon |
| `minfocallength` | Focal Length Minimum | R | Nikon |
| `mirrordownstatus` | Mirror Down Status | R | Canon |
| `mirrorlock` | Mirror Lock | R/W | Canon |
| `mirrorlockstatus` | Mirror Lock Status | R | Canon |
| `mirrorupshootingcount` | Mirror Up Shooting Count | R | Nikon |
| `mirrorupstatus` | Mirror Up Status | R | Nikon |
| `model` | Camera Model | R | Canon ×2 |
| `movieprohibit` | Movie Prohibit Condition | R | Nikon |
| `orientation` | Camera Orientation | R | Canon, Nikon ×2 |
| `orientation2` | Camera Orientation | R | Nikon |
| `ptpversion` | PTP Version | R | Canon |
| `serialnumber` | Serial Number | R | generic (any PTP) |
| `shuttercounter` | Shutter Counter | R | Canon, Fuji ×2 |
| `vendorextension` | Vendor Extension | R | generic (any PTP) |

## 7. Camera Settings — `settings`

Device-level preferences and identity.

| Setting name (`name`) | Label | Access | Supported by |
|---|---|:--:|---|
| `artist` | Artist | R/W | Canon, Nikon ×2 |
| `autofocus` | Autofocus | R/W | Nikon |
| `autopoweroff` | Auto Power Off | R/W | Canon |
| `beep` | Beep Mode | R/W | Canon |
| `cameraaction` | Camera Action | R/W | Fuji |
| `capture` | Capture | R/W | Canon |
| `capturetarget` | Capture Target | R/W | Canon, Nikon, Panasonic, Sony ×4 |
| `ccdnumber` | CCD Number | R | Nikon |
| `chdk` | CHDK | R/W | Canon |
| `cleansensor` | Clean Sensor | R/W | Nikon |
| `copyright` | Copyright | R/W | Canon, Fuji, Nikon ×3 |
| `csmmenu` | CSM Menu | R/W | Nikon |
| `customfuncex` | Custom Functions Ex | R/W | Canon |
| `datetime` | Camera Date and Time | R/W | Canon, Sony ×4 |
| `datetimeutc` | Camera Date and Time | R/W | Canon |
| `depthoffield` | Depth of Field | R/W | Canon |
| `devicename` | Device Name | R/W | Fuji |
| `eventmode` | Event Mode | R/W | Canon |
| `evfmode` | EVF Mode | R/W | Canon |
| `externalrecordingcontrol` | External Recording Control | R/W | Nikon |
| `fastfs` | Fast Filesystem | R/W | Nikon |
| `flashcharged` | Flash Charging State | R | Canon |
| `flickerreduction` | Flicker Reduction | R/W | Nikon |
| `focusarea` | Focus Area | R/W | Canon |
| `focusinfo` | Focus Info | R | Canon |
| `guid` | WLAN GUID | R/W | Nikon |
| `imagecomment` | Image Comment | R/W | Fuji, Nikon ×2 |
| `imagecommentenable` | Enable Image Comment | R/W | Nikon |
| `infodisperrstatus` | Info Display Error Status | R | Nikon |
| `lcdofftime` | LCD Off Time | R/W | Nikon |
| `menusandplayback` | Menus and Playback | R/W | Nikon |
| `movierecordtarget` | Recording Destination | R/W | Canon |
| `nickname` | Nickname | R/W | Canon |
| `oneshotrawon` | One Shot Raw On | R | Canon |
| `output` | Camera Output | R/W | Canon ×2 |
| `ownername` | Owner Name | R/W | Canon ×2 |
| `prioritymode` | Priority Mode | R/W | Fuji, Sony ×2 |
| `recordingmedia` | Recording Media | R/W | Nikon |
| `remotemode` | Remote Mode | R/W | Canon |
| `reversedial` | Reverse Command Dial | R/W | Nikon |
| `reviewtime` | Quick Review Time | R/W | Canon |
| `strobofiring` | Strobo Firing | R | Canon |
| `testolc` | Test OLC | R/W | Canon |
| `thumbsize` | Thumb Size | R/W | Nikon |

> Several entries here are **virtual** (`propid 0`) — they don't map to a single PTP
> property but to driver behavior: `capturetarget` (save to card vs. RAM/host),
> `autofocus`, `thumbsize`, `fastfs`, `remotemode`, `eventmode`. `capturetarget` in
> particular is the one you set before tethered capture to decide where the image lands.

## 8. Image Settings — `imgsettings`

The "film" properties — the ones a photographer changes shot to shot.

| Setting name (`name`) | Label | Access | Supported by |
|---|---|:--:|---|
| `audiobitpersample` | Audio Bit per Sample | R/W | generic (any PTP) |
| `audiobitrate` | Audio Bitrate | R/W | generic (any PTP) |
| `audioformat` | Audio Format | R/W | generic (any PTP) |
| `audiosamplingrate` | Audio Sampling Rate | R/W | generic (any PTP) |
| `audiovolume` | Audio Volume | R/W | generic (any PTP) |
| `autoiso` | Auto ISO | R/W | Nikon |
| `colormodel` | Color Model | R/W | Nikon |
| `colorspace` | Color Space | R/W | Canon, Fuji, Nikon ×3 |
| `colortemperature` | Color Temperature | R/W | Canon, Fuji, Olympus/OM, Sony ×4 |
| `filmsimulation` | Film Simulation | R/W | Fuji |
| `graineffect` | Grain Effect | R/W | Fuji |
| `imageformat` | Image Format | R/W | Canon, Fuji, Olympus/OM, Panasonic ×5 |
| `imageformatcf` | Image Format CF | R/W | Canon |
| `imageformatexthd` | Image Format Ext HD | R/W | Canon |
| `imageformatsd` | Image Format SD | R/W | Canon |
| `imagequality` | Image Quality | R/W | Canon |
| `imagesize` | Image Size | R/W | Canon, Nikon, Sony ×4 |
| `iso` | ISO Speed | R/W | Canon, Fuji, Nikon, Olympus/OM, Panasonic, Sony ×11 |
| `isoauto` | ISO Auto | R/W | Nikon |
| `isoautomode` | ISO Auto Mode | R/W | Olympus/OM |
| `movieiso` | Movie ISO Speed | R/W | Nikon |
| `photoeffect` | Photo Effect | R/W | Canon |
| `rawimagesize` | Raw Image Size | R/W | Nikon |
| `videobrightness` | Video Brightness | R/W | generic (any PTP) |
| `videocontrast` | Video Contrast | R/W | generic (any PTP) |
| `videoformat` | Video Format | R/W | generic (any PTP) |
| `videoframerate` | Video Framerate | R/W | generic (any PTP) |
| `videoquality` | Video Quality | R/W | generic (any PTP) |
| `videoresolution` | Video Resolution | R/W | generic (any PTP) |
| `whitebalance` | WhiteBalance | R/W | Canon, Nikon, Olympus/OM ×5 |
| `whitebalanceadjusta` | WhiteBalance Adjust A | R/W | Canon, Olympus/OM ×2 |
| `whitebalanceadjustb` | WhiteBalance Adjust B | R/W | Canon, Olympus/OM ×2 |
| `whitebalancexa` | WhiteBalance X A | R | Canon |
| `whitebalancexb` | WhiteBalance X B | R | Canon |

## 9. Capture Settings — `capturesettings`

The largest section: exposure control, focus, metering, bracketing, flash, live view,
HDR, and the OM/Olympus Pro-Capture / high-res family. 213 distinct settings.

| Setting name (`name`) | Label | Access | Supported by |
|---|---|:--:|---|
| `adlbracketingpattern` | ADL Bracketing Pattern | R/W | Nikon |
| `adlbracketingstep` | ADL Bracketing Step | R/W | Nikon |
| `aeb` | Auto Exposure Bracketing | R/W | Canon |
| `aebexpcompensation` | AEB Exposure Compensation | R/W | Canon |
| `aebracketingcount` | AE Bracketing Count | R | Nikon |
| `aebracketingpattern` | AE Bracketing Pattern | R/W | Nikon |
| `aebracketingstep` | AE Bracketing Step | R/W | Nikon |
| `aelaflmode` | AE-L/AF-L Mode | R/W | Nikon |
| `af-area-illumination` | AF Area Illumination | R/W | Nikon |
| `afareasetting` | AF Area Setting | R/W | Olympus/OM |
| `afbeep` | AF Beep Mode | R/W | Nikon |
| `afdistance` | AF Distance | R/W | Canon |
| `afmethod` | AF Method | R/W | Canon |
| `afmode` | AF Mode | R/W | Panasonic |
| `afpoint` | AF Point | R/W | Olympus/OM |
| `alomode` | Auto Lighting Optimization | R/W | Canon |
| `aperture` | Aperture | R/W | Canon, Fuji, Nikon, Olympus/OM, Sigma ×6 |
| `aperture2` | Aperture 2 | R/W | Nikon |
| `applicationmode` | Application Mode | R/W | Nikon ×2 |
| `aspectratio` | Aspect Ratio | R/W | Canon, Olympus/OM, Sony ×3 |
| `assistlight` | Assist Light | R/W | Canon, Nikon ×2 |
| `autodistortioncontrol` | Auto Distortion Control | R/W | Nikon |
| `autoexposuremode` | Canon Auto Exposure Mode | R/W | Canon |
| `autoexposuremodedial` | Canon Auto Exposure Mode Dial | R/W | Canon |
| `autofocusarea` | Auto Focus Area | R/W | Nikon |
| `autofocusmode2` | Auto Focus Mode 2 | R/W | Nikon |
| `autorotation` | Rotation Flag | R/W | Canon |
| `autowhitebias` | Auto White Balance Bias | R/W | Nikon ×2 |
| `avmax` | AV Max | R/W | Canon |
| `avopen` | AV Open | R/W | Canon |
| `bracketing` | Bracketing | R/W | Nikon |
| `bracketmode` | Bracket Mode | R/W | Canon, Nikon ×2 |
| `bracketorder` | Bracket Order | R/W | Nikon |
| `bracketset` | Bracket Set | R/W | Nikon |
| `burstinterval` | Burst Interval | R/W | generic (any PTP) |
| `burstnumber` | Burst Number | R/W | generic (any PTP) |
| `capturedelay` | Capture Delay | R/W | generic (any PTP) |
| `capturemode` | Still Capture Mode | R/W | Fuji, Sony ×3 |
| `centerweightsize` | Center Weight Area | R/W | Nikon |
| `cloudywhitebias` | Cloudy White Balance Bias | R/W | Nikon ×2 |
| `colortemperature` | Color temperature | R/W | Panasonic |
| `continuousaf` | Continuous AF | R/W | Canon |
| `custommode` | Custom Mode | R/W | Olympus/OM |
| `custommodedial` | Custom Mode Dial | R/W | Olympus/OM |
| `daylightwhitebias` | Daylight White Balance Bias | R/W | Nikon ×2 |
| `drivemode` | Drive Mode | R/W | Canon, Olympus/OM ×2 |
| `dro` | DRange Optimizer | R/W | Sony |
| `effectmode` | Effect Mode | R/W | Nikon ×2 |
| `evstep` | EV Step | R/W | Nikon |
| `expmode` | Exp mode | R/W | Panasonic |
| `exposurecompensation` | Exposure Compensation | R/W | Canon, Olympus/OM, Panasonic, Sony ×8 |
| `exposurecompensation2` | Exposure Compensation | R/W | Nikon |
| `exposuredelaymode` | Exposure Delay Mode | R/W | Nikon |
| `exposurelock` | Exposure Lock | R/W | Nikon |
| `exposuremetermode` | Exposure Metering Mode | R/W | Olympus/OM ×2 |
| `expprogram` | Exposure Program | R/W | Olympus/OM, Sony ×3 |
| `expprogram2` | Exposure Program | R/W | Nikon |
| `f-number` | F-Number | R/W | Panasonic, Sony ×4 |
| `facedetection` | Face & Eye Detection | R/W | Nikon, Olympus/OM ×2 |
| `filenrsequencing` | File Number Sequencing | R/W | Nikon |
| `flashcommandacompensation` | Flash Command A Compensation | R/W | Nikon |
| `flashcommandamode` | Flash Command A Mode | R/W | Nikon |
| `flashcommandavalue` | Flash Command A Value | R/W | Nikon |
| `flashcommandbcompensation` | Flash Command B Compensation | R/W | Nikon |
| `flashcommandbmode` | Flash Command B Mode | R/W | Nikon |
| `flashcommandbvalue` | Flash Command B Value | R/W | Nikon |
| `flashcommandchannel` | Flash Command Channel | R/W | Nikon |
| `flashcommandermode` | Flash Commander Mode | R/W | Nikon |
| `flashcommanderpower` | Flash Commander Power | R/W | Nikon |
| `flashcommandselfcompensation` | Flash Command Self Compensation | R/W | Nikon |
| `flashcommandselfmode` | Flash Command Self Mode | R/W | Nikon |
| `flashcommandselfvalue` | Flash Command Self Value | R/W | Nikon |
| `flashcompensation` | Flash Compensation | R/W | Canon |
| `flashexposurecompensation` | Flash Exposure Compensation | R/W | Nikon, Olympus/OM ×2 |
| `flashmode` | Flash Mode | R/W | Canon, Olympus/OM ×3 |
| `flashmodemanualpower` | Flash Mode Manual Power | R/W | Nikon |
| `flashshutterspeed` | Flash Shutter Speed | R/W | Nikon |
| `flashsign` | Flash Sign | R/W | Nikon |
| `flashwhitebias` | Flash White Balance Bias | R/W | Nikon ×2 |
| `flexibleprogram` | Flexible Program | R/W | Nikon |
| `flourescentwhitebias` | Fluorescent White Balance Bias | R/W | Nikon ×2 |
| `focaldistancemeters` | Focal Distance Meters | R | Sony |
| `focallength` | Focal Length | R/W | Olympus/OM ×2 |
| `focalposition` | Focal Position | R | Sony |
| `focusarea` | Focus Area | R/W | Sony |
| `focusareawrap` | Focus Area Wrap | R/W | Nikon |
| `focusdistance` | Focus Distance | R/W | generic (any PTP) |
| `focusingpoint` | Focusing Point | R/W | Canon |
| `focusmetermode` | Focus Metering Mode | R/W | generic (any PTP) |
| `focusmode` | Focus Mode | R/W | Canon, Olympus/OM, Sony ×4 |
| `focusmode2` | Focus Mode 2 | R/W | Nikon |
| `hdmioutputdatadepth` | HDMI Output Data Depth | R/W | Nikon |
| `hdrhighdynamic` | HDR High Dynamic | R/W | Nikon |
| `hdrmode` | HDR Mode | R/W | Nikon, Olympus/OM ×2 |
| `hdrsmoothing` | HDR Smoothing | R/W | Nikon |
| `highisonr` | High ISO Noise Reduction | R/W | Canon |
| `highrescharge` | High Res Charge Time | R/W | Olympus/OM |
| `highresresolution` | High Res Resolution | R/W | Olympus/OM |
| `highresshot` | High Res Shot | R/W | Olympus/OM |
| `highreswait` | High Res Wait Time | R/W | Olympus/OM |
| `hueadjustment` | Hue Adjustment | R/W | Nikon |
| `imagequality` | Image Quality | R/W | Sony ×3 |
| `imagereview` | Image Review | R/W | Nikon, Olympus/OM ×2 |
| `imagerotationflag` | Image Rotation Flag | R/W | Nikon |
| `imagestabilization` | Image Stabilization | R/W | Olympus/OM, Sony ×2 |
| `jpegquality` | JPEG Quality | R/W | Sony |
| `liveviewaffocus` | Live View AF Focus | R/W | Nikon |
| `liveviewafmode` | Live View AF Mode | R/W | Nikon ×2 |
| `liveviewexposurepreview` | Live View Exposure Preview | R/W | Nikon |
| `liveviewimagezoomratio` | Live View Image Zoom Ratio | R/W | Nikon |
| `liveviewsettingeffect` | Live View Setting Effect | R/W | Sony |
| `liveviewsize` | Live View Size | R/W | Canon, Fuji, Panasonic, Sony ×4 |
| `liveviewwhitebalance` | Live View White Balance | R/W | Nikon |
| `liveviewzoom` | Live View Zoom Ratio | R/W | Olympus/OM |
| `longexpnr` | Long Exp Noise Reduction | R/W | Nikon ×2 |
| `lvcloseupmode` | LV Close Up Mode | R/W | Olympus/OM |
| `manualmoviesetting` | Manual Movie Setting | R/W | Nikon |
| `maximumshots` | Maximum Shots | R | Nikon |
| `meteringmode` | Metering Mode | R/W | Canon ×2 |
| `mfadjust` | MF Adjust | R/W | Panasonic |
| `microphone` | Microphone | R/W | Nikon |
| `minimumshutterspeed` | Auto Minimum Shutter Speed | R/W | Olympus/OM |
| `modelflash` | Modelling Flash | R/W | Nikon |
| `movieexposurecompensation` | Movie Exposure Compensation | R/W | Nikon |
| `movief-number` | Movie F-Number | R/W | Nikon |
| `movierecordingslot` | Movie Recording Slot | R/W | Olympus/OM |
| `movieservoaf` | Movie Servo AF | R/W | Canon |
| `movieshutterspeed` | Movie Shutter Speed 2 | R/W | Nikon |
| `moviesound` | Movie Sound | R/W | Nikon |
| `moviewhitebalance` | Movie White Balance | R/W | Nikon |
| `naturallightautowhitebias` | Natural light auto White Balance Bias | R/W | Nikon |
| `nikonflashmode` | Nikon Flash Mode | R/W | Nikon |
| `nocfcardrelease` | Release without CF card | R/W | Nikon |
| `noisefilter` | Noise Filter | R/W | Olympus/OM |
| `optimizeimage` | Optimize Image | R/W | Nikon |
| `pcsaveimgformat` | RAW+J PC Save Image | R/W | Sony |
| `pcsaveimgsize` | PC Save Image Size | R/W | Sony |
| `picturestyle` | Picture Style | R/W | Canon |
| `playslot` | Play Slot | R/W | Olympus/OM |
| `procaptureframelimit` | Pro Capture Frame Count Limit | R/W | Olympus/OM |
| `procaptureframelimiter` | Pro Capture Frame Count Limiter | R/W | Olympus/OM |
| `procapturemaxfps` | Pro Capture Max FPS | R/W | Olympus/OM |
| `procapturepreshutter` | Pro Capture Pre-Shutter Frames | R/W | Olympus/OM |
| `procapturesh1framelimit` | Pro Capture SH1 Frame Count Limit | R/W | Olympus/OM |
| `procapturesh1framelimiter` | Pro Capture SH1 Frame Count Limiter | R/W | Olympus/OM |
| `procapturesh1maxfps` | Pro Capture SH1 Max FPS | R/W | Olympus/OM |
| `procapturesh1preshutter` | Pro Capture SH1 Pre-Shutter Frames | R/W | Olympus/OM |
| `procapturesh2framelimit` | Pro Capture SH2 Frame Count Limit | R/W | Olympus/OM |
| `procapturesh2framelimiter` | Pro Capture SH2 Frame Count Limiter | R/W | Olympus/OM |
| `procapturesh2maxfps` | Pro Capture SH2 Max FPS | R/W | Olympus/OM |
| `procapturesh2preshutter` | Pro Capture SH2 Pre-Shutter Frames | R/W | Olympus/OM |
| `recording` | Start/Stop recording | R/W | Panasonic |
| `releaseprioritycaf` | Release Priority C-AF | R/W | Olympus/OM |
| `releaseprioritysaf` | Release Priority S-AF | R/W | Olympus/OM |
| `remotemode` | Remote Mode | R/W | Nikon |
| `remotetimeout` | Remote Timeout | R/W | Nikon |
| `reverseindicators` | Reverse Indicators | R/W | Nikon |
| `saturation` | Saturation | R/W | Nikon |
| `scenemode` | Scene Mode | R/W | Nikon |
| `selftimer` | Self Timer | R/W | Canon |
| `selftimerdelay` | Selftimer Delay | R/W | Nikon |
| `sensorcrop` | Sensor Crop | R/W | Sony |
| `sequentialframelimit` | Sequential Frame Count Limit | R/W | Olympus/OM |
| `sequentialframelimiter` | Sequential Frame Count Limiter | R/W | Olympus/OM |
| `sequentialmaxfps` | Sequential Max FPS | R/W | Olympus/OM |
| `sh1framelimit` | SH1 Frame Count Limit | R/W | Olympus/OM |
| `sh1framelimiter` | SH1 Frame Count Limiter | R/W | Olympus/OM |
| `sh1maxfps` | SH1 Max FPS | R/W | Olympus/OM |
| `sh2framelimit` | SH2 Frame Count Limit | R/W | Olympus/OM |
| `sh2framelimiter` | SH2 Frame Count Limiter | R/W | Olympus/OM |
| `sh2maxfps` | SH2 Max FPS | R/W | Olympus/OM |
| `shadewhitebias` | Shady White Balance Bias | R/W | Nikon ×2 |
| `sharpening` | Sharpening | R/W | Nikon |
| `sharpness` | Sharpness | R/W | generic (any PTP) |
| `shootingmode` | Canon Shooting Mode | R/W | Canon |
| `shutterlagtiming` | Shutter Lag Timing | R/W | Sony |
| `shutterspeed` | Shutter Speed | R/W | Canon, Fuji, Olympus/OM, Panasonic, Pentax, Sigma, Sony ×10 |
| `shutterspeed2` | Shutter Speed 2 | R/W | Nikon ×3 |
| `shuttertype` | Shutter Type | R/W | Sony |
| `silentmode` | Silent Mode | R/W | Sony |
| `silentsequentialframelimit` | Silent Sequential Frame Count Limit | R/W | Olympus/OM |
| `silentsequentialframelimiter` | Silent Sequential Frame Count Limiter | R/W | Olympus/OM |
| `silentsequentialmaxfps` | Silent Sequential Max FPS | R/W | Olympus/OM |
| `stillrecordingmode` | Still Recording Mode | R/W | Olympus/OM |
| `stillrecordingslot` | Still Recording Slot | R/W | Olympus/OM |
| `storageid` | Storage Device | R/W | Canon |
| `subjectdetection` | Subject Detection | R/W | Olympus/OM |
| `tonecompensation` | Tone Compensation | R/W | Nikon |
| `tungstenwhitebias` | Tungsten White Balance Bias | R/W | Nikon ×2 |
| `usermode` | User Mode | R/W | Nikon |
| `videomode` | Video Mode | R/W | Nikon |
| `viewfindergrid` | Viewfinder Grid | R/W | Nikon |
| `vignettecorrection` | Vignette Correction | R/W | Nikon |
| `wbbracketingpattern` | WB Bracketing Pattern | R/W | Nikon |
| `wbbracketingstep` | WB Bracketing Step | R/W | Nikon |
| `wbkeepwarmcolor` | WB Keep Warm Color | R/W | Olympus/OM |
| `wbpresetcomment1` | WB Preset Comment 1 | R/W | Nikon |
| `wbpresetcomment2` | WB Preset Comment 2 | R/W | Nikon |
| `wbpresetcomment3` | WB Preset Comment 3 | R/W | Nikon |
| `wbpresetcomment4` | WB Preset Comment 4 | R/W | Nikon |
| `wbpresetcomment5` | WB Preset Comment 5 | R/W | Nikon |
| `wbpresetcomment6` | WB Preset Comment 6 | R/W | Nikon |
| `whitebalance` | White Balance | R/W | Panasonic |
| `whitebalanceadjustab` | Adjust A/B | R/W | Panasonic |
| `whitebalanceadjustgm` | Adjust G/M | R/W | Panasonic |
| `whitebiaspreset0` | White Balance Bias Preset 0 | R | Nikon |
| `whitebiaspreset1` | White Balance Bias Preset 1 | R | Nikon |
| `whitebiaspreset2` | White Balance Bias Preset 2 | R | Nikon |
| `whitebiaspreset3` | White Balance Bias Preset 3 | R | Nikon |
| `whitebiaspreset4` | White Balance Bias Preset 4 | R | Nikon |
| `whitebiaspresetno` | White Balance Bias Preset Nr | R/W | Nikon |
| `zoom` | Zoom | R/W | Canon, Sony ×3 |
| `zoomspeed` | Zoom Speed | R/W | Canon |

---

## 10. Per-model Nikon override tables

Some Nikon bodies expose settings with different value tables or availability than the
generic path. `menus[]` swaps in a model-specific `submenu[]` (matched on USB
product id) *before* falling back to the generic table. These override/add on top of
the generic Nikon rows:

| Table | Model(s) (USB product id) | Extra/overridden settings |
|---|---|---|
| `nikon_d90_camera_settings` / `nikon_d90_capture_settings` | D90 (0x0421) | active-D-lighting, compression, ISO-auto hi-limit, shooting speed, hi-ISO NR, meter-off time |
| `nikon_d7000_camera_settings` | D7000 (0x0428) | (settings section) |
| `nikon_d7100_camera_settings` / `nikon_d7100_capture_settings` | D7100 (0x0430) | exposure-program modes, focus metering, movie quality, flash sync, shooting speed |
| `nikon_d5000_capture_settings` | D5000 (0x0423) | live-view zoom ratio |
| `nikon_d5100_capture_settings` | D5100 (0x0429) | exposure-program modes, movie quality |
| `nikon_d500_capture_settings` | D500 (0x043c) | |
| `nikon_d3s_capture_settings` | D3s (0x0426) | AF-C/S priority, dynamic AF area, AF-lock-on, flash sync, JPEG compression policy (16 settings) |
| `nikon_d40_capture_settings` | D40 (0x0414) | compression |
| `nikon_d850_capture_settings` | D850 (0x0441) | center-weight, focus metering, movie quality, shooting speed, compression |
| `nikon_d7500_capture_settings` | D7500 (0x0440) | compression |
| `nikon_d780_capture_settings` | D780 (0x0446) | |
| `nikon_z6_capture_settings` | Z-series (Z6/Z7/Z50/Z5/Z8/Z9/Zfc/Z30, 0x0442–0x0452) | P-A-D-P value table shared across the Z line |
| `nikon_1_j3` / `nikon_1_s1` (settings) | Nikon 1 J3 (0x0605) / S1 (0x0606) | 1-series ISO/compression/exposure-program modes |
| `nikon_generic_capture_settings` | all other Nikon | the default Nikon capture set (21 rows) |

## 11. WiFi profiles — `wifiprofiles`

Not a submenu table — a custom `get_menu_func`/`put_menu_func`
(`_get_wifi_profiles_menu` / `_put_wifi_profiles_menu`) backed by
`wifi_profiles_menu` and `create_wifi_profile_submenu` (10 fields: profile name, SSID,
IP config, encryption key, etc.). Lets you read and create Nikon WLAN transfer profiles
over USB.

---

## 12. Adding your own setting

The full recipe is in [04-extending.md §A.2](04-extending.md#a2-add-a-config-setting).
In short: add one `struct submenu` row to the right section array with the setting's
`name`, its `PTP_DPC_*` property code, data type, and get/put callbacks — reusing
`_get_INT`/`_put_INT`, `_get_STR`/`_put_STR`, or `_get_GenericU16Table`/`_put_GenericU16Table`
+ a new `deviceproptable*` choice table for an enumerated setting. No core changes are
needed; the widget appears automatically in `get_config`/`set_config`.

---

Back to [README.md](README.md) · [03-ptp2-driver.md](03-ptp2-driver.md)
