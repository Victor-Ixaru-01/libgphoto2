# 02 — Port / Transport Layer (`libgphoto2_port/`)

This is a **separate library** (`libgphoto2_port.so`, ~12,400 lines) with its own
headers, versioning and plugin directory. It abstracts "move bytes to/from the device"
so drivers never care whether they're on USB, WiFi, serial or a mounted disk. **This is
the layer you extend to reach a camera from a new platform** (Android, iOS, Windows) —
see [04-extending.md](04-extending.md).

Files:

| File | Lines | Role |
|------|------:|------|
| `libgphoto2_port/gphoto2-port.c` | 1251 | The `gp_port_*` public API; dispatches to the loaded iolib's `GPPortOperations`. |
| `libgphoto2_port/gphoto2-port-info-list.c` | 639 | Enumerate iolibs & present ports; `GPPortInfo` accessors. |
| `libgphoto2_port/gphoto2-port-log.c` | 539 | Logging (`gp_log*`) and the enum/flag ↔ string helpers. |
| `libgphoto2_port/gphoto2-port-portability.c` | 286 | OS glue (dir scanning, `gp_system_*`). |
| `libgphoto2_port/gphoto2-port-version.c` | 138 | Port-lib version/feature strings. |
| `libgphoto2_port/gphoto2-port-result.c` | 88 | `gp_port_result_as_string()`. |
| `libgphoto2_port/gphoto2-port-locking.c` | 42 | The global `ltdl` mutex. |
| **iolibs** (`usb/`,`libusb1/`,`serial/`,`ptpip/`,`disk/`,`usbdiskdirect/`,`usbscsi/`,`vusb/`) | ~7000 | The pluggable backends. |

---

## 1. The transport model

A `GPPort` has a `type` (`GPPortType` bitmask: `GP_PORT_SERIAL`, `GP_PORT_USB`,
`GP_PORT_DISK`, `GP_PORT_PTPIP`, `GP_PORT_USB_DISK_DIRECT`, `GP_PORT_USB_SCSI`,
`GP_PORT_IP`), a `settings` union, a `timeout`, and two private blobs (`pl` =
driver-private, `pc` = port-core-private, which holds the loaded iolib handle and its
`GPPortOperations` vtable).

Each `gp_port_*` call is a thin wrapper:

```c
int gp_port_read(GPPort *port, char *data, int size) {
    CHECK_INIT(port);                       // ensure an iolib is loaded
    CHECK_SUPP(port, "read", port->pc->ops->read);   // does this backend implement it?
    return port->pc->ops->read(port, data, size);    // dispatch to the iolib
}
```

So the *interface* is fixed (`gp_port_read/write/…`) and the *implementation* is
whichever iolib matches the port type.

### The iolib plugin contract (`gphoto2-port-library.h`)

Every backend `.so` exports:

```c
GPPortType         gp_port_library_type       (void);            // which bus it handles
int                gp_port_library_list       (GPPortInfoList*); // add present ports
GPPortOperations  *gp_port_library_operations (void);            // the vtable below
```

`GPPortOperations` (the methods a backend may implement; unused ones are `NULL` and the
core returns `GP_ERROR_NOT_SUPPORTED`):

```
init, exit, open, close, reset, update
read, write, check_int                     // core byte I/O + interrupt endpoint
get_pin, set_pin, send_break, flush        // serial only
find_device, find_device_by_class          // USB device matching
clear_halt, msg_write, msg_read,           // USB control transfers
  msg_interface_{write,read}, msg_class_{write,read}
seek                                        // usbdiskdirect
send_scsi_cmd                               // usbscsi
```

---

## 2. Public API — `gphoto2-port.c`

### Lifecycle & config

| Function | Signature | Notes |
|----------|-----------|-------|
| `gp_port_new` | `(GPPort **port)` | Allocate. |
| `gp_port_free` | `(GPPort*)` | Close, exit, dlclose iolib, free. |
| `gp_port_set_info` | `(GPPort*, GPPortInfo)` | Bind to a port entry → loads the matching iolib (`init`). |
| `gp_port_get_info` | `(GPPort*, GPPortInfo*)` | |
| `gp_port_open` / `gp_port_close` | `(GPPort*)` | Open/close the transport. |
| `gp_port_reset` | `(GPPort*)` | Bus reset — device may re-enumerate under a new id. |
| `gp_port_set_settings` / `gp_port_get_settings` | `(GPPort*, GPPortSettings[*])` | Commit/read the settings union (serial baud/bits/parity, or USB endpoints/interface/config). Pending settings are applied via the backend `update`. |
| `gp_port_set_timeout` / `gp_port_get_timeout` | `(GPPort*, int ms[*])` | Read/write timeout in ms. |

### Byte I/O

| Function | Signature | Returns | Notes |
|----------|-----------|---------|-------|
| `gp_port_write` | `(GPPort*, const char *data, int size)` | bytes written or code | On non-serial, returns the count. |
| `gp_port_read` | `(GPPort*, char *data, int size)` | bytes read or code | Reads up to `size`. |
| `gp_port_check_int` | `(GPPort*, char *data, int size)` | bytes or code | Read the **interrupt** endpoint (event channel), default timeout. |
| `gp_port_check_int_fast` | `(GPPort*, char *data, int size)` | bytes or code | Same, very short timeout — poll without blocking. |

### Serial-only

`gp_port_get_pin(port, GPPin, GPLevel*)`, `gp_port_set_pin(port, GPPin, GPLevel)`
(pins: RTS/DTR/CTS/DSR/CD/RING), `gp_port_send_break(port, ms)`,
`gp_port_flush(port, direction)`.

### USB control & specials

| Function | Signature | Notes |
|----------|-----------|-------|
| `gp_port_usb_find_device` | `(GPPort*, int idvendor, int idproduct)` | Bind to a USB device by VID/PID. The autodetect probe. |
| `gp_port_usb_find_device_by_class` | `(GPPort*, int class, int subclass, int protocol)` | Bind by USB class (how *unknown* PTP/MTP cameras are matched — PTP still-image class 6, or MTP). |
| `gp_port_usb_clear_halt` | `(GPPort*, int ep)` | Clear a stalled endpoint (IN/OUT/INT). |
| `gp_port_usb_msg_write` / `gp_port_usb_msg_read` | `(GPPort*, int request, int value, int index, char *bytes, int size)` | Standard **control** transfers (vendor requests). |
| `gp_port_usb_msg_interface_write/read` | same | Interface-directed control transfers. |
| `gp_port_usb_msg_class_write/read` | same | Class-directed control transfers. |
| `gp_port_seek` | `(GPPort*, int offset, int whence)` | For `usbdiskdirect`. |
| `gp_port_send_scsi_cmd` | `(GPPort*, int to_dev, char *cmd, int cmd_size, char *sense, int sense_size, char *data, int data_size)` | For `usbscsi`. |

### Android/iOS bridging hook ⭐

| Function | Signature | Notes |
|----------|-----------|-------|
| `gp_port_usb_set_sys_device` | `(int fd)` | **Pass a pre-opened USB file descriptor** so libgphoto2 skips device discovery entirely. Must be called *before* library init. Returns `GP_ERROR_NOT_SUPPORTED` unless built against libusb ≥ 1.0.23 (`HAVE_LIBUSB_WRAP_SYS_DEVICE`). |
| `gp_port_usb_get_sys_device` | `(void)` | Read back the fd, or `-1` if unset. |

This is the single most important API for your goal. On Android (and any sandboxed
platform) an app cannot enumerate or open USB devices directly — it gets a file
descriptor from the OS (`UsbManager.openDevice()` → `UsbDeviceConnection.getFileDescriptor()`).
Handing that fd here lets the whole stack run unchanged. The `libusb1` backend wraps it
with `libusb_wrap_sys_device()` (see §3). Full walkthrough in
[04-extending.md](04-extending.md#android).

### Errors

`gp_port_set_error(port, fmt, …)` (drivers set detail), `gp_port_get_error(port)` (read
it back). Codes live in `gphoto2-port-result.h` (`GP_ERROR`, `GP_ERROR_IO`,
`GP_ERROR_TIMEOUT`, `GP_ERROR_IO_USB_FIND`, `GP_ERROR_IO_USB_CLAIM`,
`GP_ERROR_NOT_SUPPORTED`, `GP_ERROR_BAD_PARAMETERS`, `GP_ERROR_NO_MEMORY`, …);
`gp_port_result_as_string(code)` renders them.

---

## 3. Port enumeration — `gphoto2-port-info-list.c`

| Function | Signature | Notes |
|----------|-----------|-------|
| `gp_port_info_list_new` / `_free` | `(GPPortInfoList**)` / `(…*)` | |
| `gp_port_info_list_load` | `(GPPortInfoList*)` | dlopen every iolib in `IOLIBS`, call each `gp_port_library_list` to enumerate present ports. |
| `gp_port_info_list_append` | `(GPPortInfoList*, GPPortInfo)` | |
| `gp_port_info_list_count` | `(GPPortInfoList*)` | |
| `gp_port_info_list_lookup_path` | `(GPPortInfoList*, const char *path)` | e.g. `"usb:001,017"` → index. |
| `gp_port_info_list_lookup_name` | `(GPPortInfoList*, const char *name)` | |
| `gp_port_info_list_get_info` | `(GPPortInfoList*, int n, GPPortInfo*)` | |
| `gp_port_info_{get,set}_name` | `(GPPortInfo, char**/const char*)` | Human name. |
| `gp_port_info_{get,set}_path` | same | Path string. |
| `gp_port_info_{get,set}_type` | `(GPPortInfo, GPPortType[*])` | Bus type. |
| `gp_port_info_{get,set}_library_filename` | | Which iolib `.so` serves it. |
| `gp_port_info_new` | `(GPPortInfo*)` | |
| `gp_port_init_localedir` / `gp_port_message_codeset` | | i18n setup. |

---

## 4. Logging — `gphoto2-port-log.c`

Levels: `GP_LOG_ERROR`, `GP_LOG_VERBOSE`, `GP_LOG_DEBUG`, `GP_LOG_DATA` (raw bytes).

| Function | Notes |
|----------|-------|
| `gp_log_add_func(level, GPLogFunc, data)` → id | Install a sink (frontend or your app). |
| `gp_log_remove_func(id)` | Remove it. |
| `gp_log(level, domain, fmt, …)` / `gp_logv(...)` | Emit a message. |
| `gp_log_with_source_location(level, file, line, func, fmt, …)` | Backing for the `GP_LOG_*` macros. |
| `gp_log_data(domain, data, size, fmt, …)` | Hex-dump a buffer (the wire trace). |

Also the enum/flag ↔ string utilities used to render abilities/operations in logs:
`gpi_enum_to_string`, `gpi_string_to_enum`, `gpi_flags_to_string_list`,
`gpi_string_to_flag`, `gpi_string_or_to_flags`, `gpi_string_list_to_flags`.

> **Debugging tip.** `gp_log_add_func(GP_LOG_DATA, myfn, NULL)` gives you the exact PTP
> packets on the wire — indispensable when adding a camera or diagnosing a stall.

---

## 5. The transport backends (iolibs)

Each is a self-contained plugin implementing the `GPPortOperations` vtable. They share a
naming pattern: `gp_port_<bus>_open/close/read/write/...` plus the three exported
`gp_port_library_*` entry points.

### `libusb1/libusb1.c` (1637 lines) — the modern USB backend ⭐

The one used for essentially all mirrorless-over-USB today; built on **libusb 1.0**.

- `gp_port_library_list` / `load_devicelist` — enumerate USB devices into port entries.
- `gp_libusb1_open` / `_close` — claim/release the interface; set up async interrupt
  URBs for the event endpoint (`gp_libusb1_queue_interrupt_urbs`, `_cb_irq`,
  `_close_async_interrupts`).
- `gp_libusb1_read` / `_write` — bulk transfers.
- `gp_libusb1_check_int` — read the interrupt endpoint (camera events).
- `gp_libusb1_msg` + the `_msg_{write,read}_lib`, `_msg_interface_*`, `_msg_class_*`
  wrappers — control transfers.
- `gp_libusb1_find_device_lib` (by VID/PID), `gp_libusb1_find_device_by_class_lib`,
  `gp_libusb1_match_mtp_device` (probe an MTP interface descriptor),
  `gp_libusb1_find_path_lib`, `gp_libusb1_find_ep` / `_find_first_altsetting` (endpoint
  discovery), `gp_libusb1_clear_halt_lib`, `gp_libusb1_reset`, `gp_libusb1_update`.
- **`external_sys_device`** — the static that holds the wrapped Android fd. When
  `gp_port_usb_get_sys_device() != -1`, `open`/enumeration call
  `libusb_wrap_sys_device(ctx, fd, &handle)` instead of scanning the bus, and the device
  list is faked to that single wrapped device. This is the fd path end-to-end.

### `usb/libusb.c` (1227 lines) — legacy USB (libusb 0.1)

Same surface as libusb1 (`gp_port_usb_*`), built on the deprecated libusb-0.1. Kept for
old platforms; prefer libusb1. On macOS/Android you'd build/link libusb1.

### `ptpip/ptpip.c` (272 lines) — PTP/IP over WiFi ⭐

Tiny by design: PTP/IP is mostly TCP framing, and the real protocol logic lives in the
**ptp2 driver's** `ptpip.c` (doc 03). This iolib just opens the TCP sockets and does
`read`/`write`. `gp_port_library_list` can enumerate cameras via mDNS/Bonjour when
built with it (`_ptpip_resolved`, `_ptpip_enumerate`) — otherwise you set the IP
manually (`ptpip:<addr>`). This is the path for WiFi-tethered capture where USB isn't
possible (relevant to iOS, which has no general USB host access).

### `serial/unix.c` (940 lines) — RS-232

`gp_port_serial_open/close/read/write`, baud conversion (`gp_port_serial_baudconv`,
`_check_speed`), pins (`get/set_pin`), `send_break`, `flush`, and lock-file handling
(`gp_port_serial_lock/unlock`). Only relevant to 1990s–2000s cameras; no modern use.

### `disk/disk.c` (354 lines) — mounted storage

Treats a mounted mountpoint as a "port" (the "Directory Browse" pseudo-camera and DCF
card readers). Enumerates mountpoints; `read`/`write` are file ops.

### `usbdiskdirect/linux.c` (410 lines) & `usbscsi/linux.c` (384 lines)

Raw block/SCSI access to USB-mass-storage cameras that also expose a proprietary command
channel (some Ricoh/older models). Linux-only. `seek`/`send_scsi_cmd` live here.

### `vusb/vusb.c` + `vcamera.c` (2546 lines) — the **virtual camera** ⭐ (for you)

A fully in-process **fake PTP camera**. `vusb.c` implements the `GPPortOperations`
surface but instead of touching hardware it calls into `vcamera.c`, which emulates a PTP
device (device info, a couple of stored images, capture, property get/set). This is what
the test-suite and fuzzers drive. **For development on a platform with no camera
attached — or before your hardware bring-up works — this lets you exercise the entire
core + ptp2 stack with zero hardware.** Select it with the `disk:` / `usb:` vusb port in
test builds.

---

## 6. Portability & versioning

- `gphoto2-port-portability.c` — thin OS wrappers (`gp_system_dir`, directory scanning,
  path helpers) so the rest of the code avoids `#ifdef` soup. **This is one of the files
  a Windows/iOS port would touch.**
- `gphoto2-port-version.c` — `gp_port_library_version()` mirrors the core's version
  reporting for the port lib.
- `gphoto2-port-locking.c` — `gpi_libltdl_lock()` / `_unlock()`: the single global mutex
  serializing libtool loader calls across both libraries.

---

Continue to [03-ptp2-driver.md](03-ptp2-driver.md).
