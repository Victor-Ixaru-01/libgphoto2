# 04 — Extending libgphoto2 (and bridging to iOS / Android / Windows)

This is the practical guide for building on top of what docs 00–03 describe. It has two
halves: **(A)** the routine extension recipes (add a camera, add a setting, add a
transport) and **(B)** the platform-bridging reality for iOS, Android, Windows and macOS
— your stated end goal.

---

## A. Routine extensions

### A.0 Where the build plugs things in

Two build systems coexist; changes go in both.

- **Autotools:** camlibs are pulled in by `camlibs/Makefile.am`, which `include`s each
  driver's `Makefile-files` (e.g. `include ptp2/Makefile-files`). iolibs are subdirs of
  `libgphoto2_port/`. Feature detection is in `configure.ac` /
  `libgphoto2_port/configure.ac`.
- **Meson:** `meson_options.txt` has `camlibs` and `iolibs` array options; the port
  `meson.build` loops `foreach iolib : get_option('iolibs')` and `subdir(iolib)`.

You rarely add a *camlib* (there's essentially one you'd touch — `ptp2`). You add
**models and settings inside ptp2**, or a **new iolib** for a new bus/platform.

### A.1 Add a camera model

Most new mirrorless bodies need **zero code** — they're detected by the generic PTP/MTP
class match and "just work." Add an explicit entry only to attach a friendly name,
capability flags, or a quirk. In `camlibs/ptp2/library.c`, add a row to `models[]`:

```c
{ "Canon:EOS R100",  0x04a9, 0x32ff, PTP_CAP|PTP_CAP_PREVIEW },
```

Fields are `{ "Vendor:Model", usb_vendor, usb_product, device_flags }`. The
`device_flags` (`device-flags.h`) both drive capability advertising in
`camera_abilities` (doc 03 §2.1) and toggle per-model quirks (`DEVICE_FLAG_*`) consumed
in `ptp.c`/`library.c`. That single line makes the model appear in
`gp_camera_autodetect` with the right capabilities.

If the body needs a proprietary capture/liveview op the driver doesn't yet issue, you
also add a `ptp_<vendor>_<op>` function in `ptp.c` (+ a `PTP_OC_*` opcode in `ptp.h`)
and call it from the vendor's `camera_<vendor>_capture` in `library.c`.

### A.2 Add a config setting

In `camlibs/ptp2/config.c`, add a `struct submenu` row to the relevant menu, giving the
setting's name, its PTP device-property code (`PTP_DPC_*`, defined in `ptp.h`), the
widget type, and get/put callbacks. For an enumerated choice, also add a
`deviceproptable*` value table so raw numbers render as words:

```c
static struct deviceproptableu16 mymode[] = {
    { N_("Off"), 0x0000, 0 }, { N_("On"), 0x0001, 0 },
};
/* in the menu array: */
{ N_("My Mode"), "mymode", PTP_DPC_SomeVendorProp, PTP_VENDOR_X,
  PTP_DTC_UINT16, _get_GenericU16Table, _put_GenericU16Table },
```

`camera_get_config` will now surface it as a `CameraWidget`, and `camera_set_config`
will translate a user's choice back to a `ptp_setdevicepropvalue` write. No core changes.

### A.3 Add a new transport (iolib) — the general recipe

This is the key to reaching a camera from a new environment. Create
`libgphoto2_port/<mybus>/<mybus>.c` implementing the plugin contract (doc 02 §1):

```c
GPPortType        gp_port_library_type(void)       { return GP_PORT_USB; /* or a new type */ }
int               gp_port_library_list(GPPortInfoList *l) { /* enumerate present devices */ }
GPPortOperations *gp_port_library_operations(void) { static GPPortOperations ops; 
    ops.open = my_open; ops.close = my_close;
    ops.read = my_read; ops.write = my_write; ops.check_int = my_check_int;
    ops.find_device = my_find; /* … */ return &ops; }
```

Fill `read`/`write`/`check_int`/`open`/`close` with your platform's I/O; leave unused
methods `NULL`. Register the dir in the build. The core and ptp2 are untouched — they'll
call your `read`/`write` through `gp_port_*`. **This is usually cleaner than a fresh
iolib, though:** for USB you can almost always reuse `libusb1` unchanged and just feed it
a file descriptor (§B.2).

### A.4 Test with no camera attached — the virtual camera

`libgphoto2_port/vusb/` + `vcamera.c` emulate a full PTP camera in-process (doc 02 §5).
Point a build at the vusb iolib and you can exercise `gp_camera_init`, browsing,
capture, config get/set and the whole ptp2 stack **without hardware**. Use it to bring
up your bindings/UI on a new platform before the real transport works, and to write
regression tests.

---

## B. Platform bridging (iOS · Android · Windows · macOS)

First, calibrate the premise. libgphoto2 is a **native C library**, and "connecting a
mirrorless camera" means one of exactly two physical transports:

1. **USB** — handled by the `libusb1` iolib → PTP-over-USB (`usb.c`) in ptp2.
2. **WiFi (PTP/IP)** — handled by the `ptpip` iolib → `ptpip.c` (or `fujiptpip.c`) in
   ptp2.

The porting question is therefore always: *"can this platform give libusb a USB
connection, or must I fall back to PTP/IP over the network?"* Everything above the
transport (core + ptp2) is already portable C and needs no per-platform work.

### B.1 macOS (works today) 🟢

You are on macOS, and it is a first-class Unix target. `libusb1` works on macOS, ltdl
plugin loading works, and PTP/IP works. Build with the autotools or meson instructions
in the repo README. USB tethering to any EOS/Nikon/Sony/etc. works out of the box **as
long as no other process has grabbed the device** — macOS's `ptpcamerad`/Image Capture
and Photos will claim PTP cameras; you typically `killall PTPCamera` (or use Image
Capture's "connecting this camera opens…: No Application") so libgphoto2 can claim the
interface. This is the environment to prototype in.

### B.2 Android (USB supported via a file descriptor) 🟢

Android **blocks** direct USB enumeration/`open()` from an app sandbox — but the OS will
hand your app a file descriptor for a device the user approves. libgphoto2 has a
purpose-built hook for exactly this (doc 02 §2):

```
Java:   UsbManager.openDevice(dev) → UsbDeviceConnection.getFileDescriptor()  // an int
JNI  →  gp_port_usb_set_sys_device(fd);   // BEFORE gp_camera_init / any port load
        gp_camera_new(&cam);  gp_camera_init(cam, ctx);   // runs entirely on that fd
```

Under the hood `libusb1.c` sees `gp_port_usb_get_sys_device() != -1` and calls
`libusb_wrap_sys_device(ctx, fd, &handle)` instead of scanning the bus, faking a
single-device list around your handle (`external_sys_device`, doc 02 §5). Requirements:

- Build `libusb` **≥ 1.0.23** for Android (it has `libusb_wrap_sys_device`), and build
  libgphoto2 against it so `HAVE_LIBUSB_WRAP_SYS_DEVICE` is defined
  (`libgphoto2_port/configure.ac:387` / `meson.build:26`). Otherwise
  `gp_port_usb_set_sys_device` returns `GP_ERROR_NOT_SUPPORTED`.
- Cross-compile the whole stack with the Android NDK. Ship the camlib/iolib `.so`s in
  your APK and set `CAMLIBS`/`IOLIBS` to their on-device path (or link them statically —
  see B.5).
- Request the USB-host intent/permission and pass the approved fd across JNI.

This is a **proven path** (community projects and the `gp_port_usb_set_sys_device` API
exist specifically for it). WiFi (PTP/IP) is also available on Android with no special
hooks — connect to the camera's AP and use a `ptpip:<addr>` port.

### B.3 iOS / iPadOS (WiFi is the realistic path) 🟡

iOS is the hardest target, and the constraint is Apple's, not libgphoto2's:

- **USB host access is not generally available to third-party apps.** There is no public
  libusb-style API; USB accessories are gated behind the MFi/External Accessory program
  (`ExternalAccessory` framework) or, on recent iPadOS, narrow DriverKit paths — none of
  which libusb targets today. So the libusb1-over-USB path effectively **does not work on
  stock iOS**.
- **PTP/IP over WiFi is the viable route.** The `ptpip` transport is plain BSD sockets
  and compiles for iOS. Connect the phone to the camera's WiFi (or a shared network),
  discover/enter the camera IP, and drive capture/download through `ptpip.c` /
  `fujiptpip.c`. Most modern mirrorless bodies expose a WiFi tethering/PTP-IP mode
  precisely for phone apps.
- The C core + ptp2 themselves cross-compile for iOS (they're portable POSIX C). The
  open questions are the **plugin loader** (iOS forbids `dlopen` of app-external code, so
  you must **statically register** the camlib/iolib rather than ltdl-load them — see
  B.5) and App Store policy.

Practical iOS plan: static-link `ptp2` + `ptpip` (+ core), bypass ltdl, and expose only
the PTP/IP path. Treat USB as out of scope unless you enter MFi.

### B.4 Windows (no official support; partial plumbing exists) 🟡

The README states Windows isn't currently supported. However there **is** scattered
`#ifdef WIN32` code (`gphoto2-port-portability.c`, `serial/unix.c` guards,
`gphoto2-setting.c`, and Winsock `WSAStartup` in ptp2 `library.c`/`ptpip.c`), so it's
"partially plumbed, not maintained." libusb1 does run on Windows (WinUSB/libusbK
backend). A Windows port's real work is: the portability layer (paths, dir scanning,
process/OS glue), the ltdl plugin loading (or static registration), and a USB backend
driver install (WinUSB via Zadig or a bundled INF). Doable, but expect it to be the
bulk of a port. WiFi/PTP-IP is the low-friction fallback here too.

### B.5 The cross-cutting issue: plugin loading vs. static linking

Everything above depends on how the two plugin systems (doc 00 §2) resolve on the
target:

- **Desktop (macOS/Linux/BSD):** keep the ltdl `dlopen` model; just set
  `CAMLIBS`/`IOLIBS` to where the `.so`s are installed.
- **Mobile/sandboxed/Windows:** prefer **static registration**. Instead of dlopening,
  compile `ptp2` and the needed iolibs into your binary and call their
  `camera_id`/`camera_abilities`/`camera_init` and
  `gp_port_library_type`/`_list`/`_operations` directly, registering them with the core.
  This sidesteps `dlopen` restrictions (iOS) and packaging pain. (This is also how the
  `vusb` test harness is wired.) Budget time for a small "static camlib/iolib registry"
  shim — it's the main structural change a mobile port needs beyond cross-compilation.

### B.6 Don't reimplement — consider the existing bindings

If your goal is an app rather than a C fork, the fastest bridge is an existing binding
over a cross-compiled libgphoto2 (README lists Java, Python, C#, Go, Rust, Node, Ruby,
Crystal). For Android specifically, `gphoto2-java`/JNA-style wrappers already pair with
the `gp_port_usb_set_sys_device` fd trick. Your native work then reduces to
cross-compilation + the fd handoff (Android) or the PTP/IP path (iOS).

---

## C. Suggested first steps for your project

1. **Prototype on macOS** (§B.1) against a real camera over USB — get `gp_camera_init`,
   `gp_camera_capture`/`trigger_capture` + `wait_for_event`, `capture_preview`, and
   `get_config`/`set_config` working end-to-end. This validates your understanding of
   docs 00–03 with zero porting risk.
2. **Stand up the virtual camera** (§A.4) in CI so your higher-level code has a
   hardware-free test target.
3. **Android USB**: cross-compile with NDK + libusb ≥ 1.0.23, wire the JNI fd handoff
   through `gp_port_usb_set_sys_device` (§B.2).
4. **iOS**: static-link core + `ptp2` + `ptpip`, expose the WiFi/PTP-IP path only, and
   replace ltdl with static registration (§B.3, §B.5).
5. Only then consider **Windows** and/or **USB-on-iOS (MFi)** if the product needs them.

The architecture is on your side: the three layers are cleanly separated by function
pointers and `gp_*` wrappers, so each platform port is confined to the transport +
loader, never the camera logic.
