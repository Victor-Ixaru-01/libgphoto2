/*
 * gp_canon_imagesize.c — hardcoded Canon EOS size-class → pixel-dimension table.
 *
 * See gp_canon_imagesize.h for the why. The design in three steps:
 *   1. Decode the imageformat label into compression-stripped size classes
 *      ("cRAW + L" -> {SC_RAW, SC_L}).
 *   2. Look up each class in the connected body's profile at its NATIVE aspect ratio.
 *   3. Derive non-native aspect ratios by cropping the native dimensions (sensor-crop
 *      modes such as "1.6x" instead require an explicit override row).
 */
#include "gp_canon_imagesize.h"

#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ size classes */
/* Compression-stripped resolution classes. RAW and cRAW both map to SC_RAW; the
 * 'c' (Normal-compression) JPEG variants map to their base class. */
enum {
	SC_RAW, SC_MRAW, SC_SRAW,
	SC_L, SC_M, SC_S,
	SC_M1, SC_M2,
	SC_S1, SC_S2, SC_S3,
	SC_UNKNOWN
};

/* Every imageformat token the ptp2 driver can emit (mirrors canon_eos_single_ImageFormats
 * in camlibs/ptp2/config.c) mapped to a size class. Both the plain and 'c'-prefixed forms
 * point at the same class, because compression does not change the pixel dimensions. */
static const struct { const char *label; int cls; } kLabelClass[] = {
	{ "RAW",  SC_RAW  }, { "cRAW", SC_RAW  },
	{ "mRAW", SC_MRAW }, { "sRAW", SC_SRAW },
	{ "L",  SC_L  }, { "cL",  SC_L  },
	{ "M",  SC_M  }, { "cM",  SC_M  },
	{ "S",  SC_S  }, { "cS",  SC_S  },
	{ "M1", SC_M1 }, { "cM1", SC_M1 },
	{ "M2", SC_M2 }, { "cM2", SC_M2 },
	{ "S1", SC_S1 }, { "cS1", SC_S1 },
	{ "S2", SC_S2 }, { "cS2", SC_S2 },
	{ "S3", SC_S3 }, { "cS3", SC_S3 },
};

static int
class_for_label(const char *tok, int len)
{
	for (unsigned i = 0; i < sizeof(kLabelClass)/sizeof(kLabelClass[0]); i++)
		if ((int)strlen(kLabelClass[i].label) == len &&
		    strncmp(kLabelClass[i].label, tok, (size_t)len) == 0)
			return kLabelClass[i].cls;
	return SC_UNKNOWN;
}

/* ------------------------------------------------------------------ body profiles */
typedef struct { int cls; int w; int h; }                 sizerow;
typedef struct { const char *aspect; int cls; int w; int h; } overriderow;

typedef struct {
	const char        *key;        /* normalized model, see normalize_model() */
	const sizerow     *sizes;      /* dimensions at the native aspect ratio    */
	int                nsizes;
	const overriderow *overrides;  /* exact dims for non-native / crop aspects  */
	int                noverrides;
} bodyprofile;

/*
 * ============================ VERIFY BEFORE SHIPPING ============================
 * The pixel counts below are examples. Confirm each against the body's official
 * spec sheet (Canon lists L/M/S1/S2/S3 and RAW/mRAW/sRAW per aspect ratio). To add a
 * body: normalize its model name the way normalize_model() does ("Canon EOS 90D" ->
 * "90d") and append a profile. Native ratio is inferred from the SC_L (or SC_RAW) row,
 * so list those. Only base classes are needed — cRAW/cL/… reuse them automatically.
 * ===============================================================================
 */

/* EOS 5D Mark III — full-frame, native 3:2, ~22 MP (has mRAW/sRAW + S1/S2/S3). */
static const sizerow k_5dm3[] = {
	{ SC_RAW,  5760, 3840 }, { SC_MRAW, 3960, 2640 }, { SC_SRAW, 2880, 1920 },
	{ SC_L,    5760, 3840 }, { SC_M,    3840, 2560 },
	{ SC_S1,   2880, 1920 }, { SC_S2,   1920, 1280 }, { SC_S3,    720,  480 },
};

/* EOS R5 — full-frame, native 3:2, ~45 MP (no mRAW/sRAW). 1.6x is a sensor crop. */
static const sizerow k_r5[] = {
	{ SC_RAW, 8192, 5464 },
	{ SC_L,   8192, 5464 }, { SC_M, 5952, 3968 },
	{ SC_S1,  3984, 2656 }, { SC_S2, 2400, 1600 },
};
static const overriderow k_r5_ovr[] = {
	{ "1.6x", SC_RAW, 5088, 3392 },
	{ "1.6x", SC_L,   5088, 3392 },
};

/* EOS R50 — APS-C, native 3:2, ~24 MP (no mRAW/sRAW).
 * 3:2 rows are Canon's published spec; the 4:3 overrides were measured on a real body.
 * RAW stays full-sensor 6000x4000 at every framing ratio — Canon crops only the JPEG. */
static const sizerow k_r50[] = {
	{ SC_RAW, 6000, 4000 },
	{ SC_L,   6000, 4000 }, { SC_M, 3984, 2656 },
	{ SC_S1,  2976, 1984 }, { SC_S2, 2400, 1600 },
};
static const overriderow k_r50_ovr[] = {
	/* Measured on a real R50 at 4:3. No RAW row here: RAW is aspect-invariant full sensor.
	 * M height 2664 per Canon spec / the observed 9.5 MP; the test note's "2554" yields only
	 * 9.1 MP, so it's treated as a transcription slip — re-confirm on-body if you can. */
	{ "4:3", SC_L,  5328, 4000 },
	{ "4:3", SC_M,  3552, 2664 },
	{ "4:3", SC_S1, 2656, 1992 },
	{ "4:3", SC_S2, 2112, 1600 },
};

/* EOS 90D — APS-C, native 3:2, ~32.5 MP. */
static const sizerow k_90d[] = {
	{ SC_RAW, 6960, 4640 },
	{ SC_L,   6960, 4640 }, { SC_M, 4640, 3088 },
	{ SC_S1,  3472, 2320 }, { SC_S2, 2400, 1600 },
};

#define ROWS(a) (a), (int)(sizeof(a)/sizeof((a)[0]))
static const bodyprofile kBodies[] = {
	{ "5d mark iii", ROWS(k_5dm3), NULL, 0 },
	{ "r5",          ROWS(k_r5),   ROWS(k_r5_ovr) },
	{ "r50",         ROWS(k_r50),  ROWS(k_r50_ovr) },
	{ "90d",         ROWS(k_90d),  NULL, 0 },
};
#undef ROWS

/* ------------------------------------------------------------------ model matching */
/* Lowercase; drop "canon"/"eos"; turn every non-alphanumeric run into a single space;
 * trim. "Canon EOS 5D Mark III" -> "5d mark iii", "EOS-R5" -> "r5". */
static void
normalize_model(const char *in, char *out, int outcap)
{
	int o = 0, prev_space = 1;
	for (const char *p = in ? in : ""; *p && o < outcap - 1; p++) {
		char c = (char)tolower((unsigned char)*p);
		if (isalnum((unsigned char)c)) {
			out[o++] = c;
			prev_space = 0;
		} else if (!prev_space) {
			out[o++] = ' ';
			prev_space = 1;
		}
	}
	while (o > 0 && out[o - 1] == ' ') o--;   /* trim trailing space */
	out[o] = '\0';

	/* strip the vendor words wherever they land, then re-collapse spaces */
	static const char *drop[] = { "canon", "eos" };
	for (unsigned d = 0; d < sizeof(drop)/sizeof(drop[0]); d++) {
		char *hit;
		size_t dl = strlen(drop[d]);
		while ((hit = strstr(out, drop[d])) != NULL)
			memmove(hit, hit + dl, strlen(hit + dl) + 1);
	}
	/* collapse any doubled/edge spaces created by the removals */
	int w = 0; prev_space = 1;
	for (int r = 0; out[r]; r++) {
		if (out[r] == ' ') {
			if (!prev_space) { out[w++] = ' '; prev_space = 1; }
		} else {
			out[w++] = out[r]; prev_space = 0;
		}
	}
	while (w > 0 && out[w - 1] == ' ') w--;
	out[w] = '\0';
}

/* Longest matching key wins, so "5d mark iii" is not captured by a "5d mark ii" key and
 * "r50" is not captured by "r5". */
static const bodyprofile *
find_body(const char *model)
{
	char norm[128];
	normalize_model(model, norm, (int)sizeof(norm));

	const bodyprofile *best = NULL;
	size_t bestlen = 0;
	for (unsigned i = 0; i < sizeof(kBodies)/sizeof(kBodies[0]); i++) {
		if (strstr(norm, kBodies[i].key)) {
			size_t kl = strlen(kBodies[i].key);
			if (kl > bestlen) { best = &kBodies[i]; bestlen = kl; }
		}
	}
	return best;
}

/* ------------------------------------------------------------------ aspect ratios */
/* Return the width/height ratio for an aspect label. 0 => native/unknown (use base dims);
 * negative => a sensor-crop mode that can't be derived (needs an override row). */
static double
aspect_ratio(const char *a)
{
	if (!a || !*a)             return 0.0;
	if (!strcmp(a, "3:2"))     return 3.0 / 2.0;
	if (!strcmp(a, "4:3"))     return 4.0 / 3.0;
	if (!strcmp(a, "16:9"))    return 16.0 / 9.0;
	if (!strcmp(a, "1:1"))     return 1.0;
	if (!strcmp(a, "1.6x"))    return -1.0;   /* APS-C crop of a FF sensor */
	return 0.0;                                /* unrecognized → treat as native */
}

/* Crop native (base) dimensions to a target ratio, matching how Canon trims the sensor:
 * wider target → keep width, trim height; narrower target → keep height, trim width. The
 * trim DIRECTION is decided by the body's native ratio, not the individual class's own
 * aspect (Canon rounds per-class dims, so e.g. S1 is 3472x2320 = 1.4966, not exactly 3:2). */
static gp_canon_pixsize
crop_to_ratio(gp_canon_pixsize base, double native, double target)
{
	gp_canon_pixsize r = base;
	if (base.width <= 0 || base.height <= 0 || target <= 0.0 || native <= 0.0)
		return base;
	if (target >= native)
		r.height = (int)((double)base.width / target + 0.5);
	else
		r.width  = (int)((double)base.height * target + 0.5);
	return r;
}

/* The body's native aspect ratio, taken from its full-frame row (SC_L, else SC_RAW, else
 * the first row). All EOS bodies here are 3:2, but this stays correct for a 4:3 body too. */
static double
body_native_ratio(const bodyprofile *b)
{
	const sizerow *full = NULL;
	for (int i = 0; i < b->nsizes; i++)
		if (b->sizes[i].cls == SC_L) { full = &b->sizes[i]; break; }
	if (!full)
		for (int i = 0; i < b->nsizes; i++)
			if (b->sizes[i].cls == SC_RAW) { full = &b->sizes[i]; break; }
	if (!full && b->nsizes > 0)
		full = &b->sizes[0];
	if (!full || full->h <= 0)
		return 0.0;
	return (double)full->w / (double)full->h;
}

/* ------------------------------------------------------------------ resolution */
static gp_canon_pixsize
size_for_class(const bodyprofile *b, int cls, const char *aspect)
{
	gp_canon_pixsize none = { 0, 0 };
	if (cls == SC_UNKNOWN)
		return none;

	/* exact override for this (aspect, class) wins */
	for (int i = 0; i < b->noverrides; i++)
		if (b->overrides[i].cls == cls &&
		    aspect && !strcmp(b->overrides[i].aspect, aspect)) {
			gp_canon_pixsize s = { b->overrides[i].w, b->overrides[i].h };
			return s;
		}

	/* base (native-aspect) dimensions for the class */
	gp_canon_pixsize base = none;
	int found = 0;
	for (int i = 0; i < b->nsizes; i++)
		if (b->sizes[i].cls == cls) {
			base.width  = b->sizes[i].w;
			base.height = b->sizes[i].h;
			found = 1;
			break;
		}
	if (!found)
		return none;

	double target = aspect_ratio(aspect);
	if (target == 0.0)      /* native or unknown aspect → base dims as-is */
		return base;

	/* RAW-family files always record the FULL sensor; Canon's aspect-ratio setting only
	 * crops the JPEG (the aspect rides along as a crop hint inside the CR3). So RAW/mRAW/sRAW
	 * ignore the framing ratios and stay native — verified on the R50: RAW is 6000x4000 at
	 * 4:3, not the 5328x4000 the JPEG becomes. A genuine SENSOR crop (e.g. R5 1.6x) does shrink
	 * RAW, but that arrives as an explicit override handled above; lacking one, we can't know
	 * the cropped size, so a sensor-crop mode returns unknown. */
	if (cls == SC_RAW || cls == SC_MRAW || cls == SC_SRAW)
		return (target > 0.0) ? base : none;

	if (target < 0.0)       /* JPEG in a sensor-crop mode with no override → unknown */
		return none;

	/* Requested aspect == the body's native aspect: return the stored dims verbatim
	 * (re-cropping would distort by a pixel or two due to Canon's per-class rounding). */
	double native = body_native_ratio(b);
	double d = target - native;
	if (d < 0) d = -d;
	if (d < 0.01)
		return base;

	return crop_to_ratio(base, native, target);
}

int
gp_canon_imagesize_resolve(const char *model, const char *label, const char *aspect,
                           gp_canon_pixsize *out, int cap)
{
	if (!model || !label || !out || cap < 1)
		return -1;

	const bodyprofile *b = find_body(model);   /* NULL => every entry stays {0,0} */

	/* Walk the label, splitting on " + ", emitting one size per component. */
	int n = 0;
	const char *p = label;
	while (*p && n < cap) {
		while (*p == ' ') p++;                  /* skip leading space */
		const char *tok = p;
		const char *sep = strstr(p, " + ");
		int len = sep ? (int)(sep - p) : (int)strlen(p);
		while (len > 0 && tok[len - 1] == ' ') len--;   /* trim trailing space */

		if (len > 0) {
			int cls = class_for_label(tok, len);
			out[n] = b ? size_for_class(b, cls, aspect)
			           : (gp_canon_pixsize){ 0, 0 };
			n++;
		}
		if (!sep) break;
		p = sep + 3;                             /* past " + " */
	}
	return n;
}

int
gp_canon_imagesize_known_body(const char *model)
{
	return find_body(model) != NULL;
}
