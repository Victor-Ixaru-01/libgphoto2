/*
 * gp_ios_register.c — register the statically-linked libgphoto2 modules.
 *
 * Phase 1: only the ptp2 camlib (the modern PTP/MTP driver: all Canon EOS, Nikon,
 * Sony, etc.). Phase 2 will additionally register a custom "iccamera" iolib whose
 * read/write bridge to ICCameraDevice.requestSendPTPCommand.
 */
#include "ltdl.h"
#include "gp_ios_register.h"

#include <gphoto2/gphoto2-library.h>   /* declares camera_id / camera_abilities / camera_init */

/* The three standard camlib entry points exported by camlibs/ptp2/library.c.
 * Cast to lt_ptr the same way dlsym would hand them back. */
static const gp_static_sym ptp2_syms[] = {
	{ "camera_id",        (lt_ptr)camera_id },
	{ "camera_abilities", (lt_ptr)camera_abilities },
	{ "camera_init",      (lt_ptr)camera_init },
	{ 0, 0 }
};

void
gp_ios_register_all(void)
{
	gp_static_register_module("ptp2", ptp2_syms);
}
