/*
 * ltdl_static.c — a minimal static module loader standing in for libtool ltdl.
 *
 * See ltdl.h. Modules are registered at startup (gp_static_register_module);
 * lt_dlforeachfile enumerates them, lt_dlopenext returns a handle by name, and
 * lt_dlsym resolves a symbol from the module's static symbol table. No dlopen.
 */
#include "ltdl.h"
#include <string.h>

#define GP_LT_MAX_MODULES 8

struct gp_lt_module {
	const char        *name;
	const gp_static_sym *syms;
	int                 in_use;
};

static struct gp_lt_module g_modules[GP_LT_MAX_MODULES];
static int g_module_count = 0;

void
gp_static_register_module(const char *name, const gp_static_sym *syms)
{
	int i;
	if (!name || !syms)
		return;
	/* de-dup by name */
	for (i = 0; i < g_module_count; i++)
		if (!strcmp(g_modules[i].name, name))
			return;
	if (g_module_count >= GP_LT_MAX_MODULES)
		return;
	g_modules[g_module_count].name = name;
	g_modules[g_module_count].syms = syms;
	g_modules[g_module_count].in_use = 0;
	g_module_count++;
}

int lt_dlinit(void)  { return 0; }
int lt_dlexit(void)  { return 0; }
int lt_dladdsearchdir(const char *search_dir) { (void)search_dir; return 0; }

const char *
lt_dlerror(void)
{
	return "libgphoto2 iOS build uses static module registration (no dynamic loading)";
}

lt_dlhandle
lt_dlopenext(const char *filename)
{
	const char *base;
	int i;
	if (!filename)
		return (lt_dlhandle)0;
	base = strrchr(filename, '/');
	base = base ? base + 1 : filename;
	for (i = 0; i < g_module_count; i++) {
		/* match on full path, basename, or basename-contains-modulename
		 * (handles "ptp2", "ptp2.so", "/path/ptp2.la", etc.) */
		if (!strcmp(g_modules[i].name, filename) ||
		    !strcmp(g_modules[i].name, base) ||
		    strstr(base, g_modules[i].name) != 0) {
			g_modules[i].in_use = 1;
			return &g_modules[i];
		}
	}
	return (lt_dlhandle)0;
}

int
lt_dlclose(lt_dlhandle handle)
{
	if (handle)
		handle->in_use = 0;
	return 0;
}

lt_ptr
lt_dlsym(lt_dlhandle handle, const char *name)
{
	const gp_static_sym *s;
	if (!handle || !name)
		return (lt_ptr)0;
	for (s = handle->syms; s && s->name; s++)
		if (!strcmp(s->name, name))
			return s->addr;
	return (lt_ptr)0;
}

int
lt_dlforeachfile(const char *search_path, lt_dlforeachfile_func func, lt_ptr data)
{
	int i;
	(void)search_path;
	if (!func)
		return 0;
	for (i = 0; i < g_module_count; i++)
		if (func(g_modules[i].name, data))
			return 1;   /* non-zero: caller asked to stop */
	return 0;
}
