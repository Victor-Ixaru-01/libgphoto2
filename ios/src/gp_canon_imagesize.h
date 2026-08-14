/*
 * gp_canon_imagesize.h — map a Canon EOS "imageformat" label to output pixel dimensions.
 *
 * libgphoto2/ptp2 exposes the still-image size only as SYMBOLIC classes: L/M/S/M1/M2/
 * S1/S2/S3 for JPEG, and RAW/cRAW/mRAW/sRAW for raw. It never reports the pixel count
 * for a class, and Canon EOS bodies usually leave ImagePixWidth/Height == 0 in the
 * captured object's ObjectInfo (the real dimensions live only in the file's EXIF, which
 * libgphoto2 does not parse). This module carries a HARDCODED per-body table
 * (size class × aspect ratio → width×height) so the app can show megapixels BEFORE a
 * frame exists.
 *
 * Compression never changes the dimensions — only the byte size — so for sizing:
 *     cRAW  ≡ RAW      (both full-sensor; cRAW is merely compressed)
 *     cL    ≡ L,   cM ≡ M,   cS1 ≡ S1, …   (the leading 'c' = Normal vs Fine JPEG)
 *
 * Pure computation: NO camera transport and NO gp_iccamera handle. Feed it the three
 * strings you already read from the config tree:
 *     model   gp_iccamera_get_deviceinfo() / abilities, e.g. "Canon EOS 5D Mark III"
 *     label   gp_iccamera_get_config(...,"imageformat",...), e.g. "cRAW + L"
 *     aspect  gp_iccamera_get_config(...,"aspectratio",...), e.g. "3:2" (NULL/"" = native)
 *
 * The dimensions in the .c table are examples and MUST be verified against each body's
 * official spec sheet before you rely on them. Unknown body / class => {0,0}, which is
 * the app's signal to fall back to ObjectSize / EXIF.
 */
#ifndef GP_CANON_IMAGESIZE_H
#define GP_CANON_IMAGESIZE_H

#ifdef __cplusplus
extern "C" {
#endif

/* One output file's pixel dimensions. width == 0 means "unknown for this body/class". */
typedef struct {
	int width;
	int height;
} gp_canon_pixsize;

/* Output megapixels (width*height / 1e6). This is the produced pixel count, which differs
 * slightly from Canon's marketed sensor rating (e.g. 5760x3840 = 22.1 MP vs "22.3 MP"). */
static inline double
gp_canon_pixsize_megapixels(gp_canon_pixsize s)
{
	return (double)s.width * (double)s.height / 1000000.0;
}

/*
 * Resolve the pixel dimensions of every file a Canon EOS "imageformat" setting produces.
 *
 *   model   camera model string (matched case-insensitively; "Canon"/"EOS" and punctuation
 *           are ignored, so "Canon EOS R5" and "EOS-R5" both match the "r5" profile).
 *   label   the imageformat widget value, single ("RAW") or dual ("cRAW + L").
 *   aspect  the aspectratio widget value ("3:2","4:3","16:9","1:1","1.6x"); NULL or "" =
 *           the body's native aspect. Non-native ratios are derived by cropping the native
 *           dimensions, EXCEPT sensor-crop modes ("1.6x") which need an explicit table
 *           override — without one they resolve to {0,0}.
 *   out     caller array of at least `cap` entries; up to 2 are written (RAW+JPEG dual).
 *   cap     capacity of `out` (pass >= 2).
 *
 * Returns the number of files in the label (0..2) — i.e. how many `out` entries were set.
 * Each out[i] carries the dimensions, or {0,0} when the body or class is unknown. Returns
 * a negative value only on a usage error (NULL model/label/out, or cap < 1).
 *
 * Example: gp_canon_imagesize_resolve("Canon EOS 5D Mark III", "cRAW + L", "3:2", out, 2)
 *          → 2; out[0] = {5760,3840} (cRAW, full), out[1] = {5760,3840} (L).
 */
int gp_canon_imagesize_resolve(const char *model, const char *label, const char *aspect,
                               gp_canon_pixsize *out, int cap);

/* True (non-zero) if `model` matches a body profile in the table — i.e. resolve() can
 * return real dimensions for it. Use it to decide up front whether to trust this module
 * or fall back to EXIF for an unrecognized body. */
int gp_canon_imagesize_known_body(const char *model);

#ifdef __cplusplus
}
#endif

#endif /* GP_CANON_IMAGESIZE_H */
