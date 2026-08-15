# Detecting Canon photo ↔ video (movie) mode

Canon exposes the still/movie state as ordinary EOS **device properties**, and pushes a
**property-changed event** when the user flips the physical photo/video switch (or turns the
mode dial to the Movie position). There is no dedicated "mode switched" event code — you
watch the generic event queue, then re-read the mode.

Two pieces:

- `gp_iccamera_get_movie_mode()` — reads the current mode on demand.
- `gp_iccamera_poll_events()` — tells you *when* to re-read (a `PROP <hex>` line fires).

## API

```c
typedef enum {
    GP_ICCAMERA_MOVIE_MODE_UNKNOWN = -1,  /* neither property readable */
    GP_ICCAMERA_MOVIE_MODE_PHOTO   =  0,  /* stills */
    GP_ICCAMERA_MOVIE_MODE_VIDEO   =  1   /* movie / video */
} gp_iccamera_movie_mode;

/* raw_fixedmovie / raw_aemode (optional) receive the underlying values, -1 if unreadable. */
int gp_iccamera_get_movie_mode(gp_iccamera *, int *raw_fixedmovie, int *raw_aemode);
```

It reads **`FixedMovie` (0xD1C2)** — the movie switch — as the primary signal, and
**`AutoExposureMode` (0xD105)** whose value `0x14 == "Movie"` as a fallback/cross-check.
Call it from the **background** camera thread, like the other `gp_iccamera_*` ops.

## Which property moves? Verify once per body

The DPC that changes when you flip the switch is **not identical across bodies**:

- **R-series** (R50/R5/R6/R7…): the dedicated photo/video switch drives **`FixedMovie` (0xD1C2)**.
- **DSLRs** with a Movie position on the mode dial: the **AE mode** (`0xD105` / `AEModeDial 0xD138`) reports `"Movie"` (`0x14`).

To pin it on your body: flip the switch while draining `poll_events`, note the `PROP <hex>`
that fires, and read that code with `gp_iccamera_get_eosprop`. `get_movie_mode` already
covers 0xD1C2 + 0xD105; if yours turns out to signal on `AEModeDial (0xD138)` instead, add a
third `icc_read_eos_int(params, 0xD138)` check in `gp_iccamera_get_movie_mode()`.

## Wiring it to poll_events (Swift)

`poll_events` drains the one EOS event queue and returns one line per event; a device-property
change is reported as **`PROP <hex>`**. Re-read the mode when a mode-related code appears (or,
simplest, on **any** `PROP` line — `get_movie_mode` is cheap).

```swift
import Foundation

enum MovieMode: Int { case unknown = -1, photo = 0, video = 1 }

/// Current still/movie state. Call on the camera's background queue.
func currentMovieMode(_ cam: OpaquePointer) -> MovieMode {
    var fixedMovie: Int32 = -1, aeMode: Int32 = -1
    let m = gp_iccamera_get_movie_mode(cam, &fixedMovie, &aeMode)
    // fixedMovie / aeMode are the raw values — log them once to confirm the mapping.
    return MovieMode(rawValue: Int(m)) ?? .unknown
}

/// Long-lived loop on the background thread. Emits a callback only on transitions.
func watchMovieMode(_ cam: OpaquePointer,
                    onChange: @escaping (MovieMode) -> Void) {
    var last: MovieMode = .unknown
    // Property codes worth reacting to (hex, lowercased) — see note above.
    let modeCodes: Set<String> = ["d1c2", "d105", "d138"]

    while !Thread.current.isCancelled {
        var buf = [CChar](repeating: 0, count: 4096)
        let n = gp_iccamera_poll_events(cam, &buf, 4096)   // returns event count, or <0
        guard n >= 0 else { continue }

        let lines = String(cString: buf)
            .split(separator: "\n").map(String.init)

        // Re-read only if a PROP line for a mode code showed up (or drop this filter
        // and re-read on any non-empty poll — get_movie_mode is cheap).
        let sawModeChange = lines.contains { line in
            let parts = line.split(separator: " ")
            return parts.first == "PROP" && parts.count > 1 &&
                   modeCodes.contains(parts[1].lowercased())
        }
        guard sawModeChange else { continue }

        let now = currentMovieMode(cam)
        if now != last, now != .unknown {
            last = now
            DispatchQueue.main.async { onChange(now) }   // e.g. "video" → swap UI
        }
    }
}
```

Usage:

```swift
// device.name == "Canon EOS R50"
watchMovieMode(cam) { mode in
    switch mode {
    case .video: showVideoUI()
    case .photo: showPhotoUI()
    case .unknown: break
    }
}
```

## Notes

- **Keep an event pump running.** These property-changed events only arrive if something
  drains the queue — the same `poll_events` loop you likely already run for capture/update.
- **First read may be UNKNOWN** until the camera has reported the property once; a
  `RequestDevicePropValue` refresh happens inside `get_movie_mode`, so a second call usually
  resolves it.
- **Semantics caveat.** `FixedMovie` is treated as `0 = photo`, non-zero = video. Confirm on
  your body via the raw out-params before relying on it in shipping UI.
