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

/* Canon EOS live view (streamed EVF frames).
 *   _start  → route the EVF to "PC" and enter live-view mode (call once). Writes a
 *             human trace to `status`. Returns 0 on success, negative on failure.
 *   _frame  → fetch ONE JPEG preview frame. Returns 0 and sets *outdata (malloc'd —
 *             free with gp_iccamera_freebuf) + *outlen on success; returns 1 (soft)
 *             when no frame is ready yet — just call again; negative on hard error.
 *   _stop   → leave live-view mode.
 * Call all three from a BACKGROUND thread, like the other gp_iccamera_* ops. */
int gp_iccamera_liveview_start(gp_iccamera *, char *status, int statuslen);
int gp_iccamera_liveview_frame(gp_iccamera *, uint8_t **outdata, int *outlen);
int gp_iccamera_liveview_stop (gp_iccamera *);

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
