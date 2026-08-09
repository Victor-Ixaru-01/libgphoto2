/*
 * gp_ios_register.h — static registration of the built-in libgphoto2 modules.
 *
 * On iOS there is no dlopen, so the camlibs/iolibs are compiled into the binary
 * and registered by name at startup. Call gp_ios_register_all() ONCE, before any
 * gp_abilities_list_load() / gp_camera_init().
 */
#ifndef GP_IOS_REGISTER_H
#define GP_IOS_REGISTER_H

#ifdef __cplusplus
extern "C" {
#endif

void gp_ios_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* GP_IOS_REGISTER_H */
