# 00 — Architecture Overview

This is the conceptual map. Read it before the per-file references so the function
tables in later docs have a place to hang.

---

## 1. The object model

Everything in the public API is organized around a handful of opaque, reference-counted
structs. You get them from a `gp_*_new()` and use accessor functions; you never poke at
their fields (except drivers, which are allowed at `->port`, `->pl`, `->fs`).

| Object | Header | Purpose |
|--------|--------|---------|
| `Camera` | `gphoto2-camera.h` | The central handle. Owns a `GPPort`, a `CameraFilesystem`, a `CameraFunctions` vtable (filled by the driver), and two private blobs (`pl` = driver-private, `pc` = core-private). |
| `GPContext` | `gphoto2-context.h` | Bundle of frontend callbacks: progress, error/status/message text, question (yes/no), cancel, idle. Threaded through nearly every call. |
| `CameraAbilities` / `CameraAbilitiesList` | `gphoto2-abilities-list.h` | The database of *which cameras are supported* and *what each can do*. Built by asking every camlib for its model list. |
| `CameraFilesystem` | `gphoto2-filesys.h` | A **caching, virtual filesystem** over the camera's storage. Frontends see folders/files; the driver registers callbacks that fetch the real data on demand. |
| `CameraFile` | `gphoto2-file.h` | An in-memory (or fd-backed) blob with a name, MIME type and mtime. The unit of image/preview/metadata transfer. |
| `CameraWidget` | `gphoto2-widget.h` | A node in the **configuration tree** (window → sections → text/toggle/range/radio/date widgets). This is how *all* camera settings — ISO, shutter, focus, white balance, capture target — are exposed generically. |
| `CameraList` | `gphoto2-list.h` | A simple ordered list of (name, value) string pairs. Used for file lists, folder lists, detected cameras, config names. |
| `GPPort` / `GPPortInfo` / `GPPortInfoList` | `gphoto2-port*.h` | The transport handle and the enumeration of available ports. |

### The `Camera` struct is the hub

```c
struct _Camera {
    GPPort           *port;       // transport (USB/PTPIP/serial/…) — the driver reads/writes here
    CameraFilesystem *fs;         // caching VFS — the driver registers fetch callbacks here
    CameraFunctions  *functions;  // vtable of camera operations, filled in by camera_init()
    CameraPrivateLibrary  *pl;    // driver-private state (for ptp2: wraps a PTPParams)
    CameraPrivateCore     *pc;    // core-private: abilities, ltdl handle, refcount, timeouts
};
```

`CameraFunctions` is the driver's contract with the core — a struct of function
pointers the driver sets during `camera_init()`:

```
pre_func / post_func   – run around every operation (open/close, speed changes)
exit                   – tear down the connection
get_config/set_config  – the whole config tree  (+ get/set/list "single" variants)
capture                – take a picture, store on card, return its path
trigger_capture        – fire shutter, return immediately (pair with wait_for_event)
capture_preview        – grab a liveview/viewfinder frame into a CameraFile
summary/manual/about   – human-readable text
wait_for_event         – block for the next camera event (file added, capture complete…)
```

Filesystem operations (list/get/put/delete files, make/remove dirs, storage info) are
**not** in this vtable — they are registered separately on `camera->fs` via
`gp_filesystem_set_funcs()`. See §4.

---

## 2. The two plugin systems (this is the key idea)

libgphoto2 links against **nothing** camera- or bus-specific at build time. At runtime
it uses libtool's `ltdl` to `dlopen()` shared objects from two directories:

### (a) Camera drivers — "camlibs"

Installed to `${libdir}/libgphoto2/<version>/`. The search path is overridable with the
`CAMLIBS` environment variable. Each `.so` **must** export three C functions
(`gphoto2-library.h`):

```c
int camera_id        (CameraText *id);                 // unique driver id string, for dedup
int camera_abilities (CameraAbilitiesList *list);      // append every model this driver supports
int camera_init      (Camera *camera, GPContext *ctx); // wire up camera->functions + camera->fs
```

`gp_abilities_list_load()` walks the camlib directory, dlopens each file, calls
`camera_id` (to skip duplicates) then `camera_abilities` (to harvest the model list),
stamping each entry with the originating `.so` filename. **For mirrorless the only
driver that matters is `ptp2`, whose `camera_abilities` registers 972 models plus
generic PTP/MTP class matches.**

### (b) Transport drivers — "iolibs"

Installed to `${libdir}/libgphoto2_port/<version>/`. Search path overridable with
`IOLIBS`. Each `.so` exports (`gphoto2-port-library.h`):

```c
GPPortType         gp_port_library_type       (void);            // GP_PORT_USB, GP_PORT_PTPIP, …
int                gp_port_library_list       (GPPortInfoList*); // enumerate present ports
GPPortOperations  *gp_port_library_operations (void);            // the read/write/open/… vtable
```

`GPPortOperations` is the transport vtable (open, close, read, write, check_int, plus
USB control-message and serial-pin methods). The core's `gp_port_*` functions are thin
wrappers that dispatch to whichever iolib is loaded for the port.

> **Why this matters for you:** adding a new bus (say, a custom Android USB shim or an
> iOS transport) is fundamentally "write a new iolib that fills in
> `GPPortOperations`." You do **not** touch the drivers or the core. See
> [04-extending.md](04-extending.md).

---

## 3. Control flow: four end-to-end walkthroughs

### 3a. Autodetect — "what cameras are plugged in?"

`gp_camera_autodetect(list, ctx)` (in `gphoto2-camera.c:603`):

1. `gp_port_info_list_new/load` — load all iolibs, enumerate every present port
   (each USB device becomes a `usb:BUS,DEV` port entry).
2. `gp_abilities_list_new/load` — load all camlibs, build the supported-model DB.
3. `gp_abilities_list_detect(al, il, xlist, ctx)` — for every known model with a USB
   vendor/product id, ask the USB iolib `gp_port_usb_find_device()` whether such a
   device is present; matches are appended as `(model, port)` pairs.
4. The generic bare `usb:` entry is filtered out; the rest is returned to the caller.

### 3b. Init — "connect to this camera"

`gp_camera_init(camera, ctx)` (`gphoto2-camera.c:668`):

1. If no model/port was set, run the autodetect above and pick the first (or the one
   matching the pre-set port).
2. For USB, call `gp_port_usb_find_device[_by_class]()` to bind the `GPPort` to the
   physical device.
3. `lt_dlopenext(camera->pc->a.library)` — dlopen the camlib named in the abilities.
4. `lt_dlsym("camera_init")` and call it. The driver fills `camera->functions` and
   registers filesystem callbacks, opens a PTP session, reads device info, etc.

After this, the camera is "used"; the core's `CHECK_INIT` macro auto-inits on first
operation if you skipped the explicit call.

### 3c. Download a file

`gp_camera_file_get(camera, folder, file, GP_FILE_TYPE_NORMAL, cfile, ctx)`:

1. Core wrapper opens the port and calls the driver `pre_func`.
2. Delegates to `gp_filesystem_get_file()` on `camera->fs`.
3. The filesystem checks its **cache/LRU**; on a miss it invokes the driver's
   registered `get_file_func`, which (in ptp2) issues `PTP GetObject` and streams the
   bytes into the `CameraFile`.
4. `post_func` runs, port closes, data is now in `cfile` (retrieve with
   `gp_file_get_data_and_size`).

`GP_FILE_TYPE_*` selects which *view* you want: `NORMAL` (the JPEG/RAW), `PREVIEW`
(embedded thumbnail), `EXIF`, `METADATA`, `AUDIO`, `RAW`.

### 3d. Tethered capture (the mirrorless money path)

Two styles, both dispatching through `CameraFunctions`:

- **Synchronous:** `gp_camera_capture(camera, GP_CAPTURE_IMAGE, &path, ctx)` — fires the
  shutter, waits for the image to be written, returns its `CameraFilePath`. Download
  with `gp_camera_file_get`.
- **Asynchronous (preferred for bursts / liveview):**
  `gp_camera_trigger_capture()` returns immediately, then you loop on
  `gp_camera_wait_for_event(camera, timeout, &type, &data, ctx)` until you see
  `GP_EVENT_FILE_ADDED` (data = `CameraFilePath*`) or `GP_EVENT_CAPTURE_COMPLETE`.

Liveview frames come from `gp_camera_capture_preview()`, which in ptp2 maps to
vendor-specific viewfinder ops (Canon EOS `GetViewFinderData`, Nikon `GetLiveViewImg`,
Sony/Panasonic/Olympus equivalents). See
[03-ptp2-driver.md](03-ptp2-driver.md#liveview-and-capture).

---

## 4. Data flow: the caching filesystem

`CameraFilesystem` (`gphoto2-filesys.c`, ~2500 lines — the second largest core file) is
more than a listing. It is a **write-through cache** so frontends can browse without
hammering the camera:

- Folder/file **tree** is cached after the first listing.
- File **content** and **info** (size, dimensions, mtime, permissions) are cached, with
  an **LRU eviction** policy (`gp_filesystem_lru_*`) capped by total bytes, so pulling a
  thumbnail grid doesn't blow up memory.
- EXIF mtime can be extracted to fill in timestamps the camera didn't report
  (`get_exif_mtime`, needs libexif).
- The driver supplies the real behavior by registering a `CameraFilesystemFuncs` struct
  (12 callbacks: file/folder listing, get/put/delete file, get/set info, make/remove
  dir, delete-all, storage-info) via `gp_filesystem_set_funcs()`.

So the driver implements *fetching*; the core implements *caching, path handling, and
the public API*. The `_noop` functions (`gp_filesystem_set_info_noop`,
`gp_filesystem_set_file_noop`, `gp_filesystem_delete_file_noop`) let a driver update the
cache after it already changed the camera, without a redundant round-trip.

---

## 5. The configuration tree

Camera settings are **not** a fixed struct — they're a dynamically built tree of
`CameraWidget` nodes, so a driver can expose whatever knobs a given body has:

```
GP_WIDGET_WINDOW  (root)
 ├─ GP_WIDGET_SECTION  "Capture Settings"
 │   ├─ GP_WIDGET_RADIO   "iso"        value: "100" (choices: 100,200,…)
 │   ├─ GP_WIDGET_RADIO   "shutterspeed"
 │   └─ GP_WIDGET_TOGGLE  "autofocus"  value: 0/1
 └─ GP_WIDGET_SECTION  "Other"
     └─ GP_WIDGET_DATE    "datetime"
```

Frontend flow: `gp_camera_get_config()` → render the tree → user edits a widget
(`gp_widget_set_value`, mark `gp_widget_set_changed`) → `gp_camera_set_config()` hands
the tree back to the driver, which writes changed widgets to the camera. There are also
`*_single_config` fast paths to get/set one named widget without building the whole
tree. In ptp2 the entire tree is defined by a giant table in `config.c` (see doc 03).

---

## 6. Errors, logging, threading, memory

- **Errors** propagate as negative `int` codes. Human-facing detail is *also* pushed to
  the frontend via `gp_context_error(ctx, ...)`. Port-level codes come from
  `gphoto2-port-result.h`; higher-level ones from `gphoto2-result.h`.
- **Logging** is `gp_log()/GP_LOG_D/E/…` at levels ERROR/VERBOSE/DEBUG/DATA. Frontends
  register a sink with `gp_log_add_func()`. `GP_LOG_DATA` dumps raw wire bytes — the
  first thing to enable when debugging a new camera.
- **Threading.** libgphoto2 is **not internally thread-safe per `Camera`**: serialize
  all calls for a given camera on one thread (or lock around them). The one global lock
  present guards `ltdl` (`gphoto2-port-locking.h`, `gpi_libltdl_lock`), because libtool's
  loader isn't reentrant. Multiple *different* cameras on separate threads are fine.
- **Memory.** Objects are refcounted; balance `ref`/`unref`. Functions that hand back an
  allocated array (e.g. `gp_camera_get_storageinfo`) document that the caller frees it.
  `CameraFile` data can be owned-and-freed by the file or borrowed — see the file API.

---

## 7. Where the bytes actually go

For a Canon EOS R over USB, a single "set ISO to 400" is:

```
gp_camera_set_config()                       [core, gphoto2-camera.c]
  → camera->functions->set_config()          [ptp2, library.c: camera_set_config]
     → walks CameraWidget tree, finds changed "iso"
     → put-callback in config.c maps "400" → PTP property value
     → ptp_setdevicepropvalue() / ptp_canon_eos_setdevicepropvalueex()   [ptp2, ptp.c]
        → params->senddata_func == ptp_usb_senddata()                    [ptp2, usb.c]
           → gp_port_write()                                             [port core, gphoto2-port.c]
              → port->pc->ops->write == gp_libusb1_write()               [iolib, libusb1.c]
                 → libusb_bulk_transfer()                                [libusb, the actual USB I/O]
```

Every arrow is a layer boundary you can extend or intercept. The bottom two are where
a mobile/alternate-platform port plugs in.

Continue to [01-core-api.md](01-core-api.md).
