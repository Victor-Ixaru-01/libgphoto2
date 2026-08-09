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

#ifdef __cplusplus
}
#endif

#endif /* GP_ICCAMERA_H */
