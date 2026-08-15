/*
 * gp_canon_moviesize.h — decode/encode Canon EOS movie recording size (resolution + fps).
 *
 * The size is carried by a body-specific device property holding a struct:
 *   EOS R50   → 0xD20D (MovieParam5), a 40-byte struct { size=40, fps*100, rescode, ... }.
 *   EOS R50 V → 0xD20D absent; carrier TBD (discover with gp_iccamera_watch_prop_changes()).
 *
 * The ptp2 driver (ptp-pack.c) condenses that struct into a u32 for the config layer:
 *     packed = (rescode << 16) | (fps*100 & 0xffff)
 * This module maps that packed value ↔ {width, height, fps} per body. fps is generic
 * (fps*100 = low 16 bits); only the small resolution-code → dimensions table is per body.
 * Verified on the R50: rescode 0 = 1920x1080, 5 = 3840x2160; fps 2500/5000/2500/10000 for
 * 4K25p / FHD50p / FHD25p / FHD100p. See docs-architecture/canon-movie-recording-size.md.
 *
 * Pure data/logic — no camera handle. gp_iccamera_get_movie_size()/_set_movie_size() do the I/O.
 */
#ifndef GP_CANON_MOVIESIZE_H
#define GP_CANON_MOVIESIZE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compression variant — MovieParam5 word 5, confirmed on an EOS R50 by toggling it. */
#define GP_CANON_IPB_STANDARD 0
#define GP_CANON_IPB_LIGHT    1

typedef struct {
	int width;      /* 0 if the resolution code isn't mapped for this body */
	int height;
	int fps_x100;   /* frame rate ×100 (2500 = 25.00p, 10000 = 100.00p)     */
	int compression;/* GP_CANON_IPB_* — the R50's IPB Standard/Light axis.    */
	                /* Always 0 on bodies without one (e.g. the R50 V, whose  */
	                /* variants live in the recording format, 0xD257).        */
} gp_canon_movsize;

/* Bit depth and gamma are NOT part of the movie size — on the R50 they follow HDR PQ
 * (0xD20C): 10-bit/PQ when it is on, 8-bit/standard when off. Read 0xD20C with
 * gp_iccamera_get_eosprop() for those; see docs-architecture/canon-movie-remaining-time.md. */

/* The device-property code carrying the movie recording size on `model` (0xD20D on the EOS R50).
 * 0 when the body isn't in the table or its carrier isn't identified yet (e.g. EOS R50 V). */
uint16_t gp_canon_moviesize_prop(const char *model);

/* Non-zero if the body is registered (even if its carrier/resolution codes are pending). */
int gp_canon_moviesize_known_body(const char *model);

/* Decode the driver's packed value → *out. Packed layout:
 *   bits  0..15  fps × 100
 *   bits 16..23  resolution code
 *   bits 24..31  compression variant (GP_CANON_IPB_*)
 * fps and compression are always set. Returns 1 if the resolution code is known for this body
 * (width/height set), else 0 (dims 0). */
int gp_canon_moviesize_decode(const char *model, uint32_t packed, gp_canon_movsize *out);

/* Encode a desired size → *packed for setting. `compression` is GP_CANON_IPB_* (pass
 * GP_CANON_IPB_STANDARD on bodies without that axis). Returns 1 if (width,height) maps to a
 * known resolution code for the body; 0 otherwise (*packed still carries fps + compression
 * with rescode 0). */
int gp_canon_moviesize_encode(const char *model, int width, int height, int fps_x100,
                              int compression, uint32_t *packed);

static inline double
gp_canon_movsize_fps(const gp_canon_movsize *m)
{
	return m ? (double)m->fps_x100 / 100.0 : 0.0;
}

#ifdef __cplusplus
}
#endif

#endif /* GP_CANON_MOVIESIZE_H */
