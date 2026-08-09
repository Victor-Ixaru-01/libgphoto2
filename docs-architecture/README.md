# libgphoto2 — Architecture & API Reference (Modern Mirrorless Path)

This documentation set explains **how libgphoto2 works internally**, focused on the
path that matters for modern mirrorless / DSLR cameras: the **core library**, the
**port (transport) layer**, and the **PTP/MTP driver (`camlibs/ptp2`)**. Together
these three pieces are what actually talk to Canon EOS, Nikon Z/D, Sony Alpha,
Fujifilm, OM/Olympus, Panasonic, Sigma and Leica bodies.

> **Scope note.** libgphoto2 also ships ~55 *legacy* camera drivers (old webcams and
> 2000s point-and-shoots) under `camlibs/`. Those are intentionally **not** covered
> here — none of them are relevant to mirrorless cameras, which universally speak
> PTP/MTP and are handled by the single `ptp2` driver. If you ever need them, they
> all follow the same camlib plugin contract documented in
> [01-core-api.md](01-core-api.md#the-camlib-plugin-contract).

## What libgphoto2 is (and is not)

- It is a **C backend library**. It has **no GUI** and is not an application. Frontends
  (gphoto2 CLI, digiKam, Darktable, Entangle, gPhoto Python/Java/Rust bindings, …)
  link against it.
- It talks to cameras that speak a **protocol it understands**. For modern cameras
  that protocol is **PTP** (Picture Transfer Protocol) and its Microsoft superset
  **MTP**. USB-Mass-Storage cameras don't need libgphoto2 at all — the OS mounts them.
- It is portable across **Linux, macOS, the BSDs and (with caveats) Android**. There is
  **no upstream Windows or iOS support today** — see
  [04-extending.md](04-extending.md) for exactly what stands in the way and what a
  port would involve, since that is your stated goal.

## The three layers at a glance

```
   ┌─────────────────────────────────────────────────────────────┐
   │  Frontend (your app / gphoto2 CLI / digiKam / a mobile UI)   │
   └───────────────────────────────┬─────────────────────────────┘
                                    │  public API:  gp_camera_*, gp_file_*, gp_widget_*
   ┌────────────────────────────────▼────────────────────────────┐
   │  libgphoto2  (core)   →  docs: 01-core-api.md                │
   │  Camera object · CameraFilesystem (caching) · CameraFile ·   │
   │  CameraWidget (config tree) · GPContext (callbacks) ·        │
   │  CameraAbilitiesList (driver DB + autodetect)                │
   └───────────────┬───────────────────────────┬─────────────────┘
       loads .so   │                           │  dlopen()s the matching
       driver via  │                           │  io-library at runtime
       dlopen()    ▼                           ▼
   ┌───────────────────────────┐   ┌──────────────────────────────┐
   │ camlibs/ptp2 (the driver) │   │ libgphoto2_port (transport)  │
   │ → docs: 03-ptp2-driver.md │   │ → docs: 02-port-api.md        │
   │ PTP protocol · vendor ops │   │ libusb1 (USB) · ptpip (WiFi) ·│
   │ config tree · marshalling │◄──┤ serial · disk · usbscsi · …   │
   └───────────────────────────┘   └──────────────────────────────┘
                                    the driver sends/receives bytes
                                    only through gp_port_* calls
```

Two independent plugin systems are at play, both using **libtool `ltdl`** at runtime:

1. **camlibs** — camera drivers. Each exports `camera_id`, `camera_abilities`,
   `camera_init`. For mirrorless, this is always `ptp2`.
2. **iolibs** — port/transport drivers. Each exports `gp_port_library_type`,
   `gp_port_library_list`, `gp_port_library_operations`.

The core never hard-links either; it discovers and loads them from disk. That is the
single most important thing to understand before extending the project.

## Read in this order

| # | Document | What it covers |
|---|----------|----------------|
| 0 | [00-overview.md](00-overview.md) | The object model, control/data flow, end-to-end walkthroughs of autodetect / init / download / capture, the two plugin contracts, threading & memory conventions. **Start here.** |
| 1 | [01-core-api.md](01-core-api.md) | `libgphoto2/` + public headers. Every public function of the core, grouped by object (Camera, AbilitiesList, Filesystem, File, Widget, Context, List, Setting, Result, Version) with inputs/outputs. |
| 2 | [02-port-api.md](02-port-api.md) | `libgphoto2_port/`. The `gp_port_*` API, the info-list, logging, and every transport backend (libusb1, legacy libusb, serial, ptpip, disk, usbscsi, usbdiskdirect, vusb). |
| 3 | [03-ptp2-driver.md](03-ptp2-driver.md) | `camlibs/ptp2/`. `PTPParams`, the protocol layer `ptp.c` (grouped by vendor), the USB/PTP-IP transports, `library.c` (the camlib implementation), `config.c` (the settings tree), marshalling, CHDK and Olympus wrapping. |
| 4 | [04-extending.md](04-extending.md) | **Your expansion guide.** How to add a camera, add a config setting, and — in depth — what bridging to Android / iOS / Windows actually requires, including the existing `gp_port_usb_set_sys_device()` file-descriptor hook. |
| 5 | [05-config-reference.md](05-config-reference.md) | **Full config reference.** Every one of the 359 camera settings exposed by ptp2 `config.c` — the exact `name` keys for `--get-config`/`--set-config`, access mode (read-only vs. read/write), and which camera brands implement each. |

### Brand guides

| Guide | What it covers |
|-------|----------------|
| [canon-cameras.md](canon-cameras.md) | **Canon end-to-end.** Which driver runs a given Canon; the two Canon PTP dialects (EOS vs. legacy PowerShot); the EOS handshake, capture & event model; live view & bulb; all 102 Canon settings; CHDK; and the standalone legacy `camlibs/canon` driver. |

### Project plans

| Plan | What it covers |
|------|----------------|
| [ios-framework-plan.md](ios-framework-plan.md) | **iOS framework to bridge libgphoto2 → a Canon over USB.** The transport reality on iOS (why `ICCameraDevice.requestSendPTPCommand` is the App-Store-safe seam for iPhone + iPad), the recommended architecture, the LGPL and command-filtering risks, and a phased build plan with a Swift API sketch. |
| [ios-phase0-runbook.md](ios-phase0-runbook.md) | **Phase 0 spike runbook.** How to run `PTPPhase0Tester.swift` against a real Canon EOS to settle the make-or-break question — does iOS forward vendor PTP opcodes or return `-21249`? Includes wiring, verdict interpretation, and the corrected Canon EOS opcode map. |

## Conventions used throughout

- **Return codes.** Almost every function returns an `int` gphoto2 error code:
  `GP_OK` (`0`) or `> 0` on success, a **negative** `GP_ERROR_*` on failure. Callers
  test `if (ret < GP_OK)`. The string form is `gp_result_as_string(ret)`.
- **Ownership.** "out" pointer params are filled by the callee. Objects are
  reference-counted (`gp_*_ref` / `gp_*_unref`); `_free` is the deprecated hard form.
- **`GPContext`.** Passed almost everywhere. It carries the frontend's callbacks
  (progress bar, error/status text, yes/no questions, cancellation). Drivers *report
  up* through it; they never talk to the user directly.
- **Signature notation in the tables.** Types are shown as in the headers. `[out]`
  marks a pointer the function writes through; `[in/out]` marks one it both reads and
  overwrites (e.g. a size that says "buffer size" going in and "bytes used" coming out).

_Generated as an internal architecture reference. Line-number citations point at the
tree state on the `claude/libgphoto2-docs-architecture` branch and may drift as the
code changes._
