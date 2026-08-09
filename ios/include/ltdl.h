/*
 * ltdl.h — drop-in replacement for libtool's <ltdl.h> on iOS.
 *
 * iOS forbids dlopen() of app-external code, so libgphoto2's dynamic camlib/iolib
 * discovery cannot work. This header + ltdl_static.c provide a tiny STATIC loader:
 * modules (ptp2, and later a custom iolib) are compiled into the binary and
 * registered by name; lt_dlopenext/lt_dlsym then resolve against that registry
 * instead of the filesystem. The unmodified libgphoto2 core keeps working.
 */
#ifndef GP_STATIC_LTDL_H
#define GP_STATIC_LTDL_H

typedef void *lt_ptr;
typedef struct gp_lt_module *lt_dlhandle;

/* One exported symbol of a statically-registered module. Terminate arrays with {0,0}. */
typedef struct { const char *name; lt_ptr addr; } gp_static_sym;

/* Register a module (e.g. "ptp2") and its symbol table. Call before gp_*_load. */
void gp_static_register_module(const char *name, const gp_static_sym *syms);

/* libtool-ltdl API surface actually used by libgphoto2. */
int          lt_dlinit(void);
int          lt_dlexit(void);
int          lt_dladdsearchdir(const char *search_dir);
lt_dlhandle  lt_dlopenext(const char *filename);
int          lt_dlclose(lt_dlhandle handle);
lt_ptr       lt_dlsym(lt_dlhandle handle, const char *name);
const char  *lt_dlerror(void);

typedef int (*lt_dlforeachfile_func)(const char *filename, lt_ptr data);
int          lt_dlforeachfile(const char *search_path, lt_dlforeachfile_func func, lt_ptr data);

#endif /* GP_STATIC_LTDL_H */
