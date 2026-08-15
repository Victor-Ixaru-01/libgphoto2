/*
 * gp_canon_moviesize.c — per-body Canon EOS movie recording-size table.
 *
 * See gp_canon_moviesize.h. Model matching mirrors gp_canon_imagesize.c: normalize the model
 * (strip "Canon"/"EOS"/punctuation, lowercase) and match by longest key, so "R50" and "R50 V"
 * resolve to distinct profiles.
 */
#include "gp_canon_moviesize.h"

#include <string.h>
#include <ctype.h>

/* Resolution-code → dimensions, per body. fps is generic (packed low 16 bits) and not tabled. */
typedef struct { int code; int w; int h; } rescode;

/* Resolution codes are shared across bodies (verified on-body): 0 = FHD, 5 = 4K UHD. */
static const rescode k_r50_res[] = {
	{ 0, 1920, 1080 },
	{ 5, 3840, 2160 },
};

typedef struct {
	const char    *key;    /* normalized model                      */
	uint16_t       prop;   /* movie-size carrier DPC, 0 if TBD       */
	const rescode *res;    /* resolution-code table                 */
	int            nres;
} bodyprofile;

#define ROWS(a) (a), (int)(sizeof(a)/sizeof((a)[0]))
static const bodyprofile kBodies[] = {
	{ "r50",   0xD20D, ROWS(k_r50_res) },   /* MovieParam5, 40-byte struct, fps at word 1 */
	{ "r50 v", 0xD29E, ROWS(k_r50_res) },   /* MovieParam6, 32-byte struct, actual fps at word 7 */
};
#undef ROWS

/* Lowercase; drop "canon"/"eos"; collapse non-alphanumeric runs to single spaces; trim.
 * "Canon EOS R50 V" -> "r50 v", "EOS-R50" -> "r50". */
static void
normalize_model(const char *in, char *out, int outcap)
{
	int o = 0, prev_space = 1;
	for (const char *p = in ? in : ""; *p && o < outcap - 1; p++) {
		char c = (char)tolower((unsigned char)*p);
		if (isalnum((unsigned char)c)) { out[o++] = c; prev_space = 0; }
		else if (!prev_space)          { out[o++] = ' '; prev_space = 1; }
	}
	while (o > 0 && out[o - 1] == ' ') o--;
	out[o] = '\0';

	static const char *drop[] = { "canon", "eos" };
	for (unsigned d = 0; d < sizeof(drop)/sizeof(drop[0]); d++) {
		char  *hit;
		size_t dl = strlen(drop[d]);
		while ((hit = strstr(out, drop[d])) != NULL)
			memmove(hit, hit + dl, strlen(hit + dl) + 1);
	}
	int w = 0; prev_space = 1;
	for (int r = 0; out[r]; r++) {
		if (out[r] == ' ') { if (!prev_space) { out[w++] = ' '; prev_space = 1; } }
		else               { out[w++] = out[r]; prev_space = 0; }
	}
	while (w > 0 && out[w - 1] == ' ') w--;
	out[w] = '\0';
}

/* Longest matching key wins, so "r50" doesn't capture an "r50 v" model. */
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

uint16_t
gp_canon_moviesize_prop(const char *model)
{
	const bodyprofile *b = find_body(model);
	return b ? b->prop : 0;
}

int
gp_canon_moviesize_known_body(const char *model)
{
	return find_body(model) != NULL;
}

int
gp_canon_moviesize_decode(const char *model, uint32_t packed, gp_canon_movsize *out)
{
	gp_canon_movsize tmp = { 0, 0, 0, 0 };
	const bodyprofile *b = find_body(model);

	tmp.fps_x100    = (int)(packed & 0xFFFF);         /* fps is generic       */
	tmp.compression = (int)((packed >> 24) & 0xFF);   /* MovieParam5 word 5   */
	int have = 0;
	if (b) {
		int code = (int)((packed >> 16) & 0xFF);
		for (int i = 0; i < b->nres; i++)
			if (b->res[i].code == code) {
				tmp.width  = b->res[i].w;
				tmp.height = b->res[i].h;
				have = 1;
				break;
			}
	}
	if (out) *out = tmp;
	return have;
}

int
gp_canon_moviesize_encode(const char *model, int width, int height, int fps_x100,
                          int compression, uint32_t *packed)
{
	const bodyprofile *b = find_body(model);
	uint32_t code = 0;
	int have = 0;
	if (b) {
		for (int i = 0; i < b->nres; i++)
			if (b->res[i].w == width && b->res[i].h == height) {
				code = (uint32_t)b->res[i].code;
				have = 1;
				break;
			}
	}
	if (packed) *packed = ((uint32_t)compression << 24) | ((code & 0xFF) << 16) |
	                      ((uint32_t)fps_x100 & 0xFFFF);
	return have;
}
