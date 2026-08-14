/*
 * smoke_test.c — validates the static-registration build natively (macOS arm64).
 *
 * If this links and prints the ptp2 model count + a few Canon EOS entries, then
 * the curated iOS source set + the stub ltdl loader are wired correctly (the same
 * portable C runs identically on iOS arm64).
 */
#include <stdio.h>
#include <string.h>

#include <gphoto2/gphoto2-camera.h>
#include <gphoto2/gphoto2-abilities-list.h>
#include <gphoto2/gphoto2-context.h>
#include <gphoto2/gphoto2-version.h>
#include "gp_ios_register.h"
#include "gp_canon_imagesize.h"

/* Exercise the Canon size-class → pixel-dimension resolver against known-good values
 * (R50 measured on-body at 4:3; 5D Mk III + R5 from the seeded table). Returns the number
 * of failing cases (0 == all good). */
static int
check_imagesize(void)
{
	static const struct {
		const char *model, *label, *aspect;
		int n, w0, h0, w1, h1;
	} cases[] = {
		/* R50 @ 4:3, measured on-body — RAW stays full-sensor, only the JPEG is cropped */
		{ "Canon EOS R50",        "cRAW + L", "4:3",  2, 6000, 4000, 5328, 4000 },
		{ "Canon EOS R50",        "M",        "4:3",  1, 3552, 2664,    0,    0 },
		{ "Canon EOS R50",        "S1",       "4:3",  1, 2656, 1992,    0,    0 },
		{ "Canon EOS R50",        "RAW",      "4:3",  1, 6000, 4000,    0,    0 }, /* RAW aspect-invariant */
		{ "Canon EOS R50",        "L",        "3:2",  1, 6000, 4000,    0,    0 },
		/* 5D Mk III — dual full-res, plus a derived 16:9 crop of L */
		{ "Canon EOS 5D Mark III","cRAW + L", "3:2",  2, 5760, 3840, 5760, 3840 },
		{ "Canon EOS 5D Mark III","L",        "16:9", 1, 5760, 3240,    0,    0 },
		/* R5 — 1.6x is a real sensor crop (override); model-name normalization still matches */
		{ "EOS-R5",               "RAW",      "1.6x", 1, 5088, 3392,    0,    0 },
	};

	int fails = 0;
	for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		gp_canon_pixsize o[2] = { {0,0}, {0,0} };
		int n = gp_canon_imagesize_resolve(cases[i].model, cases[i].label,
		                                   cases[i].aspect, o, 2);
		int ok = (n == cases[i].n) &&
		         o[0].width == cases[i].w0 && o[0].height == cases[i].h0 &&
		         o[1].width == cases[i].w1 && o[1].height == cases[i].h1;
		printf("  %-4s %-8s %-4s -> %4dx%-4d + %4dx%-4d  [%s]\n",
		       ok ? "ok" : "FAIL", cases[i].label, cases[i].aspect,
		       o[0].width, o[0].height, o[1].width, o[1].height, cases[i].model);
		if (!ok) fails++;
	}

	/* known-body detection: recognized vs. not */
	if (!gp_canon_imagesize_known_body("Canon EOS R50")) { printf("  FAIL known_body(R50)\n"); fails++; }
	if ( gp_canon_imagesize_known_body("Nikon D6"))       { printf("  FAIL known_body(Nikon D6)\n"); fails++; }

	return fails;
}

int
main(void)
{
	const char **v = gp_library_version(GP_VERSION_SHORT);
	printf("libgphoto2 version: %s\n", (v && v[0]) ? v[0] : "?");

	gp_ios_register_all();

	GPContext *ctx = gp_context_new();
	CameraAbilitiesList *al = NULL;
	int ret = gp_abilities_list_new(&al);
	if (ret < GP_OK) { printf("FAIL gp_abilities_list_new: %d\n", ret); return 1; }

	ret = gp_abilities_list_load(al, ctx);
	if (ret < GP_OK) { printf("FAIL gp_abilities_list_load: %d\n", ret); return 1; }

	int n = gp_abilities_list_count(al);
	printf("registered camera models: %d\n", n);
	if (n <= 0) { printf("FAIL: no models registered (static ltdl not wired?)\n"); return 1; }

	int shown = 0;
	for (int i = 0; i < n && shown < 6; i++) {
		CameraAbilities a;
		if (gp_abilities_list_get_abilities(al, i, &a) == GP_OK &&
		    strstr(a.model, "EOS R") != NULL) {
			printf("  • %-28s usb %04x:%04x  lib=%s\n",
			       a.model, a.usb_vendor, a.usb_product, a.library);
			shown++;
		}
	}

	Camera *cam = NULL;
	ret = gp_camera_new(&cam);
	printf("gp_camera_new: %s\n", ret == GP_OK ? "ok" : "FAILED");
	if (cam) gp_camera_free(cam);

	gp_abilities_list_free(al);
	gp_context_unref(ctx);

	printf("image-size resolver:\n");
	int isf = check_imagesize();
	if (isf) { printf("FAIL: %d image-size case(s) wrong\n", isf); return 1; }

	printf("SMOKE TEST OK\n");
	return 0;
}
