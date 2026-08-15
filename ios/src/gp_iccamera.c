/*
 * gp_iccamera.c — the ptp2 ⇆ ImageCaptureCore transport bridge (compiled into the framework).
 *
 * ptp2 runs a PTP transaction as: sendreq_func → (senddata_func | getdata_func | ø) → getresp_func,
 * moving data through a PTPDataHandler. ICCameraDevice.requestSendPTPCommand does a WHOLE
 * transaction in one async call (command + out-data → in-data + response). So we buffer the
 * command in sendreq, fire the callback at the phase that has all the data, and hand the
 * response back in getresp. The transaction-id is forged to what ptp2 expects, because
 * ImageCaptureCore owns the real PTP session and its transaction ids.
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>   /* usleep */
#include <time.h>     /* clock_gettime */
#if defined(HAVE_ICONV) && defined(HAVE_LANGINFO_H)
#  include <iconv.h>
#  include <langinfo.h>
#endif

#include <gphoto2/gphoto2-camera.h>
#include <gphoto2/gphoto2-context.h>

#include "ptp.h"
#include "ptp-private.h"
#include "gp_iccamera.h"
#include "gp_canon_moviesize.h"

struct gp_iccamera {
	PTPData        ptpdata;   /* MUST be first: ptp2 reads (PTPData*)params->data */
	Camera        *camera;
	GPContext     *context;

	gp_iccamera_transact_cb cb;
	void          *cbctx;

	/* per-transaction scratch */
	unsigned char  cmd[12 + 5 * 4];
	int            cmdlen;
	int            fired;
	unsigned char *indata;
	int            indatalen;
	unsigned char  resp[64];
	int            resplen;

	/* EVF coordinate system (record type 14), captured per live-view frame — the space
	 * AF/zoom coordinates live in (e.g. 6000x4000 on the R50). Used to map screen taps. */
	uint32_t       evf_coord_w, evf_coord_h;

	/* EVF frame rect (record type 13) — the movable zoom/AF box; candidate AF-reticle source
	 * when FocusInfoEx (0xD1D3) isn't reported. Coordinates in the type-14 space. */
	int32_t        evf_frame_x, evf_frame_y, evf_frame_w, evf_frame_h;
	int            evf_frame_valid;

	/* Roll/pitch level from EVF record type 16 (offsets 8/12): roll x100, pitch x100,
	 * each mod 36000 (>18000 means negative). */
	uint32_t       evf_level_a, evf_level_b;
	int            evf_level_valid;
};

/* --- little-endian helpers --- */
static void     put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void     put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static uint16_t get16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

/* Fire one requestSendPTPCommand via the app callback. Stashes response + in-data. */
static uint16_t
icc_fire(struct gp_iccamera *icc, const unsigned char *out, int outlen)
{
	unsigned char *in = NULL;
	int inlen = 0, rc;

	if (icc->indata) { free(icc->indata); icc->indata = NULL; icc->indatalen = 0; }
	icc->resplen = (int)sizeof(icc->resp);
	rc = icc->cb(icc->cbctx, icc->cmd, icc->cmdlen, out, outlen, &in, &inlen, icc->resp, &icc->resplen);
	if (rc != 0) {
		if (in) free(in);
		return PTP_ERROR_IO;
	}
	icc->indata = in;
	icc->indatalen = inlen;
	icc->fired = 1;
	return PTP_RC_OK;
}

/* --- PTPParams transport vtable --- */

static uint16_t
icc_sendreq(PTPParams *params, PTPContainer *req, int dataphase)
{
	struct gp_iccamera *icc = (struct gp_iccamera *)params->data;
	uint32_t p[5];
	int np = req->Nparam, i;
	(void)dataphase;
	if (np > 5) np = 5;
	icc->cmdlen = 12 + 4 * np;
	put32(icc->cmd + 0, (uint32_t)icc->cmdlen);
	put16(icc->cmd + 4, 1);            /* PTP command block */
	put16(icc->cmd + 6, req->Code);
	put32(icc->cmd + 8, 0);            /* transaction id — ImageCaptureCore rewrites it */
	p[0] = req->Param1; p[1] = req->Param2; p[2] = req->Param3; p[3] = req->Param4; p[4] = req->Param5;
	for (i = 0; i < np; i++) put32(icc->cmd + 12 + 4 * i, p[i]);
	icc->fired = 0;
	return PTP_RC_OK;
}

static uint16_t
icc_senddata(PTPParams *params, PTPContainer *ptp, uint64_t size, PTPDataHandler *handler)
{
	struct gp_iccamera *icc = (struct gp_iccamera *)params->data;
	unsigned char *buf = NULL;
	unsigned long got = 0;
	uint16_t r;
	(void)ptp;
	if (size > 0) {
		buf = malloc((size_t)size);
		if (!buf) return PTP_ERROR_IO;
		r = handler->getfunc(params, handler->priv, (unsigned long)size, buf, &got);
		if (r != PTP_RC_OK) { free(buf); return r; }
	}
	r = icc_fire(icc, buf, (int)got);
	if (buf) free(buf);
	return r;
}

static uint16_t
icc_getdata(PTPParams *params, PTPContainer *ptp, PTPDataHandler *handler)
{
	struct gp_iccamera *icc = (struct gp_iccamera *)params->data;
	uint16_t r;
	(void)ptp;
	r = icc_fire(icc, NULL, 0);
	if (r != PTP_RC_OK) return r;
	if (icc->indata && icc->indatalen > 0)
		handler->putfunc(params, handler->priv, (unsigned long)icc->indatalen, icc->indata);
	return PTP_RC_OK;
}

static uint16_t
icc_getresp(PTPParams *params, PTPContainer *resp)
{
	struct gp_iccamera *icc = (struct gp_iccamera *)params->data;
	uint16_t code;
	int np, off = 12;

	if (!icc->fired) {                      /* NODATA op: fire now (command only) */
		uint16_t r = icc_fire(icc, NULL, 0);
		if (r != PTP_RC_OK) return r;
	}
	if (icc->resplen < 8) return PTP_ERROR_IO;

	code = get16(icc->resp + 6);
	resp->Code = code;
	resp->SessionID = params->session_id;
	resp->Transaction_ID = params->transaction_id - 1;   /* forge: ICC owns the real ids */
	np = (icc->resplen - 12) / 4;
	if (np < 0) np = 0;
	if (np > 5) np = 5;
	resp->Nparam = (uint8_t)np;
	resp->Param1 = np > 0 ? get32(icc->resp + off + 0)  : 0;
	resp->Param2 = np > 1 ? get32(icc->resp + off + 4)  : 0;
	resp->Param3 = np > 2 ? get32(icc->resp + off + 8)  : 0;
	resp->Param4 = np > 3 ? get32(icc->resp + off + 12) : 0;
	resp->Param5 = np > 4 ? get32(icc->resp + off + 16) : 0;
	icc->fired = 0;
	return code;   /* 0x2001 == PTP_RC_OK on success, else the camera's error code */
}

/* Events: ImageCaptureCore drains the interrupt endpoint itself; EOS events are fetched
 * as commands (ptp_canon_eos_getevent). Stub the async event channel as "no event". */
static uint16_t icc_event_stub(PTPParams *params, PTPContainer *event) { (void)params; (void)event; return PTP_ERROR_TIMEOUT; }
static uint16_t icc_cancel_stub(PTPParams *params, uint32_t tid)        { (void)params; (void)tid;  return PTP_RC_OK; }

static void icc_debug(void *data, const char *fmt, va_list args) { (void)data; (void)fmt; (void)args; }
static void icc_error(void *data, const char *fmt, va_list args) { (void)data; vfprintf(stderr, fmt, args); fputc('\n', stderr); }

/* --- lifecycle --- */

gp_iccamera *
gp_iccamera_new(gp_iccamera_transact_cb cb, void *ctx)
{
	struct gp_iccamera *icc;
	PTPParams *params;

	if (!cb) return NULL;
	icc = calloc(1, sizeof(*icc));
	if (!icc) return NULL;
	icc->cb = cb;
	icc->cbctx = ctx;

	if (gp_camera_new(&icc->camera) < GP_OK) { free(icc); return NULL; }
	icc->context = gp_context_new();
	icc->camera->pl = calloc(1, sizeof(CameraPrivateLibrary));
	if (!icc->camera->pl) {
		gp_context_unref(icc->context);
		gp_camera_free(icc->camera);
		free(icc);
		return NULL;
	}
	icc->ptpdata.camera = icc->camera;
	icc->ptpdata.context = icc->context;

	params = &icc->camera->pl->params;
	params->data              = icc;               /* == &icc->ptpdata (first member) */
	params->byteorder         = PTP_DL_LE;
	params->maxpacketsize     = 512;
	params->sendreq_func      = icc_sendreq;
	params->senddata_func     = icc_senddata;
	params->getdata_func      = icc_getdata;
	params->getresp_func      = icc_getresp;
	params->event_check       = icc_event_stub;
	params->event_check_queue = icc_event_stub;
	params->event_wait        = icc_event_stub;
	params->cancelreq_func    = icc_cancel_stub;
	params->debug_func        = icc_debug;
	params->error_func        = icc_error;

#if defined(HAVE_ICONV) && defined(HAVE_LANGINFO_H)
	{
		char *curloc = nl_langinfo(CODESET);
		if (!curloc) curloc = "UTF-8";
		params->cd_ucs2_to_locale = iconv_open(curloc, "UCS-2LE");
		params->cd_locale_to_ucs2 = iconv_open("UCS-2LE", curloc);
	}
#endif

	/* ImageCaptureCore already opened the PTP session — do NOT ptp_opensession().
	 * Read device info so the ptp2 vendor logic knows what it's talking to. */
	if (ptp_getdeviceinfo(params, &params->deviceinfo) != PTP_RC_OK) {
		gp_iccamera_free(icc);
		return NULL;
	}
	/* Newer Canons report the MTP/Microsoft vendor extension (0x06) over this interface.
	 * Restore the Canon EOS extension id so ptp2's Canon-specific capture/config/event
	 * code paths activate — the same fixup libgphoto2's own camera_init performs. */
	if (params->deviceinfo.VendorExtensionID == PTP_VENDOR_MICROSOFT &&
	    params->deviceinfo.Manufacturer &&
	    strstr(params->deviceinfo.Manufacturer, "Canon"))
		params->deviceinfo.VendorExtensionID = PTP_VENDOR_CANON;

	/* Canon EOS: enter remote mode + enable events, then drain the initial property dump
	 * so the config get-functions can read current values from params->canon_props. */
	if (params->deviceinfo.VendorExtensionID == PTP_VENDOR_CANON) {
		int i;
		ptp_canon_eos_setremotemode(params, 1);
		ptp_canon_eos_seteventmode(params, 1);
		params->eos_captureenabled = 1;
		for (i = 0; i < 3; i++)
			ptp_check_eos_events(params);
	}
	return icc;
}

void
gp_iccamera_free(gp_iccamera *icc)
{
	PTPParams *params;
	if (!icc) return;
	params = &icc->camera->pl->params;
	if (params->inliveview) { params->inliveview = 0; ptp_canon_eos_end_viewfinder(params); }
#if defined(HAVE_ICONV) && defined(HAVE_LANGINFO_H)
	if (params->cd_ucs2_to_locale != (iconv_t)-1) iconv_close(params->cd_ucs2_to_locale);
	if (params->cd_locale_to_ucs2 != (iconv_t)-1) iconv_close(params->cd_locale_to_ucs2);
#endif
	ptp_free_deviceinfo(&params->deviceinfo);
	if (icc->indata) free(icc->indata);
	free(icc->camera->pl);
	icc->camera->pl = NULL;
	gp_camera_free(icc->camera);
	gp_context_unref(icc->context);
	free(icc);
}

/* --- proof operations --- */

int
gp_iccamera_get_deviceinfo(gp_iccamera *icc, char *out, int outlen)
{
	PTPParams *params = &icc->camera->pl->params;
	PTPDeviceInfo *di = &params->deviceinfo;
	snprintf(out, (size_t)outlen,
	         "%s %s | ext 0x%08x (%s) | %u ops, %u props, %u events",
	         di->Manufacturer ? di->Manufacturer : "?",
	         di->Model ? di->Model : "?",
	         di->VendorExtensionID,
	         di->VendorExtensionID == PTP_VENDOR_CANON ? "Canon EOS" : "other",
	         di->Operations_len, di->DeviceProps_len, di->Events_len);
	return 0;
}

int
gp_iccamera_eos_handshake(gp_iccamera *icc, char *out, int outlen)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t r1 = ptp_canon_eos_setremotemode(params, 1);
	uint16_t r2 = ptp_canon_eos_seteventmode(params, 1);
	snprintf(out, (size_t)outlen, "EOS SetRemoteMode=0x%04x, SetEventMode=0x%04x", r1, r2);
	return (r1 == PTP_RC_OK && r2 == PTP_RC_OK) ? 0 : -1;
}

void gp_iccamera_freebuf(uint8_t *p) { if (p) free(p); }

/* --- config tree (drives config.c's full widget tree over our transport) --- */

int
gp_iccamera_list_config(gp_iccamera *icc, char *out, int outlen)
{
	CameraList *list = NULL;
	char *sp = out; int sl = outlen, i, n, ret;
	if (outlen) out[0] = 0;
	if (gp_list_new(&list) < GP_OK) return -1;
	ret = camera_list_config(icc->camera, list, icc->context);
	if (ret != GP_OK) { gp_list_free(list); return ret; }
	n = gp_list_count(list);
	for (i = 0; i < n; i++) {
		const char *name = NULL;
		gp_list_get_name(list, i, &name);
		int w = snprintf(sp, (size_t)sl, "%s\n", name ? name : "?");
		if (w > 0 && w < sl) { sp += w; sl -= w; }
	}
	gp_list_free(list);
	return 0;
}

int
gp_iccamera_get_config(gp_iccamera *icc, const char *name,
                       char *value, int vlen, char *choices, int clen)
{
	CameraWidget *w = NULL;
	CameraWidgetType type;
	int ret, i, n;

	if (value && vlen) value[0] = 0;
	if (choices && clen) choices[0] = 0;

	ret = camera_get_single_config(icc->camera, name, &w, icc->context);
	if (ret != GP_OK || !w) { if (value && vlen) snprintf(value, (size_t)vlen, "(unavailable)"); return ret ? ret : -1; }

	gp_widget_get_type(w, &type);
	switch (type) {
	case GP_WIDGET_RADIO:
	case GP_WIDGET_MENU:
	case GP_WIDGET_TEXT: {
		char *val = NULL;
		char *cp = choices; int cl = clen;
		gp_widget_get_value(w, &val);
		if (value && vlen && val) snprintf(value, (size_t)vlen, "%s", val);
		n = gp_widget_count_choices(w);
		for (i = 0; i < n && cl > 1; i++) {
			const char *ch = NULL;
			gp_widget_get_choice(w, i, &ch);
			int cw = snprintf(cp, (size_t)cl, "%s\n", ch ? ch : "");
			if (cw > 0 && cw < cl) { cp += cw; cl -= cw; }
		}
		break;
	}
	case GP_WIDGET_TOGGLE: {
		int val = 0;
		gp_widget_get_value(w, &val);
		if (value && vlen) snprintf(value, (size_t)vlen, "%d", val);
		if (choices && clen) snprintf(choices, (size_t)clen, "0\n1\n");
		break;
	}
	case GP_WIDGET_RANGE: {
		float val = 0, mn = 0, mx = 0, st = 0;
		gp_widget_get_value(w, &val);
		gp_widget_get_range(w, &mn, &mx, &st);
		if (value && vlen) snprintf(value, (size_t)vlen, "%g", val);
		if (choices && clen) snprintf(choices, (size_t)clen, "%g..%g/%g", mn, mx, st);
		break;
	}
	default:
		if (value && vlen) snprintf(value, (size_t)vlen, "(type %d)", (int)type);
		break;
	}
	gp_widget_free(w);
	return 0;
}

int
gp_iccamera_set_config(gp_iccamera *icc, const char *name, const char *value)
{
	CameraWidget *w = NULL;
	CameraWidgetType type;
	int ret;

	ret = camera_get_single_config(icc->camera, name, &w, icc->context);
	if (ret != GP_OK || !w) return ret ? ret : -1;

	gp_widget_get_type(w, &type);
	switch (type) {
	case GP_WIDGET_RADIO:
	case GP_WIDGET_MENU:
	case GP_WIDGET_TEXT:
		gp_widget_set_value(w, value);
		break;
	case GP_WIDGET_TOGGLE: {
		int v = atoi(value);
		gp_widget_set_value(w, &v);
		break;
	}
	case GP_WIDGET_RANGE: {
		float f = (float)atof(value);
		gp_widget_set_value(w, &f);
		break;
	}
	default:
		gp_widget_free(w);
		return -2;
	}
	gp_widget_set_changed(w, 1);
	ret = camera_set_single_config(icc->camera, name, w, icc->context);
	gp_widget_free(w);
	return ret;
}

/* Ported from ptp2's camera_trigger_canon_eos_capture (non-M path), using only exported
 * ptp_* functions so it runs over our transport. First iteration — logs each step so the
 * real device behaviour can be read from `status`. */
int
gp_iccamera_capture(gp_iccamera *icc, uint8_t **outdata, int *outlen,
                    char *ext, int extlen, char *status, int statuslen)
{
	PTPParams        *params = &icc->camera->pl->params;
	PTPCanonEOSEvent  ev;
	PTPObjectInfo     oi;
	uint16_t          r;
	int               i, tries;
	char             *sp = status; int sl = statuslen;
#define SLOG(...) do { int _n = snprintf(sp, (size_t)sl, __VA_ARGS__); if (_n > 0 && _n < sl) { sp += _n; sl -= _n; } } while (0)

	*outdata = NULL; *outlen = 0;
	memset(&oi, 0, sizeof(oi));
	if (ext && extlen) snprintf(ext, (size_t)extlen, "jpg");

	/* live view and the shutter share the EVF; leave live view before releasing */
	if (params->inliveview) { params->inliveview = 0; ptp_canon_eos_end_viewfinder(params); }

	/* 1. ensure EOS remote mode (idempotent) */
	ptp_canon_eos_setremotemode(params, 1);
	ptp_canon_eos_seteventmode(params, 1);
	params->eos_captureenabled = 1;

	/* 2. drain stale events so the camera does not report busy */
	ptp_check_eos_events(params);
	while (ptp_get_one_eos_event(params, &ev)) ptp_free_eos_event(&ev);

	/* 3. press sequence: half-press (AF), settle, full-press (retry on DeviceBusy), release */
	r = ptp_canon_eos_remotereleaseon(params, 1, 0);
	if (r != PTP_RC_OK) { SLOG("half-press failed 0x%04x", r); return -1; }
	usleep(400 * 1000);
	ptp_check_eos_events(params);
	while (ptp_get_one_eos_event(params, &ev)) ptp_free_eos_event(&ev);

	for (tries = 0; ; tries++) {
		r = ptp_canon_eos_remotereleaseon(params, 2, 0);
		if (r == 0x2019 /* DeviceBusy */ && tries < 5) { usleep(700 * 1000); continue; }
		break;
	}
	if (r != PTP_RC_OK) {
		SLOG("full-press failed 0x%04x", r);
		ptp_canon_eos_remotereleaseoff(params, 1);
		return -2;
	}
	ptp_canon_eos_remotereleaseoff(params, 2);
	ptp_canon_eos_remotereleaseoff(params, 1);
	SLOG("shutter fired; ");

	/* 4. wait (~12s) for the ObjectAdded event */
	for (i = 0; i < 60 && !oi.Handle; i++) {
		ptp_check_eos_events(params);
		while (ptp_get_one_eos_event(params, &ev)) {
			if (ev.type == PTP_EOSEvent_ObjectAdded && ev.u.object.Handle) {
				oi = ev.u.object;   /* take ownership of this event's ObjectInfo */
				break;
			}
			ptp_free_eos_event(&ev);
		}
		if (oi.Handle) break;
		usleep(200 * 1000);
	}
	if (!oi.Handle) { SLOG("no ObjectAdded event within timeout (is capturetarget the card?)"); return -3; }

	SLOG("obj 0x%08x storage 0x%08x fmt 0x%04x size %llu; ",
	     oi.Handle, oi.StorageID, oi.ObjectFormat, (unsigned long long)oi.ObjectSize);
	if (ext && extlen) {
		if (oi.ObjectFormat == PTP_OFC_CANON_CR3) snprintf(ext, (size_t)extlen, "cr3");
		else if (oi.ObjectFormat == PTP_OFC_CANON_CRW || oi.ObjectFormat == PTP_OFC_CANON_CRW3) snprintf(ext, (size_t)extlen, "cr2");
		else snprintf(ext, (size_t)extlen, "jpg");
	}

	/* 5. download in ≤1MB chunks (the EOS R dislikes large single reads) */
	{
		uint32_t total = (uint32_t)oi.ObjectSize, offset = 0;
		uint8_t *buf = NULL;

		if (total == 0) {   /* size unknown: one big read, use whatever comes back */
			unsigned char *chunk = NULL; uint32_t got = 0x0fffffff;
			r = ptp_getpartialobject(params, oi.Handle, 0, got, &chunk, &got);
			if (r != PTP_RC_OK || !chunk) { SLOG("getobject failed 0x%04x", r); if (oi.Filename) free(oi.Filename); return -4; }
			ptp_canon_eos_transfercomplete(params, oi.Handle);
			*outdata = chunk; *outlen = (int)got;
			SLOG("downloaded %u bytes", got);
			if (oi.Filename) free(oi.Filename);
			return 0;
		}

		buf = malloc(total);
		if (!buf) { SLOG("out of memory (%u)", total); if (oi.Filename) free(oi.Filename); return -5; }
		while (offset < total) {
			unsigned char *chunk = NULL;
			uint32_t want = total - offset;
			if (want > (1u << 20)) want = (1u << 20);
			r = ptp_getpartialobject(params, oi.Handle, offset, want, &chunk, &want);
			if (r != PTP_RC_OK || !chunk) { SLOG("getpartialobject@%u failed 0x%04x", offset, r); free(buf); if (oi.Filename) free(oi.Filename); return -4; }
			memcpy(buf + offset, chunk, want);
			free(chunk);
			if (want == 0) break;
			offset += want;
		}
		ptp_canon_eos_transfercomplete(params, oi.Handle);
		*outdata = buf; *outlen = (int)offset;
		SLOG("downloaded %d bytes", (int)offset);
	}
	if (oi.Filename) free(oi.Filename);
	return 0;
#undef SLOG
}

/* --- live view (ported from library.c's Canon EOS GET_VIEWFINDER path) --- */

int
gp_iccamera_liveview_start(gp_iccamera *icc, char *status, int statuslen)
{
	PTPParams          *params = &icc->camera->pl->params;
	PTPDevicePropDesc   dpd;
	PTPPropValue        val;
	uint16_t            r;
	char               *sp = status; int sl = statuslen;
#define SLOG(...) do { int _n = snprintf(sp, (size_t)sl, __VA_ARGS__); if (_n > 0 && _n < sl) { sp += _n; sl -= _n; } } while (0)

	if (status && statuslen) status[0] = 0;

	/* EVF must be on; only set when off (setting it every time costs seconds). */
	memset(&dpd, 0, sizeof(dpd));
	r = ptp_canon_eos_getdevicepropdesc(params, PTP_DPC_CANON_EOS_EVFMode, &dpd);
	if (r == PTP_RC_OK && dpd.CurrentValue.u16 != 1) {
		val.u16 = 1;
		r = ptp_canon_eos_setdevicepropvalue(params, PTP_DPC_CANON_EOS_EVFMode, &val, PTP_DTC_UINT16);
		if (r != PTP_RC_OK && r != PTP_RC_DeviceBusy) { SLOG("EVFMode=1 failed 0x%04x", r); ptp_free_devicepropdesc(&dpd); return -1; }
	}
	ptp_free_devicepropdesc(&dpd);

	/* Route live view to BOTH the camera's own screen and the host. The value is a
	 * bitmask (bit0 TFT = rear screen, bit1 PC, bit2/3 MOBILE); config.c enumerates
	 * 3 as "TFT + PC". PC alone (2) blanks the camera screen, so we OR in the TFT bit.
	 * Only write when TFT+PC aren't already both up, to avoid a re-set stall. */
	memset(&dpd, 0, sizeof(dpd));
	r = ptp_canon_eos_getdevicepropdesc(params, PTP_DPC_CANON_EOS_EVFOutputDevice, &dpd);
	if (r == PTP_RC_OK && (dpd.CurrentValue.u32 & 3u) != 3u) {
		val.u32 = dpd.CurrentValue.u32 | 3u;   /* TFT + PC (keep any MOBILE bits already set) */
		r = ptp_canon_eos_setdevicepropvalue(params, PTP_DPC_CANON_EOS_EVFOutputDevice, &val, PTP_DTC_UINT32);
		if (r != PTP_RC_OK) { SLOG("EVFOutputDevice=TFT+PC failed 0x%04x", r); ptp_free_devicepropdesc(&dpd); return -2; }
	}
	ptp_free_devicepropdesc(&dpd);

	ptp_canon_eos_keepdeviceon(params);   /* else the body auto-shuts-down mid-stream */
	params->inliveview = 1;
	SLOG("live view on");
	return 0;
#undef SLOG
}

int
gp_iccamera_liveview_frame(gp_iccamera *icc, uint8_t **outdata, int *outlen,
                           uint32_t *hist, int histcap, int *histn)
{
	PTPParams     *params = &icc->camera->pl->params;
	unsigned char *data = NULL, *xdata;
	unsigned int   size = 0;
	uint16_t       r;
	int            tries, hn = 0;
	uint8_t       *jpg = NULL; uint32_t jlen = 0;

	*outdata = NULL; *outlen = 0;
	if (histn) *histn = 0;

	/* one event poll per frame (do NOT drain the queue — library.c does the same) */
	ptp_check_eos_events(params);

	/* "not ready" (0xA102) right after enabling is normal; retry briefly (~300ms). */
	for (tries = 0; ; tries++) {
		r = ptp_canon_eos_get_viewfinder_image(params, &data, &size);
		if ((r == PTP_RC_CANON_EOS_ObjectNotReady || r == PTP_RC_DeviceBusy) && tries < 6) {
			usleep(50 * 1000);
			continue;
		}
		break;
	}
	if (r == PTP_RC_CANON_EOS_ObjectNotReady || r == PTP_RC_DeviceBusy) return 1;  /* soft: try next tick */
	if (r != PTP_RC_OK) return -1;
	if (!data || size < 8) { if (data) free(data); return 1; }

	/* The buffer is a sequence of records: [u32 len][u32 type][payload len-8].
	 * type 1/11 = JPEG preview, 9 = movie-mode frame (first one wins). The histogram
	 * rides in the same buffer as 256-bucket uint32 channels (1024 bytes each, EDSDK
	 * layout; a combined 4096-byte record is split into 4) — identified by size, since
	 * ptp2 doesn't name the record type. See gp_iccamera_liveview_inspect for discovery. */
	xdata = data;
	while ((size_t)(xdata - data) + 8 <= size) {
		uint32_t len  = get32(xdata);
		uint32_t type = get32(xdata + 4);
		uint32_t plen;
		const uint8_t *payload;
		if (len < 8 || len > size - (uint32_t)(xdata - data)) break;   /* malformed */
		plen = len - 8;
		payload = xdata + 8;

		if (!jpg && (type == 1 || type == 9 || type == 11)) {
			jpg = malloc(plen);
			if (jpg) { memcpy(jpg, payload, plen); jlen = plen; }
		} else if (type == 17 && plen >= 1024) {   /* histogram: 4 x 256 x u32 (RGBY) */
			if (hist && histcap > 0) {
				uint32_t nvals = plen / 4, k;
				for (k = 0; k < nvals && hn < histcap; k++)
					hist[hn++] = get32(payload + k * 4);
			}
		} else if (type == 14 && plen >= 8) {       /* coordinate system size (WxH) */
			icc->evf_coord_w = get32(payload);
			icc->evf_coord_h = get32(payload + 4);
		} else if (type == 13 && plen >= 16) {      /* movable zoom/AF frame rect */
			icc->evf_frame_x = (int32_t)get32(payload);
			icc->evf_frame_y = (int32_t)get32(payload + 4);
			icc->evf_frame_w = (int32_t)get32(payload + 8);
			icc->evf_frame_h = (int32_t)get32(payload + 12);
			icc->evf_frame_valid = 1;
		} else if (type == 16 && plen >= 16) {      /* roll/pitch level: [2][0][roll*100][pitch*100] */
			icc->evf_level_a = get32(payload + 8);   /* roll  x100 (mod 36000) */
			icc->evf_level_b = get32(payload + 12);  /* pitch x100 (mod 36000) */
			icc->evf_level_valid = 1;
		}
		xdata += len;
	}
	free(data);
	if (histn) *histn = hn;

	if (jpg && jlen > 0) { *outdata = jpg; *outlen = (int)jlen; return 0; }
	if (jpg) free(jpg);
	return 1;   /* no JPEG this round (histn may still be set) */
}

int
gp_iccamera_liveview_inspect(gp_iccamera *icc, char *out, int outlen)
{
	PTPParams     *params = &icc->camera->pl->params;
	unsigned char *data = NULL, *xdata;
	unsigned int   size = 0;
	uint16_t       r;
	int            n = 0;
	char *sp = out; int sl = outlen;

	if (out && outlen) out[0] = 0;
	ptp_check_eos_events(params);
	r = ptp_canon_eos_get_viewfinder_image(params, &data, &size);
	if (r != PTP_RC_OK || !data) { if (data) free(data); return (r == PTP_RC_OK) ? 0 : -1; }

	xdata = data;
	while ((size_t)(xdata - data) + 8 <= size) {
		uint32_t len  = get32(xdata);
		uint32_t type = get32(xdata + 4);
		uint32_t plen, show, b;
		int w;
		if (len < 8 || len > size - (uint32_t)(xdata - data)) break;
		plen = len - 8;
		w = snprintf(sp, (size_t)sl, "type=%u len=%u ", type, plen);
		if (w > 0 && w < sl) { sp += w; sl -= w; }
		/* dump up to 64 bytes (deeper than ptp_bytes2str's 16) so values buried in the larger
		 * records — e.g. roll/pitch — are visible; JPEG/histogram are just truncated harmlessly */
		show = plen < 64 ? plen : 64;
		for (b = 0; b < show; b++) {
			w = snprintf(sp, (size_t)sl, "%02x ", ((const uint8_t *)(xdata + 8))[b]);
			if (w > 0 && w < sl) { sp += w; sl -= w; } else break;
		}
		w = snprintf(sp, (size_t)sl, "\n");
		if (w > 0 && w < sl) { sp += w; sl -= w; }
		xdata += len;
		n++;
	}
	free(data);
	return n;
}

int
gp_iccamera_liveview_stop(gp_iccamera *icc)
{
	PTPParams *params = &icc->camera->pl->params;
	if (!params->inliveview) return 0;
	params->inliveview = 0;
	return ptp_canon_eos_end_viewfinder(params) == PTP_RC_OK ? 0 : -1;
}

/* EVF coordinate-system size (record type 14), captured by the last liveview_frame. 0 until a
 * frame has been fetched. This is the space AF frames / touch-AF points are expressed in. */
int
gp_iccamera_get_coordsize(gp_iccamera *icc, uint32_t *w, uint32_t *h)
{
	if (w) *w = icc->evf_coord_w;
	if (h) *h = icc->evf_coord_h;
	return (icc->evf_coord_w && icc->evf_coord_h) ? 0 : -1;
}

/* EVF frame rect (record type 13) from the last live-view frame — the movable zoom/AF box, in
 * the type-14 coordinate space. Candidate AF reticle when FocusInfoEx isn't reported. 0/-1. */
int
gp_iccamera_get_evf_frame(gp_iccamera *icc, int *x, int *y, int *w, int *h)
{
	if (!icc->evf_frame_valid) return -1;
	if (x) *x = icc->evf_frame_x;
	if (y) *y = icc->evf_frame_y;
	if (w) *w = icc->evf_frame_w;
	if (h) *h = icc->evf_frame_h;
	return 0;
}

/* Roll/pitch level from the last live-view frame (EVF record type 16): *a = roll x100,
 * *b = pitch x100 (each mod 36000; >18000 = negative). Returns 0 with values, or -1. */
int
gp_iccamera_get_level(gp_iccamera *icc, uint32_t *a, uint32_t *b)
{
	if (!icc->evf_level_valid) return -1;
	if (a) *a = icc->evf_level_a;
	if (b) *b = icc->evf_level_b;
	return 0;
}

/* Normalize a raw level angle (x100, mod 36000) to signed centidegrees in (-18000, 18000]. */
static int32_t
icc_level_signed(uint32_t raw)
{
	int32_t v = (int32_t)(raw % 36000);
	return v > 18000 ? v - 36000 : v;
}

/* Portrait/landscape from the roll of the last live-view frame's electronic level (record type
 * 16). See gp_iccamera.h for the reliability caveat and the CW/CCW sign convention. */
int
gp_iccamera_get_orientation(gp_iccamera *icc, int *roll_deg, int *pitch_deg)
{
	int32_t roll, pitch, aroll;

	if (roll_deg)  *roll_deg  = 0;
	if (pitch_deg) *pitch_deg = 0;
	if (!icc->evf_level_valid) return GP_ICCAMERA_ORIENTATION_UNKNOWN;

	roll  = icc_level_signed(icc->evf_level_a);
	pitch = icc_level_signed(icc->evf_level_b);
	/* centidegrees → whole degrees, rounded to nearest */
	if (roll_deg)  *roll_deg  = (roll  + (roll  >= 0 ? 50 : -50)) / 100;
	if (pitch_deg) *pitch_deg = (pitch + (pitch >= 0 ? 50 : -50)) / 100;

	aroll = roll < 0 ? -roll : roll;                            /* |roll| in centidegrees */
	if (aroll <=  4500) return GP_ICCAMERA_ORIENTATION_LANDSCAPE;          /* within 45° of level */
	if (aroll >= 13500) return GP_ICCAMERA_ORIENTATION_LANDSCAPE_INVERTED; /* within 45° of 180°  */
	return roll > 0 ? GP_ICCAMERA_ORIENTATION_PORTRAIT_CW
	                : GP_ICCAMERA_ORIENTATION_PORTRAIT_CCW;
}

/* Read the selected AF reticles directly from FocusInfoEx (0xD1D3), bypassing config.c's
 * widget gating (which needs the prop to be advertised). Prompts + drains, then returns the
 * parsed "sizeX,sizeY,size2X,size2Y;{x,y,w,h},…" string. Returns 0 (string in `out`, may be
 * empty) or -1 if the camera hasn't reported FocusInfoEx yet (needs live view / AF active). */
int
gp_iccamera_get_focusinfo(gp_iccamera *icc, char *out, int outlen)
{
	PTPParams        *params = &icc->camera->pl->params;
	PTPDevicePropDesc dpd;

	if (out && outlen) out[0] = 0;
	ptp_canon_eos_requestdevicepropvalue(params, PTP_DPC_CANON_EOS_FocusInfoEx);
	ptp_check_eos_events(params);
	memset(&dpd, 0, sizeof(dpd));
	if (ptp_canon_eos_getdevicepropdesc(params, PTP_DPC_CANON_EOS_FocusInfoEx, &dpd) != PTP_RC_OK)
		return -1;
	if (dpd.DataType == PTP_DTC_STR && dpd.CurrentValue.str && out && outlen)
		snprintf(out, (size_t)outlen, "%s", dpd.CurrentValue.str);
	ptp_free_devicepropdesc(&dpd);
	return 0;
}

/* Touch-AF: set the live-view AF frame to a point in the EVF coordinate space (0x915A, sends
 * data). EXPERIMENTAL — ptp2 doesn't define this op's payload; first attempt sends the point as
 * two LE uint32 (x,y). The PTP response code is written to `status` so the format can be
 * verified/corrected on hardware. */
int
gp_iccamera_set_af_frame(gp_iccamera *icc, int x, int y, char *status, int statuslen)
{
	PTPParams    *params = &icc->camera->pl->params;
	PTPContainer  ptp;
	unsigned char buf[8], *data = buf;
	uint16_t      r;

	put32(buf + 0, (uint32_t)x);
	put32(buf + 4, (uint32_t)y);
	memset(&ptp, 0, sizeof(ptp));
	ptp.Code = PTP_OC_CANON_EOS_SetLiveAfFrame;
	ptp.Nparam = 0;
	r = ptp_transaction(params, &ptp, PTP_DP_SENDDATA, sizeof(buf), &data, NULL);
	if (status && statuslen) snprintf(status, (size_t)statuslen, "SetLiveAfFrame(%d,%d) → 0x%04x", x, y, r);
	return r == PTP_RC_OK ? 0 : -1;
}

/* --- Canon EOS commands (EDSDK-equivalent). See EDSDK-CAPABILITY-MAP.md. --- */

int
gp_iccamera_af(gp_iccamera *icc, int on)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t r = on ? ptp_canon_eos_afdrive(params) : ptp_canon_eos_afcancel(params);
	return r == PTP_RC_OK ? 0 : -1;
}

int
gp_iccamera_bulb(gp_iccamera *icc, int start)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t r;
	if (start) {
		ptp_canon_eos_setremotemode(params, 1);
		params->eos_captureenabled = 1;
		r = ptp_canon_eos_bulbstart(params);
	} else {
		r = ptp_canon_eos_bulbend(params);
	}
	return r == PTP_RC_OK ? 0 : -1;
}

int
gp_iccamera_drivelens(gp_iccamera *icc, int amount)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t r = ptp_canon_eos_drivelens(params, (uint32_t)amount);
	return r == PTP_RC_OK ? 0 : -1;
}

int
gp_iccamera_uilock(gp_iccamera *icc, int lock)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t r = lock ? ptp_canon_eos_setuilock(params) : ptp_canon_eos_resetuilock(params);
	return r == PTP_RC_OK ? 0 : -1;
}

int
gp_iccamera_evf_zoom(gp_iccamera *icc, int zoom)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t r = ptp_canon_eos_zoom(params, (uint32_t)zoom);
	return r == PTP_RC_OK ? 0 : -1;
}

int
gp_iccamera_evf_zoomposition(gp_iccamera *icc, int x, int y)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t r = ptp_canon_eos_zoomposition(params, (uint32_t)x, (uint32_t)y);
	return r == PTP_RC_OK ? 0 : -1;
}

int
gp_iccamera_dof_preview(gp_iccamera *icc, int on)
{
	PTPParams   *params = &icc->camera->pl->params;
	PTPPropValue val;
	val.u32 = on ? 1 : 0;
	uint16_t r = ptp_canon_eos_setdevicepropvalue(params, PTP_DPC_CANON_EOS_DepthOfFieldPreview, &val, PTP_DTC_UINT32);
	return r == PTP_RC_OK ? 0 : -1;
}

int
gp_iccamera_popupflash(gp_iccamera *icc)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t r = ptp_canon_eos_popupflash(params);
	return r == PTP_RC_OK ? 0 : -1;
}

int
gp_iccamera_rollpitch(gp_iccamera *icc, int on)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t r = ptp_canon_eos_setrequestrollingpitchinglevel(params, (uint32_t)(on ? 1 : 0));
	return r == PTP_RC_OK ? 0 : -1;
}

int
gp_iccamera_drive_powerzoom(gp_iccamera *icc, int mode)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t r = ptp_generic_no_data(params, PTP_OC_CANON_EOS_DrivePowerZoom, 1, (uint32_t)mode);
	return r == PTP_RC_OK ? 0 : -1;
}

/* --- Generic Canon EOS device-property access (scalar props by code) --- */

int
gp_iccamera_get_eosprop(gp_iccamera *icc, uint16_t code, uint32_t *value, uint32_t *datatype,
                        uint32_t *choices, int choicecap, int *nchoices)
{
	PTPParams        *params = &icc->camera->pl->params;
	PTPDevicePropDesc dpd;

	if (value)    *value = 0;
	if (datatype) *datatype = 0;
	if (nchoices) *nchoices = 0;

	ptp_canon_eos_requestdevicepropvalue(params, code);   /* best-effort refresh */
	ptp_check_eos_events(params);

	memset(&dpd, 0, sizeof(dpd));
	if (ptp_canon_eos_getdevicepropdesc(params, code, &dpd) != PTP_RC_OK)
		return -1;

	if (datatype) *datatype = dpd.DataType;
	if (value) {
		switch (dpd.DataType) {
		case PTP_DTC_INT8:  case PTP_DTC_UINT8:  *value = dpd.CurrentValue.u8;  break;
		case PTP_DTC_INT16: case PTP_DTC_UINT16: *value = dpd.CurrentValue.u16; break;
		default:                                  *value = dpd.CurrentValue.u32; break;
		}
	}
	if (choices && choicecap > 0 && (dpd.FormFlag & PTP_DPFF_Enumeration)) {
		int n = dpd.FORM.Enum.NumberOfValues, i, w = 0;
		for (i = 0; i < n && w < choicecap; i++) {
			switch (dpd.DataType) {
			case PTP_DTC_INT8:  case PTP_DTC_UINT8:  choices[w++] = dpd.FORM.Enum.SupportedValue[i].u8;  break;
			case PTP_DTC_INT16: case PTP_DTC_UINT16: choices[w++] = dpd.FORM.Enum.SupportedValue[i].u16; break;
			default:                                  choices[w++] = dpd.FORM.Enum.SupportedValue[i].u32; break;
			}
		}
		if (nchoices) *nchoices = w;
	}
	ptp_free_devicepropdesc(&dpd);
	return 0;
}

int
gp_iccamera_set_eosprop(gp_iccamera *icc, uint16_t code, uint32_t value)
{
	PTPParams        *params = &icc->camera->pl->params;
	PTPDevicePropDesc dpd;
	PTPPropValue      val;
	uint16_t          r, dt;

	memset(&dpd, 0, sizeof(dpd));
	if (ptp_canon_eos_getdevicepropdesc(params, code, &dpd) != PTP_RC_OK)
		return -1;
	dt = dpd.DataType;
	ptp_free_devicepropdesc(&dpd);

	memset(&val, 0, sizeof(val));
	switch (dt) {
	case PTP_DTC_INT8:  case PTP_DTC_UINT8:  val.u8  = (uint8_t)value;  break;
	case PTP_DTC_INT16: case PTP_DTC_UINT16: val.u16 = (uint16_t)value; break;
	default:                                  val.u32 = value;           break;
	}
	r = ptp_canon_eos_setdevicepropvalue(params, code, &val, dt);
	return r == PTP_RC_OK ? 0 : -1;
}

/* Read one cached EOS devprop as an int after a best-effort refresh. Returns -1 if the
 * property isn't exposed by this body (so callers can distinguish "photo" from "unknown"). */
static int
icc_read_eos_int(PTPParams *params, uint16_t code)
{
	PTPDevicePropDesc dpd;
	int v = -1;

	ptp_canon_eos_requestdevicepropvalue(params, code);   /* best-effort refresh */
	ptp_check_eos_events(params);

	memset(&dpd, 0, sizeof(dpd));
	if (ptp_canon_eos_getdevicepropdesc(params, code, &dpd) != PTP_RC_OK)
		return -1;
	switch (dpd.DataType) {
	case PTP_DTC_INT8:  case PTP_DTC_UINT8:  v = dpd.CurrentValue.u8;  break;
	case PTP_DTC_INT16: case PTP_DTC_UINT16: v = dpd.CurrentValue.u16; break;
	default:                                  v = (int)dpd.CurrentValue.u32; break;
	}
	ptp_free_devicepropdesc(&dpd);
	return v;
}

int
gp_iccamera_get_movie_mode(gp_iccamera *icc, int *raw_fixedmovie, int *raw_aemode)
{
	PTPParams *params = &icc->camera->pl->params;

	/* Primary: the movie switch (0xD1C2, UINT32). Fallback/cross-check: AE mode (0xD105,
	 * UINT16) whose value 0x14 == "Movie". Either is enough to declare VIDEO; if both read
	 * and neither indicates movie, it's PHOTO; if neither reads at all, UNKNOWN. */
	int fixedmovie = icc_read_eos_int(params, PTP_DPC_CANON_EOS_FixedMovie);
	int aemode     = icc_read_eos_int(params, PTP_DPC_CANON_EOS_AutoExposureMode);

	if (raw_fixedmovie) *raw_fixedmovie = fixedmovie;
	if (raw_aemode)     *raw_aemode     = aemode;

	if (fixedmovie > 0)               return GP_ICCAMERA_MOVIE_MODE_VIDEO;
	if (aemode == 0x14)               return GP_ICCAMERA_MOVIE_MODE_VIDEO;   /* "Movie" */
	if (fixedmovie == 0 || aemode >= 0)
		return GP_ICCAMERA_MOVIE_MODE_PHOTO;
	return GP_ICCAMERA_MOVIE_MODE_UNKNOWN;
}

/* --- Movie-size discovery probe (see gp_iccamera.h / canon-movie-size-probe.md) --- */

static const char *
icc_dtc_name(uint16_t dt)
{
	switch (dt) {
	case PTP_DTC_INT8:   return "INT8";
	case PTP_DTC_UINT8:  return "UINT8";
	case PTP_DTC_INT16:  return "INT16";
	case PTP_DTC_UINT16: return "UINT16";
	case PTP_DTC_INT32:  return "INT32";
	case PTP_DTC_UINT32: return "UINT32";
	case PTP_DTC_STR:    return "STR";
	default:             return "UNDEF/?";
	}
}

static uint32_t
icc_dpv_u32(uint16_t dt, PTPPropValue *v)
{
	switch (dt) {
	case PTP_DTC_INT8:  case PTP_DTC_UINT8:  return v->u8;
	case PTP_DTC_INT16: case PTP_DTC_UINT16: return v->u16;
	default:                                  return v->u32;
	}
}

int
gp_iccamera_movie_size_probe(gp_iccamera *icc, char *out, int outlen)
{
	PTPParams *params = &icc->camera->pl->params;
	static const struct { uint16_t code; const char *name; } props[] = {
		{ PTP_DPC_CANON_EOS_MovSize,                 "MovSize"     },
		{ PTP_DPC_CANON_EOS_MovieParam,              "MovieParam"  },
		{ PTP_DPC_CANON_EOS_MovieParam2,             "MovieParam2" },
		{ PTP_DPC_CANON_EOS_MovieParam3,             "MovieParam3" },
		{ PTP_DPC_CANON_EOS_MovieParam4,             "MovieParam4" },
		{ PTP_DPC_CANON_EOS_MovieParam5,             "MovieParam5" },
		{ PTP_DPC_CANON_EOS_VariableMovieRecSetting, "VarMovieRec" },
	};
	int off = 0;
	unsigned p;

	if (!out || outlen < 1)
		return -1;
	out[0] = '\0';

	#define APPEND(...) do { \
		if (off < outlen - 1) { \
			int _w = snprintf(out + off, (size_t)(outlen - off), __VA_ARGS__); \
			if (_w < 0)                    { /* encoding error: skip */ } \
			else if (_w >= outlen - off)   off = outlen - 1;  /* truncated */ \
			else                           off += _w; \
		} \
	} while (0)

	for (p = 0; p < sizeof(props)/sizeof(props[0]); p++) {
		PTPDevicePropDesc dpd;
		uint16_t code = props[p].code;

		ptp_canon_eos_requestdevicepropvalue(params, code);   /* best-effort refresh */
		ptp_check_eos_events(params);

		memset(&dpd, 0, sizeof(dpd));
		if (ptp_canon_eos_getdevicepropdesc(params, code, &dpd) != PTP_RC_OK) {
			APPEND("%04X %-11s  (not reported by this body)\n", code, props[p].name);
			continue;
		}

		APPEND("%04X %-11s dt=%-6s %s cur=0x%08x",
		       code, props[p].name, icc_dtc_name(dpd.DataType),
		       dpd.GetSet == PTP_DPGS_GetSet ? "rw" : "ro",
		       icc_dpv_u32(dpd.DataType, &dpd.CurrentValue));

		if (dpd.FormFlag & PTP_DPFF_Enumeration) {
			int n = dpd.FORM.Enum.NumberOfValues, i;
			APPEND(" enum[%d]={", n);
			for (i = 0; i < n; i++)
				APPEND("%s0x%x", i ? "," : "",
				       icc_dpv_u32(dpd.DataType, &dpd.FORM.Enum.SupportedValue[i]));
			APPEND("}");
		} else if (dpd.FormFlag & PTP_DPFF_Range) {
			APPEND(" range=[0x%x..0x%x step 0x%x]",
			       icc_dpv_u32(dpd.DataType, &dpd.FORM.Range.MinValue),
			       icc_dpv_u32(dpd.DataType, &dpd.FORM.Range.MaxValue),
			       icc_dpv_u32(dpd.DataType, &dpd.FORM.Range.StepSize));
		} else {
			APPEND(" (no form / single value)");
		}
		APPEND("\n");

		ptp_free_devicepropdesc(&dpd);
	}
	#undef APPEND

	return 0;
}

int
gp_iccamera_eos_props_dump(gp_iccamera *icc, char *out, int outlen)
{
	PTPParams *params = &icc->camera->pl->params;
	int off = 0;

	if (!out || outlen < 1)
		return -1;
	out[0] = '\0';

	ptp_check_eos_events(params);   /* fold in any pending events so the cache is current */

	/* Stable, code-sorted output makes before/after diffs clean. */
	enum { CAP = 512 };
	unsigned idx[CAP];
	unsigned n = params->canon_props.len;
	if (n > CAP) n = CAP;
	for (unsigned i = 0; i < n; i++) idx[i] = i;
	for (unsigned i = 0; i < n; i++)
		for (unsigned j = i + 1; j < n; j++)
			if (params->canon_props.val[idx[j]].DevicePropCode <
			    params->canon_props.val[idx[i]].DevicePropCode) {
				unsigned t = idx[i]; idx[i] = idx[j]; idx[j] = t;
			}

	#define APPEND(...) do { \
		if (off < outlen - 1) { \
			int _w = snprintf(out + off, (size_t)(outlen - off), __VA_ARGS__); \
			if (_w < 0)                  { /* skip */ } \
			else if (_w >= outlen - off) off = outlen - 1; \
			else                         off += _w; \
		} \
	} while (0)

	for (unsigned k = 0; k < n; k++) {
		PTPDevicePropDesc *dpd = &params->canon_props.val[idx[k]];

		APPEND("%04X dt=%-7s %s cur=0x%08x",
		       dpd->DevicePropCode, icc_dtc_name(dpd->DataType),
		       dpd->GetSet == PTP_DPGS_GetSet ? "rw" : "ro",
		       icc_dpv_u32(dpd->DataType, &dpd->CurrentValue));

		if (dpd->FormFlag & PTP_DPFF_Enumeration)
			APPEND(" enum[%d]", dpd->FORM.Enum.NumberOfValues);
		else if (dpd->FormFlag & PTP_DPFF_Range)
			APPEND(" range");
		APPEND("\n");
	}
	#undef APPEND

	return (int)n;
}

static int
icc_has_op(PTPParams *p, uint16_t op)
{
	for (unsigned i = 0; i < p->deviceinfo.Operations_len; i++)
		if (p->deviceinfo.Operations[i] == op) return 1;
	return 0;
}

static int
icc_has_eos_prop(PTPParams *p, uint16_t dpc)
{
	for (unsigned i = 0; i < p->canon_props.len; i++)
		if (p->canon_props.val[i].DevicePropCode == dpc) return 1;
	return 0;
}

int
gp_iccamera_capabilities(gp_iccamera *icc, char *out, int outlen)
{
	PTPParams     *params = &icc->camera->pl->params;
	PTPDeviceInfo *di     = &params->deviceinfo;
	int off = 0;
	unsigned i;

	if (!out || outlen < 1)
		return -1;
	out[0] = '\0';

	int is_canon = (di->VendorExtensionID == PTP_VENDOR_CANON);
	int is_eos   = is_canon && icc_has_op(params, PTP_OC_CANON_EOS_GetEvent);
	if (is_eos)
		ptp_check_eos_events(params);   /* populate the EOS property cache */

	#define APPEND(...) do { \
		if (off < outlen - 1) { \
			int _w = snprintf(out + off, (size_t)(outlen - off), __VA_ARGS__); \
			if (_w < 0)                  { /* skip */ } \
			else if (_w >= outlen - off) off = outlen - 1; \
			else                         off += _w; \
		} \
	} while (0)

	APPEND("== %s ==\n", di->Model ? di->Model : "(unknown model)");
	APPEND("manufacturer: %s   firmware: %s\n",
	       di->Manufacturer ? di->Manufacturer : "?", di->DeviceVersion ? di->DeviceVersion : "?");
	APPEND("serial: %s\n", di->SerialNumber ? di->SerialNumber : "?");
	APPEND("vendor ext: 0x%08x   ops: %u   events: %u   std props: %u   EOS props: %u\n",
	       di->VendorExtensionID, di->Operations_len, di->Events_len, di->DeviceProps_len,
	       (unsigned)params->canon_props.len);

	APPEND("\nCLASS: %s\n",
	       is_eos   ? "Canon EOS (full support: config tree, capture, live view, events)" :
	       is_canon ? "Canon legacy / PowerShot (config tree only; no EOS event model)" :
	                  "non-Canon / generic PTP");

	APPEND("\nFEATURES:\n");
	APPEND("  remote capture ......... %s\n",
	       (icc_has_op(params, PTP_OC_CANON_EOS_RemoteReleaseOn) ||
	        icc_has_op(params, PTP_OC_CANON_EOS_RemoteRelease)) ? "yes" : "no");
	APPEND("  live view (EVF) ........ %s\n",
	       icc_has_op(params, PTP_OC_CANON_EOS_GetViewFinderData) ? "yes" : "no");
	APPEND("  autofocus (DoAf) ....... %s\n", icc_has_op(params, PTP_OC_CANON_EOS_DoAf) ? "yes" : "no");
	APPEND("  movie switch ........... %s\n",
	       icc_has_op(params, PTP_OC_CANON_EOS_MovieSelectSWOn) ? "yes" : "no");
	APPEND("  image format (RAW/…) ... %s\n",
	       icc_has_eos_prop(params, PTP_DPC_CANON_EOS_ImageFormat) ? "yes (0xD120)" : "no");
	if (icc_has_eos_prop(params, PTP_DPC_CANON_EOS_MovieParam5))
		APPEND("  movie recording size ... yes: 0xD20D (MovieParam5, R50-style 40-byte struct) — decoded\n");
	else if (icc_has_eos_prop(params, PTP_DPC_CANON_EOS_MovieParam6))
		APPEND("  movie recording size ... yes: 0xD29E (MovieParam6, R50 V-style 32-byte struct) — decoded\n");
	else if (is_eos)
		APPEND("  movie recording size ... carrier UNKNOWN on this body — discover with\n"
		       "                           gp_iccamera_watch_prop_changes (see canon-movie-recording-size.md)\n");

	APPEND("\nOPERATIONS (%u):\n", di->Operations_len);
	for (i = 0; i < di->Operations_len; i++)
		APPEND("  %04x  %s\n", di->Operations[i], ptp_get_opcode_name(params, di->Operations[i]));

	#undef APPEND

	return is_eos ? GP_ICCAMERA_CLASS_CANON_EOS :
	       is_canon ? GP_ICCAMERA_CLASS_CANON_LEGACY :
	                  GP_ICCAMERA_CLASS_UNKNOWN;
}

int
gp_iccamera_watch_prop_changes(gp_iccamera *icc, int ms, char *out, int outlen)
{
	PTPParams *params = &icc->camera->pl->params;

	if (!out || outlen < 1)
		return -1;
	out[0] = '\0';
	if (params->deviceinfo.VendorExtensionID != PTP_VENDOR_CANON)
		return -1;
	if (ms < 0)
		ms = 0;

	enum { MAXCODES = 128, RAWCAP = 1536 };
	uint16_t codes[MAXCODES];
	int      counts[MAXCODES];
	int      ncodes = 0;
	char     raw[RAWCAP];
	int      rawoff = 0;
	raw[0] = '\0';

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (;;) {
		PTPCanonEOSEvent ev;

		if (ptp_check_eos_events(params) != PTP_RC_OK)
			break;

		while (ptp_get_one_eos_event(params, &ev)) {
			if (ev.type == PTP_EOSEvent_PropertyChanged) {
				uint16_t code = ev.u.propid;
				int f = -1;
				for (int i = 0; i < ncodes; i++)
					if (codes[i] == code) { f = i; break; }
				if (f < 0 && ncodes < MAXCODES) {
					codes[ncodes] = code; counts[ncodes] = 0; f = ncodes; ncodes++;
				}
				if (f >= 0) counts[f]++;
			} else if (ev.type == PTP_EOSEvent_Unknown && ev.u.info[0]) {
				/* unhandled event carrying a raw hex payload — capture verbatim */
				if (rawoff < RAWCAP - 1) {
					int w = snprintf(raw + rawoff, (size_t)(RAWCAP - rawoff),
					                 "RAW %s\n", ev.u.info);
					if (w > 0 && w < RAWCAP - rawoff) rawoff += w;
					else rawoff = RAWCAP - 1;
				}
			}
			ptp_free_eos_event(&ev);
		}

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed = (now.tv_sec - t0.tv_sec) * 1000L
		             + (now.tv_nsec - t0.tv_nsec) / 1000000L;
		if (elapsed >= ms)
			break;
		usleep(50 * 1000);   /* 50 ms between polls */
	}

	/* sort codes ascending for a stable summary */
	for (int i = 0; i < ncodes; i++)
		for (int j = i + 1; j < ncodes; j++)
			if (codes[j] < codes[i]) {
				uint16_t tc = codes[i]; codes[i] = codes[j]; codes[j] = tc;
				int tn = counts[i]; counts[i] = counts[j]; counts[j] = tn;
			}

	int off = 0;
	#define APPEND(...) do { \
		if (off < outlen - 1) { \
			int _w = snprintf(out + off, (size_t)(outlen - off), __VA_ARGS__); \
			if (_w < 0)                  { /* skip */ } \
			else if (_w >= outlen - off) off = outlen - 1; \
			else                         off += _w; \
		} \
	} while (0)

	APPEND("watched %d ms: %d distinct prop code(s)\n", ms, ncodes);
	for (int i = 0; i < ncodes; i++)
		APPEND("PROP %04x  x%d\n", codes[i], counts[i]);
	if (rawoff > 0)
		APPEND("%s", raw);
	#undef APPEND

	return ncodes;
}

/* --- Movie recording size (resolution + fps), via the gp_canon_moviesize table --- */

int
gp_iccamera_get_movie_size(gp_iccamera *icc,
                           uint32_t *code, int *width, int *height, int *fps_x100,
                           char *label, int labellen,
                           uint32_t *choices, int choicecap, int *nchoices)
{
	PTPParams  *params = &icc->camera->pl->params;
	const char *model  = params->deviceinfo.Model;

	if (code)     *code     = 0;
	if (width)    *width    = 0;
	if (height)   *height   = 0;
	if (fps_x100) *fps_x100 = 0;
	if (label && labellen > 0) label[0] = '\0';
	if (nchoices) *nchoices = 0;

	uint16_t prop = gp_canon_moviesize_prop(model);
	if (!prop)
		return -2;   /* body's movie-size property unknown (e.g. EOS R50 V) or unknown body */

	/* The driver packs each movie-size struct into a u32: (rescode<<16)|(fps*100). `choices`
	 * (if requested) returns the camera's availlist of valid sizes — decode each with
	 * gp_canon_moviesize_decode to build a picker. */
	uint32_t packed = 0, dt = 0;
	if (gp_iccamera_get_eosprop(icc, prop, &packed, &dt, choices, choicecap, nchoices) != 0)
		return -1;   /* property not reported by this body */

	if (code) *code = packed;

	gp_canon_movsize m;
	gp_canon_moviesize_decode(model, packed, &m);
	if (width)    *width    = m.width;
	if (height)   *height   = m.height;
	if (fps_x100) *fps_x100 = m.fps_x100;
	if (label && labellen > 0) {
		if (m.width > 0)
			snprintf(label, (size_t)labellen, "%dx%d %d.%02dp",
			         m.width, m.height, m.fps_x100/100, m.fps_x100%100);
		else
			snprintf(label, (size_t)labellen, "res?(0x%x) %d.%02dp",
			         packed >> 16, m.fps_x100/100, m.fps_x100%100);
	}
	return 0;
}

int
gp_iccamera_set_movie_size(gp_iccamera *icc, uint32_t code)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t   prop   = gp_canon_moviesize_prop(params->deviceinfo.Model);
	if (!prop)
		return -2;
	/* `code` is the packed (rescode<<16)|(fps*100); ptp_canon_eos_setdevicepropvalue expands it
	 * back into the 40-byte MovieParam5 struct. */
	return gp_iccamera_set_eosprop(icc, prop, code);
}

/* --- Event-driven update: drain the one Canon EOS event queue (EDSDK property/object/state
 * events all land here) and summarise what changed. See EDSDK-CAPABILITY-MAP.md step 3. --- */
int
gp_iccamera_poll_events(gp_iccamera *icc, char *out, int outlen)
{
	PTPParams        *params = &icc->camera->pl->params;
	PTPCanonEOSEvent  ev;
	int               n = 0;
	char             *sp = out; int sl = outlen;

	if (out && outlen) out[0] = 0;
	if (params->deviceinfo.VendorExtensionID != PTP_VENDOR_CANON) return 0;
	if (ptp_check_eos_events(params) != PTP_RC_OK) return -1;

	while (ptp_get_one_eos_event(params, &ev)) {
		int w = 0;
		switch (ev.type) {
		case PTP_EOSEvent_PropertyChanged:
			w = snprintf(sp, (size_t)sl, "PROP %04x\n", ev.u.propid);
			break;
		case PTP_EOSEvent_ObjectAdded:      /* new file on card (SaveTo=card) */
		case PTP_EOSEvent_ObjectTransfer:   /* camera asks host to pull it (SaveTo=host) */
			w = snprintf(sp, (size_t)sl, "OBJECT %08x %04x %llu\n",
			             ev.u.object.Handle, ev.u.object.ObjectFormat,
			             (unsigned long long)ev.u.object.ObjectSize);
			break;
		case PTP_EOSEvent_ObjectRemoved:
			w = snprintf(sp, (size_t)sl, "OBJECTREMOVED\n");
			break;
		case PTP_EOSEvent_CameraStatus:
			w = snprintf(sp, (size_t)sl, "STATUS %d\n", ev.u.status);
			break;
		case PTP_EOSEvent_FocusInfo:
			w = snprintf(sp, (size_t)sl, "FOCUS\n");
			break;
		case PTP_EOSEvent_Unknown:
			/* Debug surface: unhandled events carry a human string in u.info
			 * (code + raw hex bytes) — used to discover roll/pitch level, etc. */
			if (ev.u.info[0])
				w = snprintf(sp, (size_t)sl, "RAW %s\n", ev.u.info);
			break;
		default:
			w = 0;
			break;
		}
		if (w > 0 && w < sl) { sp += w; sl -= w; }
		ptp_free_eos_event(&ev);
		n++;
	}
	return n;
}

/* Download one object by handle (auto-pull a body-triggered shot). Mirrors capture()'s
 * download step so the verified capture path is left untouched. */
int
gp_iccamera_download(gp_iccamera *icc, uint32_t handle, uint32_t fmt, uint32_t size,
                     uint8_t **outdata, int *outlen, char *ext, int extlen,
                     char *status, int statuslen)
{
	PTPParams *params = &icc->camera->pl->params;
	uint16_t   r;
	char      *sp = status; int sl = statuslen;
#define SLOG(...) do { int _n = snprintf(sp, (size_t)sl, __VA_ARGS__); if (_n > 0 && _n < sl) { sp += _n; sl -= _n; } } while (0)

	*outdata = NULL; *outlen = 0;
	if (status && statuslen) status[0] = 0;
	if (ext && extlen) {
		if (fmt == PTP_OFC_CANON_CR3) snprintf(ext, (size_t)extlen, "cr3");
		else if (fmt == PTP_OFC_CANON_CRW || fmt == PTP_OFC_CANON_CRW3) snprintf(ext, (size_t)extlen, "cr2");
		else snprintf(ext, (size_t)extlen, "jpg");
	}

	if (size == 0) {   /* unknown size: one big read, use whatever comes back */
		unsigned char *chunk = NULL; uint32_t got = 0x0fffffff;
		r = ptp_getpartialobject(params, handle, 0, got, &chunk, &got);
		if (r != PTP_RC_OK || !chunk) { SLOG("getobject failed 0x%04x", r); return -1; }
		ptp_canon_eos_transfercomplete(params, handle);
		*outdata = chunk; *outlen = (int)got;
		SLOG("downloaded %u bytes", got);
		return 0;
	}

	{
		uint32_t total = size, offset = 0;
		uint8_t *buf = malloc(total);
		if (!buf) { SLOG("out of memory (%u)", total); return -2; }
		while (offset < total) {
			unsigned char *chunk = NULL;
			uint32_t want = total - offset;
			if (want > (1u << 20)) want = (1u << 20);
			r = ptp_getpartialobject(params, handle, offset, want, &chunk, &want);
			if (r != PTP_RC_OK || !chunk) { SLOG("getpartialobject@%u failed 0x%04x", offset, r); free(buf); return -1; }
			memcpy(buf + offset, chunk, want);
			free(chunk);
			if (want == 0) break;
			offset += want;
		}
		ptp_canon_eos_transfercomplete(params, handle);
		*outdata = buf; *outlen = (int)offset;
		SLOG("downloaded %d bytes", (int)offset);
	}
	return 0;
#undef SLOG
}

int
gp_iccamera_keepalive(gp_iccamera *icc)
{
	PTPParams *params = &icc->camera->pl->params;
	return ptp_canon_eos_keepdeviceon(params) == PTP_RC_OK ? 0 : -1;
}

int
gp_iccamera_raw_op(gp_iccamera *icc, uint16_t opcode,
                   const uint32_t *params_in, int nparams, char *out, int outlen)
{
	PTPParams *params = &icc->camera->pl->params;
	PTPContainer ptp;
	unsigned char *data = NULL;
	unsigned int rlen = 0;
	uint16_t r;
	int i;

	memset(&ptp, 0, sizeof(ptp));
	ptp.Code = opcode;
	if (nparams > 5) nparams = 5;
	ptp.Nparam = (uint8_t)nparams;
	for (i = 0; i < nparams; i++)
		(&ptp.Param1)[i] = params_in[i];

	r = ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &data, &rlen);
	if (data) free(data);
	snprintf(out, (size_t)outlen, "op 0x%04x → response 0x%04x, %u bytes data", opcode, r, rlen);
	return (int)r;
}
