# 03 — The PTP/MTP Driver (`camlibs/ptp2/`)

This one driver is the entire modern-camera story. It is a single camlib (`ptp2.so`)
built from ~54,000 lines and registers **972 explicit camera models** plus generic
PTP/MTP class matches — so almost any Canon, Nikon, Sony, Fujifilm, OM/Olympus,
Panasonic, Sigma or Leica body is handled here. If you are expanding libgphoto2 for
mirrorless, **this is where 90% of the work lands.**

## File map

| File | Lines | Role |
|------|------:|------|
| `ptp.h` | 5452 | All PTP data structures, opcodes, property codes, the `PTPParams` state struct, and prototypes for every `ptp_*` operation. The single source of truth for the protocol. |
| `ptp.c` | 10220 | The **protocol layer**: 225 `ptp_*` functions implementing standard PTP ops + vendor extensions. Transport-agnostic — calls through function pointers in `PTPParams`. |
| `library.c` | 10100 | The **camlib implementation**: the three exported entry points, the `CameraFunctions` bodies, the filesystem callbacks, and the per-vendor capture logic. This is the bridge between the core's generic API and `ptp.c`. |
| `config.c` | 13559 | The **configuration tree**: a giant declarative table mapping camera settings ↔ `CameraWidget`s, with get/put callbacks and value-translation tables. |
| `ptp-pack.c` | 3068 | **Marshalling**: pack/unpack PTP structures to/from little-endian wire bytes (`ptp_pack_*`, `ptp_unpack_*`). |
| `usb.c` | 688 | PTP-over-USB transport (`ptp_usb_*`) — the functions wired into `PTPParams` for USB. |
| `ptpip.c` | 996 | PTP/IP (WiFi) transport (`ptp_ptpip_*`) — the command/event socket protocol. |
| `fujiptpip.c` | 1017 | Fuji's variant of PTP/IP (their WiFi tethering differs). |
| `chdk.c` | 1376 | CHDK (Canon Hack Development Kit) scripting support for compatible Canon PowerShots. |
| `olympus-wrap.c` | 1415 | Olympus's odd "PTP-wrapped-in-XML-over-USB-Mass-Storage" scheme. |
| `device-flags.h` | 344 | The `DEVICE_FLAG_*` quirk bitmask. |
| `music-players.h` | 4584 | MTP audio-player model table (libmtp-derived) — not cameras, but same protocol. |
| `ptp-private.h` | 188 | `CameraPrivateLibrary` (= `{ PTPParams; int checkevents; }`) and `PTPData`. |

---

## 1. `PTPParams` — the driver's brain (`ptp.h:4235`)

One `PTPParams` lives inside `camera->pl` for the whole session. It is what makes
`ptp.c` transport-agnostic and stateful. Key fields:

```c
struct _PTPParams {
    uint32_t device_flags;            // DEVICE_FLAG_* quirks for this exact model
    uint8_t  byteorder;               // PTP_DL_LE (always LE in practice)
    uint16_t maxpacketsize;           // USB endpoint packet size

    /* ── the transport vtable — set by library.c per port type ── */
    PTPIOSendReq   sendreq_func;      // send a PTP command container
    PTPIOSendData  senddata_func;     // send the data phase
    PTPIOGetResp   getresp_func;      // read the response container
    PTPIOGetData   getdata_func;      // read the data phase
    PTPIOGetResp   event_check, event_check_queue, event_wait;  // event channel
    PTPIOCancelReq cancelreq_func;
    PTPErrorFunc   error_func;  PTPDebugFunc debug_func;
    void          *data;              // → PTPData { Camera*; GPContext* }

    uint32_t transaction_id, session_id;   // PTP session bookkeeping

    /* ── caches (this is why browsing is fast) ── */
    PTPObjects        objects;        // handle → object-info cache
    PTPDeviceInfo     deviceinfo;     // supported ops/props/formats
    PTPStorageIDs     storageids;
    PTPDevicePropDescs dpd_cache;     // device-property descriptors (ISO, shutter, …)
    PTPEvents         events;         // pending event queue

    /* ── vendor-specific state ── */
    PTPDevicePropDescs canon_props;  int canon_viewfinder_on, canon_event_mode;
    PTPCanonEOSEvents  eos_events;   int eos_captureenabled, eos_uilocked, …;
    int  controlmode, event90c7works, cmd9207_1arg, deletesdramfails;  // Nikon
    int  sony_mode_ver;  struct timeval starttime;                     // Sony
    uint16_t olympus_camera_control_mode;                              // Olympus
    /* PTP/IP: */ int cmdfd, evtfd, jpgfd; uint8_t cameraguid[16]; char *cameraname;
    /* Olympus XML wrap: */ PTPDeviceInfo outer_deviceinfo; struct _PTPParams *outer_params;
    /* MTP/iconv, response-packet quirks, liveview flag, cachetime … */
};
```

The takeaway: **`ptp.c` never knows what bus it's on.** It calls
`params->sendreq_func(...)`, `params->getdata_func(...)`, etc. `library.c` plugs those
in — `ptp_usb_*` from `usb.c` for USB, `ptp_ptpip_*` from `ptpip.c` for WiFi. Swap the
vtable and the same protocol code runs over a different wire (this is also how you'd add
a new transport for a new platform).

---

## 2. `library.c` — the camlib implementation

### 2.1 The three exported entry points

**`camera_id(CameraText *id)`** — returns the string `"PTP"` (used by the core to
dedup this driver).

**`camera_abilities(CameraAbilitiesList *list)`** (`library.c:3113`) — iterates the
`models[]` table (972 entries) and appends a `CameraAbilities` for each, setting
capability flags from that model's `device_flags`:

- `PTP_CAP` → `GP_OPERATION_CAPTURE_IMAGE | GP_OPERATION_CONFIG`.
- `GP_OPERATION_TRIGGER_CAPTURE` is added for the tethering-capable lines: Nikon D/Z
  (vendor `0x4b0`), Canon EOS/Rebel (`0x4a9`), Panasonic (`0x04da`), Sony Alpha
  (`0x54c`), Olympus (`0x7b4`).
- `PTP_CAP_PREVIEW` → `GP_OPERATION_CAPTURE_PREVIEW` (liveview).
- File ops: PREVIEW + DELETE; folder ops: PUT_FILE + MAKE_DIR + REMOVE_DIR (PUT_FILE
  cleared for Nikon, which can't accept uploads).

It then also registers the `mtp_models[]` (audio players) and generic PTP/MTP class
matches, so **an unknown camera still works** as a generic PTP device.

The model tables:
- `models[]` (`library.c:856`) — 972 `{ model, vendor, product, device_flags }` rows.
- `mtp_models[]` (`:2944`) — MTP audio devices (from `music-players.h`).
- `ptpip_models[]` (`:2928`) — WiFi/PTP-IP-capable models.

**`camera_init(Camera*, GPContext*)`** (`library.c:9570`) — the connection setup:

1. Verify the port is USB / PTP-IP / USB-SCSI.
2. Install the `CameraFunctions` vtable (`about`, `exit`, `capture`, `trigger_capture`,
   `capture_preview`, `summary`, all the `*_config` variants, `wait_for_event`).
3. Allocate `CameraPrivateLibrary` and its `PTPParams`; set `debug_func`/`error_func`;
   allocate the `PTPData` back-pointer to `camera`.
4. Look up `device_flags` from `models[]`/`mtp_models[]` by VID/PID.
5. **Wire the transport vtable** by port type:
   - `GP_PORT_USB` → `ptp_usb_sendreq/senddata/getresp/getdata` + `ptp_usb_event_*`
     (+ Olympus XML wrap setup if flagged).
   - `GP_PORT_PTPIP` → the ptpip connect + `ptp_ptpip_*` funcs.
6. Set up iconv (UCS-2 ↔ locale) for MTP string fields.
7. Open a PTP session (`ptp_opensession`), read `ptp_getdeviceinfo`, prime caches, run
   any vendor init handshake (Canon EOS "set remote mode", Nikon control-mode, Sony
   auth, Olympus PC-mode), and register the filesystem callbacks (§2.3).

### 2.2 `CameraFunctions` bodies & the capture dispatch

The generic vtable entries are `camera_exit`, `camera_about`, `camera_summary`,
`camera_get_config`/`camera_set_config`/`camera_list_config`/single variants,
`camera_capture_preview`, `camera_capture`, `camera_trigger_capture`,
`camera_wait_for_event`.

The interesting part is that **capture fans out by vendor**. `camera_capture`
(`library.c:5779`) inspects the connected model and dispatches to the right
implementation:

| Dispatcher | Camera family | Notes |
|------------|---------------|-------|
| `camera_nikon_capture` | Nikon D/Z | `:4191` |
| `camera_canon_eos_capture` / `camera_trigger_canon_eos_capture` | Canon EOS/Rebel | `:4490` / `:6054` — the big one; EOS has its own event/property model. |
| `camera_canon_capture` | older Canon PowerShot (non-EOS) | `:4744` |
| `camera_sony_capture` / `camera_sony_qx_capture` | Sony Alpha / QX | `:4940` / `:5161` |
| `camera_fuji_capture` | Fujifilm | `:5323` |
| `camera_panasonic_capture` | Panasonic | `:5535` |
| `camera_olympus_omd_capture` / `camera_olympus_xml_capture` | OM/Olympus | `:5632` / `:4656` |
| `camera_sigma_fp_capture` | Sigma fp | `:5680` |

`camera_wait_for_event` (`:6654`) similarly merges the vendor event queues (Canon EOS
events, Nikon events, generic PTP events) into the core's `CameraEventType` stream
(`GP_EVENT_FILE_ADDED`, `_CAPTURE_COMPLETE`, …). This is the function your tethering
loop actually drives.

### 2.3 The filesystem callbacks (registered on `camera->fs`)

These implement the `CameraFilesystemFuncs` contract (doc 01 §3) in terms of PTP object
handles:

| Callback | `library.c` | Maps to |
|----------|-------------|---------|
| `file_list_func` | `:8239` | `ptp_getobjecthandles` in a folder’s storage/parent. |
| `folder_list_func` | `:8261` | Same, filtered to association (folder) objects. |
| `get_file_func` | `:8794` | `ptp_getobject` / `ptp_getpartialobject` / `ptp_getthumb` depending on `CameraFileType`. |
| `put_file_func` | `:9080` | `ptp_sendobjectinfo` + `ptp_sendobject`. |
| `delete_file_func` | `:9183` | `ptp_deleteobject`. |
| `get_info_func` | `:9307` | `ptp_getobjectinfo` → size/dims/mtime/permissions. |
| `make_dir_func` / `remove_dir_func` | `:9396` / `:9241` | Association objects. |
| `storage_info_func` | `:9446` | `ptp_getstorageids` + `ptp_getstorageinfo`. |

`folder_to_handle` (`:8131`) translates a gphoto path (`/store_00010001/DCIM/100CANON`)
to the PTP `(storage, parent-handle)` pair.

---

## 3. `ptp.c` — the protocol layer (225 functions)

Every function marshals a PTP transaction: build a command container, send it through
`params->sendreq_func`, optionally send/receive a data phase, read the response. The
plumbing is centralized in **`ptp_transaction()`** (`:468`) and
**`ptp_transaction_new()`** (`:226`); the data-phase handlers
(`ptp_init_recv_memory_handler`, `ptp_init_send_memory_handler`, `ptp_init_fd_handler`)
let a transaction stream to memory or straight to an fd.

### 3.1 Standard PTP operations (ISO 15740)

These are vendor-neutral and the backbone of browsing/downloading:

| Function | PTP op | Purpose |
|----------|--------|---------|
| `ptp_opensession` / `ptp_closesession` | OpenSession | Start/stop a session. |
| `ptp_getdeviceinfo` | GetDeviceInfo | Supported ops/props/formats — cached in `params->deviceinfo`. |
| `ptp_getstorageids` / `ptp_getstorageinfo` | GetStorageIDs/Info | Enumerate cards/volumes. |
| `ptp_getobjecthandles` | GetObjectHandles | List object ids in a storage/folder. |
| `ptp_getnumobjects` | GetNumObjects | Count. |
| `ptp_getobjectinfo` | GetObjectInfo | Metadata for one handle. |
| `ptp_getobject` | GetObject | Download a whole object. |
| `ptp_getpartialobject` | GetPartialObject | Ranged download (big files / resume). |
| `ptp_getthumb` | GetThumb | Embedded thumbnail. |
| `ptp_deleteobject` | DeleteObject | Delete. |
| `ptp_sendobjectinfo` / `ptp_sendobject` | SendObjectInfo/SendObject | Upload. |
| `ptp_initiatecapture` | InitiateCapture | Generic "take a picture". |
| `ptp_getdevicepropdesc` | GetDevicePropDesc | Describe a setting (type, range, choices). |
| `ptp_getdevicepropvalue` / `ptp_setdevicepropvalue` | Get/SetDevicePropValue | Read/write a setting. |

### 3.2 Vendor extensions (the bulk of the 225)

PTP is extensible, and each manufacturer bolted on proprietary opcodes for
capture, liveview and rich control. They follow a strict `ptp_<vendor>_<action>`
naming convention, so you can find anything by grep:

| Prefix | Count | Examples |
|--------|------:|----------|
| `ptp_canon_*` (PowerShot) | 30 | `ptp_canon_initiatecaptureinmemory`, `ptp_canon_getviewfinderimage`, `ptp_canon_getchanges` |
| `ptp_canon_eos_*` (EOS) | 17 | `ptp_canon_eos_capture`, `ptp_canon_eos_getviewfinderdata`, `ptp_canon_eos_setdevicepropvalueex`, `ptp_canon_eos_getevent` |
| `ptp_nikon_*` | 14 | `ptp_nikon_capture`, `ptp_nikon_getliveviewimg`, `ptp_nikon_device_ready`, `ptp_nikon_getvendorpropcodes` |
| `ptp_sony_*` | 13 | `ptp_sony_sdioconnect`, `ptp_sony_getalldevicepropdesc`, `ptp_sony_setdevicecontrolvaluea` |
| `ptp_sigma_*` | 19 | `ptp_sigma_fp_snap`, `ptp_sigma_fp_liveview_image`, `ptp_sigma_fp_getcapturestatus` |
| `ptp_panasonic_*` | 16 | `ptp_panasonic_liveview_image`, `ptp_panasonic_setdeviceproperty`, `ptp_panasonic_9414_*` |
| `ptp_olympus_*` | 11 | `ptp_olympus_omd_capture`, `ptp_olympus_liveview_image`, `ptp_olympus_init_pc_mode` |
| `ptp_mtp_*` | 12 | `ptp_mtp_getobjectpropslist`, `ptp_mtp_getobjectpropvalue` (metadata for MTP devices) |
| `ptp_fuji_*` | 2 | Fuji-specific (most Fuji logic is in `fujiptpip.c` + config). |
| `ptp_leica_*` | 1 | `ptp_leica_getstreamdata`. |

> **How to read this file when adding a camera:** find the standard op you need
> (§3.1); if the camera needs a proprietary handshake, find its `ptp_<vendor>_*`
> neighbours. The opcodes/property codes themselves are enumerated in `ptp.h`
> (`PTP_OC_*`, `PTP_DPC_*`, `PTP_OFC_*`, plus vendor `PTP_OC_CANON_*`, `PTP_OC_NIKON_*`
> …). Adding an op = new `PTP_OC_*` constant in `ptp.h` + new `ptp_<vendor>_<x>`
> function in `ptp.c` that marshals it.

### 3.3 Liveview and capture

Liveview is just "call the vendor's viewfinder op in a loop and hand each JPEG frame to
`camera_capture_preview`": `ptp_canon_eos_getviewfinderdata`,
`ptp_nikon_getliveviewimg`, `ptp_panasonic_liveview_image`,
`ptp_sigma_fp_liveview_image`, `ptp_olympus_liveview_image`. Capture is the vendor
`camera_*_capture` dispatch in `library.c` (§2.2), which drives the matching `ptp_*`
ops and then waits for the "object added" event to learn the new file's handle.

---

## 4. Transports — `usb.c`, `ptpip.c`, `fujiptpip.c`

These provide the functions that get plugged into the `PTPParams` vtable.

### `usb.c` (PTP-over-USB)
`ptp_usb_sendreq` (command container as a USB bulk-OUT), `ptp_usb_senddata` /
`ptp_usb_getdata` (data phase, with `ptp_usb_getpacket` handling the bulk container
framing), `ptp_usb_getresp` (response), and the event side
`ptp_usb_event`/`_event_check`/`_event_check_queue`/`_event_wait` (interrupt endpoint).
Plus control helpers `ptp_usb_control_cancel_request`,
`ptp_usb_control_device_reset_request`, `ptp_usb_control_get_device_status`. Everything
here ultimately calls the port layer's `gp_port_write`/`gp_port_read`/`gp_port_check_int`
(doc 02) — so **USB I/O bottoms out in the libusb1 iolib.**

### `ptpip.c` (PTP/IP over WiFi)
Implements PTP/IP's two-socket protocol: a command/data channel and an event channel.
`ptp_ptpip_connect` performs the init handshake (`ptp_ptpip_init_command_request` →
`_ack`, `ptp_ptpip_init_event_request` → `_ack`, exchanging GUIDs/names).
`ptp_ptpip_sendreq`/`senddata`/`getdata`/`getresp` mirror the USB ones but over TCP with
PTP/IP packet headers (`ptp_ptpip_generic_read`, `ptp_ptpip_cmd_read`,
`ptp_ptpip_evt_read`). `ptp_ptpip_event_*` service the event socket. **This is the path
for platforms without USB-host access (notably iOS) — connect to the camera's WiFi and
speak PTP/IP.**

### `fujiptpip.c`
Fuji's WiFi tethering deviates from stock PTP/IP (different ports/framing/handshake), so
it gets its own ~1000-line implementation of the same idea.

---

## 5. `ptp-pack.c` — marshalling

Pure serialization helpers, no I/O. ~15 `ptp_pack_*` (struct → LE bytes, e.g.
`ptp_pack_OI` for ObjectInfo, `ptp_pack_DPV` for device-property values) and ~80
`ptp_unpack_*` (bytes → struct, e.g. `ptp_unpack_DeviceInfo`, `ptp_unpack_OI`,
`ptp_unpack_EOS_events`, `ptp_unpack_Nikon_EC`). Also the UCS-2/UTF string helpers used
for MTP fields. When a new camera returns a slightly different binary layout, this is
often where the fix goes (a new/patched `ptp_unpack_*`).

---

## 6. `config.c` — the settings tree (13,559 lines)

This file *is* the camera-configuration UI, declaratively. It builds the `CameraWidget`
tree returned by `camera_get_config` and applies edits in `camera_set_config`.

### Structure

- A **`struct submenu`** describes one setting: its label, name, PTP property code
  (`PTP_DPC_*`), widget type, and a **get callback** (`CONFIG_GET_ARGS` — build the
  widget from the current device-property value) and **put callback** (`CONFIG_PUT_ARGS`
  — translate the widget's new value back to a PTP property write).
- A **`struct menu`** groups submenus into a section (a tab), sometimes with its own
  get/put for whole-section handling.
- The top level is an array of menus; `camera_get_config` walks it, calling each get
  callback; `camera_set_config` walks it applying changed widgets.

### Value-translation tables

Raw PTP values are integers; users want words. Generic helpers
`_get_GenericU16Table` / `_put_GenericU16Table` (and 8/32-bit variants, generated by the
`struct deviceproptable##bits` macros at `config.c:713`) map between a numeric property
value and a human string using per-setting tables such as:

```c
static struct deviceproptableu16 whitebalance[] = {
    { N_("Automatic"), 0x0002, 0 }, { N_("Daylight"), 0x0004, 0 }, …
};
```

There are 219 of these tables (white balance, image format, drive mode, AF area,
ISO, metering, picture styles, per-vendor variants like `olympus_whitebalance`,
`fuji_imageformat`, …), which is a big part of why this is the largest single file in
the driver.

> **Full setting list.** Every one of the **359 setting names** (502 vendor-specific
> declarations) across all sections is enumerated, with access mode and vendor coverage,
> in **[05-config-reference.md](05-config-reference.md)** — the complete, generated
> config reference.

> **To add a setting** you write one `struct submenu` row (name, PTP property code,
> widget type, get/put callbacks) and, if it's an enumerated choice, one
> `deviceproptable*` value table. That's the whole recipe — see
> [04-extending.md](04-extending.md#a2-add-a-config-setting).

---

## 7. Special modes — `chdk.c`, `olympus-wrap.c`

- **`chdk.c`** — CHDK is third-party firmware for many Canon PowerShots that exposes a
  Lua scripting + raw-memory interface over PTP. This file implements those custom ops
  (upload/download files, execute Lua, live view). Niche; only relevant if you target
  CHDK'd Canons.
- **`olympus-wrap.c`** — some Olympus bodies present as USB Mass Storage but tunnel PTP
  commands inside XML blobs written to magic files. `olympus_setup` installs an *outer*
  `PTPParams` whose transport wraps/unwraps that XML, so the rest of `ptp.c` sees a
  normal PTP device. `outer_params`/`outer_deviceinfo`/`olympus_cmd`/`olympus_reply` in
  `PTPParams` support this. Requires libxml2.

---

## 8. Putting it together — a Nikon Z tethered shot

```
gp_camera_trigger_capture()                       [core]
 → camera->functions->trigger_capture
    → camera_trigger_capture() → Nikon branch     [library.c]
       → ptp_nikon_capture() / InitiateCapture    [ptp.c]
          → params->sendreq_func = ptp_usb_sendreq [usb.c]
             → gp_port_write() → libusb1 → libusb  [port + iolib]
... shutter fires ...
loop: gp_camera_wait_for_event(&type,&data)       [core]
 → camera_wait_for_event() merges Nikon events    [library.c]
    → ptp_nikon_check_event / ptp_usb_event_check  [ptp.c / usb.c]
 → returns GP_EVENT_FILE_ADDED, data = CameraFilePath* of the new NEF
gp_camera_file_get(folder,file,GP_FILE_TYPE_NORMAL)[core]
 → get_file_func → ptp_getobject → streams the NEF back
```

Every layer boundary is a `->func()` pointer or a `gp_*` wrapper — which is exactly why
you can extend the stack (new camera, new setting, new transport, new platform) without
rewriting the layers below.

---

Continue to [04-extending.md](04-extending.md).
