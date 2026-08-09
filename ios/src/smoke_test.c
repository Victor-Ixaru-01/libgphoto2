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

	printf("SMOKE TEST OK\n");
	return 0;
}
