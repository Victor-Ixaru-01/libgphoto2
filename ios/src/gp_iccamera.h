/*
 * gp_iccamera.h — bridge libgphoto2's ptp2 engine to Apple ImageCaptureCore.
 *
 * The app provides ONE C callback that performs a single PTP transaction via
 * ICCameraDevice.requestSendPTPCommand; this shim plugs that callback into ptp2's
 * PTPParams transport (sendreq/senddata/getdata/getresp) so the full, battle-tested
 * Canon EOS logic in ptp.c/library.c runs on top of it. No dlopen, no libusb.
 *
 * Threading: call the gp_iccamera_* functions from a BACKGROUND thread. The callback
 * will block that thread until requestSendPTPCommand's completion fires (on another
 * queue), so never call these from the main thread.
 */
#ifndef GP_ICCAMERA_H
#define GP_ICCAMERA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Perform ONE PTP transaction. Implemented by the app (Swift) over
 * ICCameraDevice.requestSendPTPCommand.
 *
 *   ctx           opaque pointer passed to gp_iccamera_new()
 *   command       the PTP command container bytes (len·type=1·opcode·txid·params)
 *   command_len   its length
 *   out_data      data-phase bytes to send TO the camera (NULL / 0 if none)
 *   out_data_len  its length
 *   in_data       [out] callee malloc()s the data phase FROM the camera here
 *                 (set to NULL / 0 if none; ownership passes to the shim, which frees it)
 *   in_data_len   [out] its length
 *   response      [out] caller-provided buffer to receive the PTP response container
 *   response_len  [in] capacity of `response`; [out] bytes written
 *
 * Return 0 on success, non-zero on failure (e.g. the ICReturn error code such as
 * -21249 ICReturnPTPNotAuthorizedToSendCommand). On failure the transaction is
 * reported to ptp2 as an I/O error.
 */
typedef int (*gp_iccamera_transact_cb)(
	void *ctx,
	const uint8_t *command, int command_len,
	const uint8_t *out_data, int out_data_len,
	uint8_t **in_data, int *in_data_len,
	uint8_t *response, int *response_len);

typedef struct gp_iccamera gp_iccamera;

/* Create a session over the given transact callback and read the camera's DeviceInfo
 * (ImageCaptureCore already owns the PTP session, so no OpenSession is sent).
 * Returns NULL on failure. */
gp_iccamera *gp_iccamera_new(gp_iccamera_transact_cb cb, void *ctx);
void         gp_iccamera_free(gp_iccamera *);

/* Proof ops (Phase 2): drive real ptp2 operations through the callback.
 * Each returns 0 on success (or a negative code) and writes a human string to `out`. */
int gp_iccamera_get_deviceinfo(gp_iccamera *, char *out, int outlen);
int gp_iccamera_eos_handshake(gp_iccamera *, char *out, int outlen);

/* Send an arbitrary PTP command through ptp2's transaction layer (for experimentation).
 * `params` are up to 5 uint32 op parameters (nparams of them). Writes the response code
 * and any returned data length to `out`. Returns the PTP response code (0x2001 == OK). */
int gp_iccamera_raw_op(gp_iccamera *, uint16_t opcode,
                       const uint32_t *params, int nparams,
                       char *out, int outlen);

/* Canon EOS capture + download in one call:
 *   - enters EOS remote mode, fires the shutter (half-press → full-press, retrying on busy),
 *   - polls ptp2's EOS event queue for the new object,
 *   - downloads it and hands back the bytes.
 * On success returns 0 and sets *outdata (malloc'd — free with gp_iccamera_freebuf) + *outlen,
 * writes the file extension ("jpg"/"cr3"/"cr2") to `ext`, and a human trace to `status`.
 * On failure returns a negative code with the reason in `status`. */
int  gp_iccamera_capture(gp_iccamera *, uint8_t **outdata, int *outlen,
                         char *ext, int extlen, char *status, int statuslen);
void gp_iccamera_freebuf(uint8_t *p);

/* Config tree (the full ptp2 / config.c settings over our transport).
 *   list_config  → newline-separated names of all settable widgets.
 *   get_config   → current `value` + newline-separated `choices` for one setting.
 *   set_config   → set one setting to `value` (a choice string). Returns 0 on success. */
int gp_iccamera_list_config(gp_iccamera *, char *out, int outlen);
int gp_iccamera_get_config(gp_iccamera *, const char *name,
                           char *value, int vlen, char *choices, int clen);
int gp_iccamera_set_config(gp_iccamera *, const char *name, const char *value);

/* Free and total space in KB for the PRIMARY storage slot (the first storage ID the camera
 * reports — on a two-slot body that is slot 1, and a slot with no card is not reported at
 * all). Pass NULL for either output if not needed.
 * Returns 0 on success, negative on error. A field the camera reports as "unknown" is set
 * to -1 rather than a bogus number, so check for < 0 before using a value.
 * Uses the standard PTP GetStorageIDs/GetStorageInfo (which is what ptp2's own
 * storage_info_func uses — the Canon EOS GetStorageInfo variant returns an undecoded blob);
 * returns negative if the body does not support the operation. Background thread only. */
int gp_iccamera_get_storageinfo(gp_iccamera *, int64_t *free_kb, int64_t *total_kb);

/* Canon EOS live view (streamed EVF frames).
 *   _start  → route the EVF to "PC" and enter live-view mode (call once). Writes a
 *             human trace to `status`. Returns 0 on success, negative on failure.
 *   _frame  → fetch ONE JPEG preview frame. Returns 0 and sets *outdata (malloc'd —
 *             free with gp_iccamera_freebuf) + *outlen on success; returns 1 (soft)
 *             when no frame is ready yet — just call again; negative on hard error.
 *             `hist` (optional, `histcap` uint32s) also receives the histogram carried
 *             in the same EVF buffer — 256-bucket uint32 channels (EDSDK layout), up to
 *             4 (R,G,B,Y order TBD); *histn = number of values written (0 if none).
 *             Pass hist=NULL / histcap=0 to skip.
 *   _stop   → leave live-view mode.
 * Call all three from a BACKGROUND thread, like the other gp_iccamera_* ops. */
int gp_iccamera_liveview_start(gp_iccamera *, char *status, int statuslen);
int gp_iccamera_liveview_frame(gp_iccamera *, uint8_t **outdata, int *outlen,
                               uint32_t *hist, int histcap, int *histn);
int gp_iccamera_liveview_stop (gp_iccamera *);

/* Fetch one EVF frame and list every sub-record (type, length, first bytes) to `out`, for
 * discovering undocumented records (histogram layout, focus, zoom…). Returns the record
 * count, or negative on error. Background thread; live view must be active. */
int gp_iccamera_liveview_inspect(gp_iccamera *, char *out, int outlen);

/* EVF coordinate-system size (from the last live-view frame) — the space AF/touch-AF points
 * use (e.g. 6000x4000). Returns 0 with *w,*h set, or negative if not captured yet. */
int gp_iccamera_get_coordsize(gp_iccamera *, uint32_t *w, uint32_t *h);

/* Read FocusInfoEx (0xD1D3) directly as the "sizeX,sizeY,…;{x,y,w,h},…" string, bypassing the
 * config-tree widget gating. 0 = string in `out` (may be empty); -1 = camera not reporting it
 * yet (needs live view / AF active). */
int gp_iccamera_get_focusinfo(gp_iccamera *, char *out, int outlen);

/* EVF frame rect (record type 13) from the last live-view frame — movable zoom/AF box in the
 * type-14 coordinate space. Candidate AF reticle when FocusInfoEx is silent. 0/-1. */
int gp_iccamera_get_evf_frame(gp_iccamera *, int *x, int *y, int *w, int *h);

/* Roll/pitch level (EVF record type 16): *a = roll x100, *b = pitch x100 (each mod 36000;
 * >18000 = negative). Returns 0 with values set, or -1. */
int gp_iccamera_get_level(gp_iccamera *, uint32_t *a, uint32_t *b);

/* Camera orientation, derived from the roll of the EVF electronic level (record type 16). */
typedef enum {
	GP_ICCAMERA_ORIENTATION_UNKNOWN            = -1, /* level not reported yet          */
	GP_ICCAMERA_ORIENTATION_LANDSCAPE          =   0,/* top up,    roll ~   0°          */
	GP_ICCAMERA_ORIENTATION_PORTRAIT_CW        =  90,/* rotated clockwise,  roll ~ +90° */
	GP_ICCAMERA_ORIENTATION_LANDSCAPE_INVERTED = 180,/* top down,  roll ~ ±180°         */
	GP_ICCAMERA_ORIENTATION_PORTRAIT_CCW       = 270 /* rotated counter-cw, roll ~ -90° */
} gp_iccamera_orientation;

/* Classify the body as portrait/landscape from the last live-view frame's roll (record type 16),
 * returning one of the gp_iccamera_orientation values above. Returns
 * GP_ICCAMERA_ORIENTATION_UNKNOWN (-1) when the level isn't being reported yet — enable it with
 * gp_iccamera_rollpitch(,1) AND keep live view running (the value rides in the EVF buffer).
 * Optionally writes the signed level in whole degrees to *roll_deg / *pitch_deg: roll = tilt about
 * the lens axis (the portrait/landscape signal), pitch = nose up/down. Caveat: when the camera is
 * aimed near straight up or down (|pitch| → 90°) roll degenerates and the class is unreliable —
 * gate on *pitch_deg there and hold the last good value. The CW/CCW → 90°/270° mapping follows the
 * R50; verify the sign on your body with gp_iccamera_liveview_inspect and swap the two PORTRAIT_*
 * cases in gp_iccamera_get_orientation() if reversed. */
int gp_iccamera_get_orientation(gp_iccamera *, int *roll_deg, int *pitch_deg);

/* Touch-AF: set the live-view AF frame to (x,y) in the EVF coordinate space (opcode 0x915A).
 * EXPERIMENTAL payload (x,y as 2x u32) — writes "SetLiveAfFrame(x,y) → 0x….." to `status`. */
int gp_iccamera_set_af_frame(gp_iccamera *, int x, int y, char *status, int statuslen);

/* Canon EOS commands (EDSDK-equivalent) — thin wrappers over ptp_canon_eos_* ops.
 * Each returns 0 on success or a negative code. Call from a BACKGROUND thread.
 * See docs-architecture/EDSDK-CAPABILITY-MAP.md for the full EDSDK→ptp2 mapping. */
int gp_iccamera_af             (gp_iccamera *, int on);         /* EdsCommand DoEvfAf: afdrive / afcancel */
int gp_iccamera_bulb           (gp_iccamera *, int start);      /* BulbStart / BulbEnd */
int gp_iccamera_drivelens      (gp_iccamera *, int amount);     /* DriveLensEvf: 1..3 near, 0x8001..0x8003 far */
int gp_iccamera_uilock         (gp_iccamera *, int lock);       /* StatusCommand UILock / UIUnLock */
int gp_iccamera_evf_zoom       (gp_iccamera *, int zoom);       /* EVF Zoom: 1 fit, 5, 6, 10, 15 */
int gp_iccamera_evf_zoomposition(gp_iccamera *, int x, int y);  /* EVF zoom rect top-left */
int gp_iccamera_dof_preview    (gp_iccamera *, int on);         /* Evf_DepthOfFieldPreview */
int gp_iccamera_popupflash     (gp_iccamera *);                 /* pop up the built-in flash */
int gp_iccamera_rollpitch      (gp_iccamera *, int on);         /* RequestRollPitchLevel */
int gp_iccamera_drive_powerzoom(gp_iccamera *, int mode);       /* DrivePowerZoom: 0 stop,1 wide,2 tele,0x11/0x12 +limit */

/* Generic Canon EOS device-property access by code (PTP_DPC_CANON_EOS_*), for the EVF/focus/
 * zoom scalar props that have no dedicated wrapper (EVFSharpness, EVFWBMode, EVFColorTemp,
 * EVFRecordStatus, PowerZoomPosition, FocusMode, LV_AF_EyeDetect, AFSelectFocusArea,
 * RefocusState, DepthOfField, …).
 *   get → refreshes (RequestDevicePropValue) then reads the cached desc; sets *value and,
 *         optionally, *datatype and the enumerated *choices (up to choicecap, *nchoices set).
 *   set → writes `value` using the property's own datatype.
 * Return 0 on success, negative on failure. Background thread. Scalars only — struct props
 * (FocusInfoEx) are not decoded here. */
int gp_iccamera_get_eosprop(gp_iccamera *, uint16_t code, uint32_t *value, uint32_t *datatype,
                            uint32_t *choices, int choicecap, int *nchoices);
int gp_iccamera_set_eosprop(gp_iccamera *, uint16_t code, uint32_t value);

/* Photo vs. video (movie) mode — reflects the body's physical photo/movie switch, or the
 * "Movie" position on the mode dial. Reads FixedMovie (0xD1C2, the movie switch) as the
 * primary signal, and AutoExposureMode (0xD105, whose value 0x14 == "Movie") as a fallback/
 * cross-check. Optionally writes the raw underlying values to *raw_fixedmovie / *raw_aemode
 * (each -1 when that property wasn't readable) so you can confirm the mapping on your body.
 * Returns one of the gp_iccamera_movie_mode values below. Background thread.
 *
 * NOTE: which DPC actually moves when you flip the switch varies per body (on R-series it's
 * usually 0xD1C2; on DSLRs with a Movie dial position it's the AE mode). If this returns
 * UNKNOWN or the wrong state on yours, flip the switch while draining gp_iccamera_poll_events,
 * note which "PROP <hex>" fires, and read that code with gp_iccamera_get_eosprop to pin the
 * mapping — see docs-architecture/canon-photo-video-mode.md. */
typedef enum {
	GP_ICCAMERA_MOVIE_MODE_UNKNOWN = -1, /* neither property readable */
	GP_ICCAMERA_MOVIE_MODE_PHOTO   =  0, /* stills             */
	GP_ICCAMERA_MOVIE_MODE_VIDEO   =  1  /* movie / video      */
} gp_iccamera_movie_mode;

int gp_iccamera_get_movie_mode(gp_iccamera *, int *raw_fixedmovie, int *raw_aemode);

/* Movie-size DISCOVERY probe: dumps the candidate Canon EOS movie properties — MovSize
 * (0xD1BB), MovieParam/2..5, VariableMovieRecSetting (0xD215) — each as a line carrying its
 * datatype, read/write flag, current raw code, and the enumeration of valid codes (or range).
 * Use it to reverse-engineer the resolution/framerate encoding on a specific body: call it,
 * change the movie recording size / frame rate on the camera, call it again, and diff which
 * property + code moved. That also answers whether one scalar code carries both resolution and
 * fps (a single prop moves) or they live in separate props. Writes newline-separated lines to
 * `out`; returns 0 on success, negative on usage error. Background thread.
 * See docs-architecture/canon-movie-size-probe.md. */
int gp_iccamera_movie_size_probe(gp_iccamera *, char *out, int outlen);

/* Full EOS device-property dump: one compact line per property in the driver's Canon EOS
 * cache — code, datatype, rw/ro, current raw value, and form (enum[N]/range/-), sorted by
 * code. The decisive discovery tool when a setting isn't under a code you guessed: dump,
 * change the setting on the camera, dump again, diff which code's `cur` moved. Pass a generous
 * buffer (>= 16 KB). Returns the number of properties written, or negative on usage error.
 * Background thread; run it in the relevant mode (e.g. movie mode — gp_iccamera_get_movie_mode()
 * == VIDEO) since some properties only populate there. */
int gp_iccamera_eos_props_dump(gp_iccamera *, char *out, int outlen);

/* What class of camera is connected (returned by gp_iccamera_capabilities). */
typedef enum {
	GP_ICCAMERA_CLASS_UNKNOWN      = 0, /* non-Canon or generic PTP                                */
	GP_ICCAMERA_CLASS_CANON_LEGACY = 1, /* PowerShot / non-EOS Canon (config tree works, no EOS events) */
	GP_ICCAMERA_CLASS_CANON_EOS    = 2  /* full EOS support: config tree, capture, live view, events    */
} gp_iccamera_class;

/* Self-describing capability report for ANY connected camera — this is the "what can this camera
 * do" probe, and it needs NO per-body knowledge. Writes to `out`: identity (model/firmware/serial/
 * vendor), class, feature detection (remote capture, live view, AF, movie switch, image format,
 * and which movie-size carrier — 0xD20D / 0xD29E / unknown), and the full list of supported
 * operations (commands, named). Returns the gp_iccamera_class (>=0), or negative on error. Pass a
 * generous buffer (>= 16 KB). For the full property list use gp_iccamera_eos_props_dump(), and to
 * drive any setting use gp_iccamera_list_config/get_config/set_config. Background thread. */
int gp_iccamera_capabilities(gp_iccamera *, char *out, int outlen);

/* Watch the Canon EOS event queue for `ms` milliseconds and report which device properties
 * changed — by CODE, so it catches even UNDEF (undecoded) properties that gp_iccamera_eos_props_dump
 * can't show. Change a setting on the camera during the window; whatever fires is the property
 * behind it. Writes a summary to `out`:
 *     watched <ms> ms: <k> distinct prop code(s)
 *     PROP d1c2  x3            (each changed code + how many times it fired, sorted)
 *     RAW <code> <hex…>        (any unhandled event carrying a raw payload, verbatim)
 * Returns the number of DISTINCT property codes seen (0 = nothing changed), or negative on error.
 * Blocks for ~`ms`; call on the BACKGROUND thread. */
int gp_iccamera_watch_prop_changes(gp_iccamera *, int ms, char *out, int outlen);

/* Movie recording size (resolution + fps). Reads the body's movie-size device property (per the
 * gp_canon_moviesize table — 0xD20D on the EOS R50) and resolves it to dimensions/fps/label.
 *   *code                  the raw property value (always set on success)
 *   *width/*height/*fps_x100  0 when the code isn't mapped to a resolution yet (fps ×100)
 *   label                  camera-style label buffer, "" if unmapped (pass NULL/0 to skip)
 *   choices/nchoices       the camera's list of VALID movie sizes (packed values; pass NULL/0 to
 *                          skip). Decode each with gp_canon_moviesize_decode() to build a picker.
 * Returns 0 on success; -1 if the property isn't reported by this body; -2 if the body's
 * movie-size property is unknown (needs discovery, e.g. EOS R50 V); -3 on usage/other error.
 * Background thread. */
int gp_iccamera_get_movie_size(gp_iccamera *,
                               uint32_t *code, int *width, int *height, int *fps_x100,
                               char *label, int labellen,
                               uint32_t *choices, int choicecap, int *nchoices);

/* Select the movie recording size by `code` (the packed value from get_movie_size, or built with
 * gp_canon_moviesize_encode). Returns 0 on success; -2 if the body's movie-size property is unknown;
 * negative on error. A successful change also fires a PROP event, so a poll_events watcher sees it.
 * NOTE: some camera states silently reject the change — e.g. the R50 in HFR mode (FHD 100p) locks
 * the recording size and returns OK without changing. ALWAYS read back with get_movie_size to
 * confirm. Background thread. */
int gp_iccamera_set_movie_size(gp_iccamera *, uint32_t code);

/* Drain the Canon EOS event queue once — the EDSDK property/object/state events all arrive
 * on this single queue. Writes a newline-separated summary of what was seen to `out`:
 *   "PROP <hex>"                    a device property changed on the camera (drives "update")
 *   "OBJECT <handle> <fmt> <size>"  a new image/object was created
 *   "OBJECTREMOVED" | "STATUS <n>" | "FOCUS"
 * Returns the number of events (0 = nothing happened), or negative on error. Background thread. */
int gp_iccamera_poll_events(gp_iccamera *, char *out, int outlen);

/* Download one object (image) by handle — used to auto-pull a shot triggered on the camera
 * BODY when an OBJECT event arrives from poll_events. `fmt` and `size` come from that event
 * line (size 0 = unknown → one big read). On success returns 0, sets *outdata (free with
 * gp_iccamera_freebuf) + *outlen, writes the extension to `ext` and a trace to `status`.
 * Negative on failure. Background thread. */
int gp_iccamera_download(gp_iccamera *, uint32_t handle, uint32_t fmt, uint32_t size,
                         uint8_t **outdata, int *outlen, char *ext, int extlen,
                         char *status, int statuslen);

/* Keep the camera awake (EDSDK ExtendShutDownTimer) — call on a camera-status event. */
int gp_iccamera_keepalive(gp_iccamera *);

#ifdef __cplusplus
}
#endif

#endif /* GP_ICCAMERA_H */
