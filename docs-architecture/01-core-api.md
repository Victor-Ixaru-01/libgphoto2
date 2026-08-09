# 01 — Core Library API (`libgphoto2/` + `gphoto2/` headers)

The core is ~10,000 lines of C. It defines the public API, the object model, the caching
filesystem, the config-widget system, and the plugin loader — but **contains no
camera- or bus-specific code**. Every function below returns an `int` gphoto2 code
(`GP_OK`/`0` or positive on success, negative `GP_ERROR_*` on failure) unless noted.

Files:

| File | Lines | Role |
|------|------:|------|
| `gphoto2-camera.c` | 1954 | The `Camera` object; wraps + dispatches every operation to the driver vtable and the filesystem. |
| `gphoto2-filesys.c` | 2481 | Caching virtual filesystem with LRU; the largest core file. |
| `gphoto2-file.c` | 1169 | `CameraFile` blob container (memory / fd / handler backed). |
| `gphoto2-abilities-list.c` | 757 | Supported-model DB; camlib loading; USB autodetect. |
| `gphoto2-widget.c` | 835 | `CameraWidget` config-tree nodes. |
| `gphoto2-context.c` | 415 | `GPContext` callback dispatch. |
| `gphoto2-list.c` | 397 | `CameraList` (name,value) pairs. |
| `gphoto2-setting.c` | 362 | Persistent `~/.gphoto/settings` key/value store. |
| `gphoto2-version.c` | 125 | Version/feature strings. |
| `gphoto2-library.c` | 88 | Stub definitions of the camlib entry points. |
| `gphoto2-result.c` | 79 | `gp_result_as_string()` error-code → text. |
| `bayer.c`, `ahd_bayer.c`, `jpeg.c`, `gamma.c`, `exif.c` | ~1600 | Image-decode helpers used by *some* legacy drivers; not on the mirrorless path. Skipped here. |

---

## 1. Camera — `gphoto2-camera.c`

The lifecycle and every user-facing operation. Internally each public op follows the
same pattern: `CHECK_INIT` (auto-init if needed, bump the "used" count) → `CHECK_OPEN`
(open port + run driver `pre_func`) → call the driver vtable entry or a filesystem
function → `CHECK_CLOSE` (run `post_func`) → `CAMERA_UNUSED` (drop the count, honoring a
deferred exit). Understanding that wrapper is understanding this file.

### Lifecycle & setup

| Function | Signature | In → Out | Notes |
|----------|-----------|----------|-------|
| `gp_camera_new` | `(Camera **camera)` | `[out]` new camera → code | Allocates the Camera, its `CameraFunctions`, core-private, a fresh `CameraFilesystem` and `GPPort`. refcount=1. |
| `gp_camera_ref` | `(Camera *camera)` | — | +1 refcount. |
| `gp_camera_unref` | `(Camera *camera)` | — | −1 refcount; frees at 0 **if not currently in use**. |
| `gp_camera_free` | `(Camera *camera)` | — | Deprecated hard free (exits connection, frees port/fs/pc). Prefer `unref`. |
| `gp_camera_set_abilities` | `(Camera*, CameraAbilities)` | — | Selects the model/driver. Call before init unless you want autodetect. Closes any live connection first. |
| `gp_camera_get_abilities` | `(Camera*, CameraAbilities* )` | `[out]` abilities | Copies the current abilities struct out. |
| `gp_camera_set_port_info` | `(Camera*, GPPortInfo)` | — | Binds the camera to a specific port (path/name/type). Closes any live connection first. |
| `gp_camera_get_port_info` | `(Camera*, GPPortInfo* )` | `[out]` info | |
| `gp_camera_set_port_speed` | `(Camera*, int speed)` | — | **Serial only.** Forces baud; normally the driver auto-negotiates. Irrelevant to USB/PTP. |
| `gp_camera_get_port_speed` | `(Camera*)` | → speed | Returns the speed (not a normal error code). |

### Connection

| Function | Signature | In → Out | Notes |
|----------|-----------|----------|-------|
| `gp_camera_autodetect` | `(CameraList *list, GPContext*)` | `[out]` list of (model, port) → count or code | Full scan: loads all iolibs+camlibs, returns every present, recognized USB camera. The convenient one-call entry point. |
| `gp_camera_init` | `(Camera*, GPContext*)` | — | Establish the connection: autodetect if unset → bind USB device → dlopen camlib → call driver `camera_init`. Idempotent-ish via the "used" guard. |
| `gp_camera_exit` | `(Camera*, GPContext*)` | — | Close connection, run driver `exit`, dlclose the camlib, reset the fs cache. Deferred if the camera is mid-operation. |

### Configuration (see the CameraWidget section for the tree itself)

| Function | Signature | In → Out | Notes |
|----------|-----------|----------|-------|
| `gp_camera_get_config` | `(Camera*, CameraWidget **window, GPContext*)` | `[out]` root widget | Whole config tree. `GP_ERROR_NOT_SUPPORTED` if the driver has no config. |
| `gp_camera_set_config` | `(Camera*, CameraWidget *window, GPContext*)` | — | Hands the (edited) tree back to the driver to apply. |
| `gp_camera_list_config` | `(Camera*, CameraList*, GPContext*)` | `[out]` widget names | Flat list of every settable widget name. If the driver lacks a native `list_config`, the core walks the tree to synthesize it. |
| `gp_camera_get_single_config` | `(Camera*, const char *name, CameraWidget**, GPContext*)` | `[out]` one widget | Fast single-setting fetch. If the driver has no native single-get, the core builds the full tree and **duplicates** the one child out (see `gphoto2-camera.c:887`). |
| `gp_camera_set_single_config` | `(Camera*, const char *name, CameraWidget*, GPContext*)` | — | Fast single-setting write; falls back to get-tree/patch/set-tree. |

### Capture & events

| Function | Signature | In → Out | Notes |
|----------|-----------|----------|-------|
| `gp_camera_capture` | `(Camera*, CameraCaptureType, CameraFilePath *path, GPContext*)` | `[out]` path on card | Synchronous shutter; type = IMAGE / MOVIE / SOUND. Returns where the file landed. |
| `gp_camera_trigger_capture` | `(Camera*, GPContext*)` | — | Fire and return immediately; pair with `wait_for_event`. Best for bursts/tethering. |
| `gp_camera_capture_preview` | `(Camera*, CameraFile*, GPContext*)` | `[out]` frame in file | Liveview/viewfinder grab; not stored on the camera. |
| `gp_camera_wait_for_event` | `(Camera*, int timeout_ms, CameraEventType *type, void **data, GPContext*)` | `[out]` event type + data | Block up to `timeout_ms`. Type ∈ {UNKNOWN, TIMEOUT, FILE_ADDED, FOLDER_ADDED, CAPTURE_COMPLETE, FILE_CHANGED}. For FILE/FOLDER events, `*data` is a heap `CameraFilePath*` the caller frees. Call repeatedly in a loop. |
| `gp_camera_get_storageinfo` | `(Camera*, CameraStorageInformation **sifs, int *nrof, GPContext*)` | `[out]` array + count | Per-storage capacity/free/label/type. **Caller frees the array.** |

### Textual info

| Function | Signature | Notes |
|----------|-----------|-------|
| `gp_camera_get_summary` | `(Camera*, CameraText *summary, GPContext*)` | Current status (shots, battery, model specifics), translated. |
| `gp_camera_get_manual` | `(Camera*, CameraText *manual, GPContext*)` | Driver usage notes. |
| `gp_camera_get_about` | `(Camera*, CameraText *about, GPContext*)` | Author/credits. |

`CameraText` is just `{ char text[32*1024]; }`.

### Folder operations (thin wrappers over the filesystem)

| Function | Signature | Notes |
|----------|-----------|-------|
| `gp_camera_folder_list_files` | `(Camera*, const char *folder, CameraList*, GPContext*)` | Sorted file names in `folder`. |
| `gp_camera_folder_list_folders` | `(Camera*, const char *folder, CameraList*, GPContext*)` | Sorted subfolders. |
| `gp_camera_folder_delete_all` | `(Camera*, const char *folder, GPContext*)` | Bulk delete. |
| `gp_camera_folder_put_file` | `(Camera*, const char *folder, const char *filename, CameraFileType, CameraFile*, GPContext*)` | Upload a file to the camera. |
| `gp_camera_folder_make_dir` | `(Camera*, const char *folder, const char *name, GPContext*)` | Create directory. |
| `gp_camera_folder_remove_dir` | `(Camera*, const char *folder, const char *name, GPContext*)` | Remove empty directory. |

### File operations

| Function | Signature | In → Out | Notes |
|----------|-----------|----------|-------|
| `gp_camera_file_get_info` | `(Camera*, folder, file, CameraFileInfo *info, ctx)` | `[out]` info | Size/type/dimensions/mtime/permissions/status. Falls back to fetching the preview to infer info if the driver can't report it. |
| `gp_camera_file_set_info` | `(Camera*, folder, file, CameraFileInfo info, ctx)` | — | Rename / change permissions where supported. |
| `gp_camera_file_get` | `(Camera*, folder, file, CameraFileType, CameraFile*, ctx)` | `[out]` file data | The main **download**. `type` picks the view (NORMAL/PREVIEW/EXIF/…). |
| `gp_camera_file_read` | `(Camera*, folder, file, CameraFileType, uint64_t offset, char *buf, uint64_t *size, ctx)` | `[in/out]` size | **Partial/streaming** read into a caller buffer; `*size` is buffer size in, bytes read out. For files too big to hold whole. |
| `gp_camera_file_delete` | `(Camera*, folder, file, ctx)` | — | Delete one file. |

### Keep-alive timeouts

Some cameras drop the link if idle. The driver registers periodic pings; the frontend
supplies the actual timer implementation (it owns the event loop).

| Function | Signature | Notes |
|----------|-----------|-------|
| `gp_camera_set_timeout_funcs` | `(Camera*, CameraTimeoutStartFunc, CameraTimeoutStopFunc, void *data)` | Frontend installs its timer start/stop. Returns void. |
| `gp_camera_start_timeout` | `(Camera*, unsigned int seconds, CameraTimeoutFunc)` | Called **by the driver**; returns an id or error. |
| `gp_camera_stop_timeout` | `(Camera*, unsigned int id)` | Driver cancels a timer. Void. |

---

## 2. AbilitiesList — `gphoto2-abilities-list.c`

The supported-camera database and the camlib loader/autodetector.

| Function | Signature | In → Out | Notes |
|----------|-----------|----------|-------|
| `gp_abilities_list_new` | `(CameraAbilitiesList **list)` | `[out]` list | |
| `gp_abilities_list_free` | `(CameraAbilitiesList*)` | — | |
| `gp_abilities_list_load` | `(CameraAbilitiesList*, GPContext*)` | — | Load **all** camlibs from the `CAMLIBS` dir and harvest their models, then sort. |
| `gp_abilities_list_load_dir` | `(CameraAbilitiesList*, const char *dir, GPContext*)` | — | Same, from a specific dir. Under the hood: `lt_dlforeachfile` → dlopen each → `camera_id` (dedup) → `camera_abilities` (append), stamping the `.so` filename + id into each entry. |
| `gp_abilities_list_reset` | `(CameraAbilitiesList*)` | — | Empty the list. |
| `gp_abilities_list_detect` | `(CameraAbilitiesList*, GPPortInfoList*, CameraList *out, GPContext*)` | `[out]` detected | Cross the model DB against present ports (USB vendor/product match via the port layer) → list of `(model, port)`. |
| `gp_abilities_list_append` | `(CameraAbilitiesList*, CameraAbilities)` | — | Add one model manually. |
| `gp_abilities_list_count` | `(CameraAbilitiesList*)` | → count | |
| `gp_abilities_list_lookup_model` | `(CameraAbilitiesList*, const char *model)` | → index or `GP_ERROR_MODEL_NOT_FOUND` | Case-sensitive exact model name. |
| `gp_abilities_list_get_abilities` | `(CameraAbilitiesList*, int index, CameraAbilities *out)` | `[out]` abilities | Copy one entry out (feed to `gp_camera_set_abilities`). |
| `gp_message_codeset` | `(const char *codeset)` | → prev codeset | Set charset for translated messages. |
| `gp_init_localedir` | `(const char *localedir)` | — | Point gettext at a locale dir. |

Internal statics of interest: `gp_abilities_list_detect_usb` (the per-model USB probe
loop), `gp_abilities_list_sort` / `cmp_abilities`, `gp_abilities_list_lookup_id`.

### The `CameraAbilities` struct (what a driver declares per model)

```
char model[128];                 // "Canon EOS R5"
CameraDriverStatus status;       // PRODUCTION / TESTING / EXPERIMENTAL / DEPRECATED
GPPortType port;                 // bitmask of supported bus types (GP_PORT_USB | …)
int speed[64];                   // serial speeds, 0-terminated (serial only)
CameraOperation operations;      // CAPTURE_IMAGE|VIDEO|PREVIEW|CONFIG|TRIGGER_CAPTURE|…
CameraFileOperation file_operations;    // DELETE|PREVIEW|RAW|AUDIO|EXIF
CameraFolderOperation folder_operations;// DELETE_ALL|PUT_FILE|MAKE_DIR|REMOVE_DIR
int usb_vendor, usb_product, usb_class, usb_subclass, usb_protocol;
char library[1024];              // (core-filled) originating .so
char id[1024];                   // (core-filled) driver id
GphotoDeviceType device_type;    // STILL_CAMERA | AUDIO_PLAYER (MTP)
```

These flags are what drive the capability checks you saw in `camera_abilities` (doc 03):
e.g. only models whose driver sets `GP_OPERATION_CAPTURE_IMAGE` will accept
`gp_camera_capture`.

### The camlib plugin contract

Any camera driver `.so` (ptp2 or otherwise) exports exactly these, declared in
`gphoto2-library.h`:

```c
int camera_id        (CameraText *id);            // globally-unique short id, e.g. "PTP"
int camera_abilities (CameraAbilitiesList *list); // append one CameraAbilities per supported model
int camera_init      (Camera *camera, GPContext*);// fill camera->functions + register fs funcs
```

`gphoto2-library.c` only holds stub prototypes; the real bodies live in each driver
(for mirrorless: `camlibs/ptp2/library.c`).

---

## 3. CameraFilesystem — `gphoto2-filesys.c`

A caching, path-validating VFS. The public API is what frontends and `gp_camera_*` use;
the `*_func` typedefs are what **drivers register** to provide the real data. Paths are
always **absolute** (`GP_ERROR_PATH_NOT_ABSOLUTE` otherwise).

### Lifecycle & driver registration

| Function | Signature | Notes |
|----------|-----------|-------|
| `gp_filesystem_new` | `(CameraFilesystem **fs)` | |
| `gp_filesystem_free` | `(CameraFilesystem*)` | |
| `gp_filesystem_reset` | `(CameraFilesystem*)` | Drop the whole cache (done on `gp_camera_exit`). |
| `gp_filesystem_set_funcs` | `(CameraFilesystem*, CameraFilesystemFuncs*, void *data)` | **The driver's hook-in.** Registers the 12 callbacks below in one struct. |

`CameraFilesystemFuncs` members (each a driver-supplied callback):
`file_list_func`, `folder_list_func`, `get_info_func`, `set_info_func`,
`get_file_func`, `read_file_func`, `del_file_func`, `put_file_func`,
`delete_all_func`, `make_dir_func`, `remove_dir_func`, `storage_info_func`.

### Public operations (used by the `gp_camera_*` wrappers)

| Function | Signature | Notes |
|----------|-----------|-------|
| `gp_filesystem_list_files` | `(fs, folder, CameraList*, ctx)` | Cached; calls `file_list_func` on miss. |
| `gp_filesystem_list_folders` | `(fs, folder, CameraList*, ctx)` | Cached; calls `folder_list_func`. |
| `gp_filesystem_count` | `(fs, folder, ctx)` | File count in a folder. |
| `gp_filesystem_name` | `(fs, folder, int n, const char **name, ctx)` | Nth file name. |
| `gp_filesystem_number` | `(fs, folder, filename, ctx)` | Index of a named file. |
| `gp_filesystem_get_folder` | `(fs, filename, char **folder, ctx)` | Which folder holds a file (scans the tree). |
| `gp_filesystem_get_file` | `(fs, folder, filename, CameraFileType, CameraFile*, ctx)` | The cached **download**; calls `get_file_func` on miss and stores in the LRU. |
| `gp_filesystem_read_file` | `(fs, folder, filename, type, uint64_t offset, char *buf, uint64_t *size, ctx)` | Partial read; calls `read_file_func`. |
| `gp_filesystem_put_file` | `(fs, folder, filename, type, CameraFile*, ctx)` | Upload; calls `put_file_func`. |
| `gp_filesystem_delete_file` | `(fs, folder, filename, ctx)` | Delete + evict from cache. |
| `gp_filesystem_delete_all` | `(fs, folder, ctx)` | Bulk delete (native `delete_all_func`, else one-by-one). |
| `gp_filesystem_make_dir` / `gp_filesystem_remove_dir` | `(fs, folder, name, ctx)` | Directory ops. |
| `gp_filesystem_get_info` | `(fs, folder, filename, CameraFileInfo*, ctx)` | Cached file info; calls `get_info_func`. |
| `gp_filesystem_set_info` | `(fs, folder, filename, CameraFileInfo, ctx)` | Rename/permissions; calls `set_info_func`. |
| `gp_filesystem_get_storageinfo` | `(fs, CameraStorageInformation**, int*, ctx)` | Storage array (caller frees). |

### Cache-maintenance / manual editing (used by drivers)

| Function | Notes |
|----------|-------|
| `gp_filesystem_append` | Insert a known folder/file into the cache without a listing round-trip (drivers add captured files this way). |
| `gp_filesystem_set_info_noop` | Update cached info after the driver already changed it on-camera. |
| `gp_filesystem_set_info_dirty` | Mark info stale so it's refetched. |
| `gp_filesystem_set_file_noop` | Seed the cache with file content the driver already has (e.g. just-captured image). |
| `gp_filesystem_delete_file_noop` | Evict a cache entry the driver already deleted. |
| `gp_filesystem_dump` | Debug: print the whole tree. |

Internal LRU/caching machinery: `gp_filesystem_lru_update/_clear/_remove_one/_free/_count/_check`,
`get_exif_mtime`/`gp_filesystem_get_exif_mtime` (libexif timestamp recovery),
`gp_filesystem_scan`/`recursive_folder_scan`, and the tree helpers
(`append_folder`, `append_file`, `lookup_folder`, `delete_all_files`, …).

### Info & storage structs

`CameraFileInfo` bundles `preview`, `file`, and `audio` sub-structs. Each carries a
`fields` bitmask (`GP_FILE_INFO_TYPE|SIZE|WIDTH|HEIGHT|PERMISSIONS|STATUS|MTIME`)
saying which members are valid — **always check `fields`**. `CameraStorageInformation`
mirrors PTP storage descriptors (base dir, label, type, capacity/free in KB and images).

---

## 4. CameraFile — `gphoto2-file.c`

A container for one blob of data plus name/MIME/mtime. Three backends
(`GP_FILE_ACCESSTYPE_MEMORY`, `_FD`, `_HANDLER`).

| Function | Signature | Notes |
|----------|-----------|-------|
| `gp_file_new` | `(CameraFile **file)` | Memory-backed. |
| `gp_file_new_from_fd` | `(CameraFile**, int fd)` | Streams to/from a Unix fd — lets you download straight to a file without buffering in RAM. |
| `gp_file_new_from_handler` | `(CameraFile**, CameraFileHandler*, void *priv)` | Custom read/write/size callbacks. |
| `gp_file_ref` / `gp_file_unref` / `gp_file_free` | `(CameraFile*)` | Refcounting. |
| `gp_file_set_name` / `gp_file_get_name` | | Base filename. |
| `gp_file_set_mime_type` / `gp_file_get_mime_type` | | e.g. `GP_MIME_JPEG`, `GP_MIME_CR3`, `GP_MIME_ARW`. |
| `gp_file_set_mtime` / `gp_file_get_mtime` | | `time_t`. |
| `gp_file_detect_mime_type` | `(CameraFile*)` | Sniff MIME from magic bytes. |
| `gp_file_adjust_name_for_mime_type` | `(CameraFile*)` | Fix extension to match MIME. |
| `gp_file_get_name_by_type` | `(CameraFile*, const char *basename, CameraFileType, char **newname)` | Compose a per-view name (e.g. `thumb_IMG.jpg`). Caller frees. |
| `gp_file_set_data_and_size` | `(CameraFile*, char *data, unsigned long size)` | **Transfers ownership** of `data` to the file (it will `free` it). |
| `gp_file_get_data_and_size` | `(CameraFile*, const char **data, unsigned long *size)` | Borrow a pointer to the bytes (do not free). |
| `gp_file_append` | `(CameraFile*, const char *data, unsigned long size)` | Grow the buffer — drivers stream in chunks. |
| `gp_file_slurp` | `(CameraFile*, char *data, size_t size, size_t *readlen)` | Read a chunk out (for handler/fd files). |
| `gp_file_open` / `gp_file_save` | `(CameraFile*, const char *filename)` | Load from / write to local disk. |
| `gp_file_clean` | `(CameraFile*)` | Reset contents, keep the object. |
| `gp_file_copy` | `(CameraFile *dst, CameraFile *src)` | Deep copy. |

---

## 5. CameraWidget — `gphoto2-widget.c`

Nodes of the configuration tree. Types: `WINDOW` (root), `SECTION` (tab), `TEXT`,
`RANGE` (slider, `float`), `TOGGLE` (`int` 0/1), `RADIO`/`MENU` (choice of strings),
`BUTTON` (callback), `DATE` (`int` epoch).

| Function | Signature | Notes |
|----------|-----------|-------|
| `gp_widget_new` | `(CameraWidgetType, const char *label, CameraWidget**)` | Create a node. |
| `gp_widget_ref`/`unref`/`free` | `(CameraWidget*)` | Refcounting; freeing the root frees children. |
| `gp_widget_append` / `gp_widget_prepend` | `(CameraWidget *parent, CameraWidget *child)` | Build the tree. |
| `gp_widget_count_children` / `gp_widget_get_child` | | Iterate children. |
| `gp_widget_get_child_by_label` / `_by_id` / `_by_name` | | Find a descendant (recursive). |
| `gp_widget_get_root` / `gp_widget_get_parent` | | Navigate up. |
| `gp_widget_get_type` / `gp_widget_get_label` / `gp_widget_get_id` | | Read identity. |
| `gp_widget_set_name` / `gp_widget_get_name` | | The stable programmatic key (e.g. `"iso"`). |
| `gp_widget_set_info` / `gp_widget_get_info` | | Help/description text. |
| `gp_widget_set_value` / `gp_widget_get_value` | `(CameraWidget*, [const] void *value)` | **Type-dependent:** `char*` for TEXT/RADIO/MENU, `float*` for RANGE, `int*` for TOGGLE/DATE. |
| `gp_widget_set_range` / `gp_widget_get_range` | `(CameraWidget*, float low, float high, float step)` | RANGE bounds. |
| `gp_widget_add_choice` / `gp_widget_count_choices` / `gp_widget_get_choice` | | RADIO/MENU options. |
| `gp_widget_changed` / `gp_widget_set_changed` | `(CameraWidget*[, int])` | The "dirty" flag `set_config` uses to know what to write. **Set it after editing.** |
| `gp_widget_set_readonly` / `gp_widget_get_readonly` | | Greyed-out settings. |

---

## 6. GPContext — `gphoto2-context.c`

The frontend's callback bundle. Two sides: **setters** the frontend calls to install
handlers, and **callers** the core/drivers use to report up.

| Setter (frontend installs) | Caller (core/driver reports) | Purpose |
|---|---|---|
| `gp_context_set_progress_funcs(ctx, start, update, stop, data)` | `gp_context_progress_start(ctx, target, fmt, …)` → id; `gp_context_progress_update(ctx, id, current)`; `gp_context_progress_stop(ctx, id)` | Progress bars for long transfers. |
| `gp_context_set_error_func(ctx, fn, data)` | `gp_context_error(ctx, fmt, …)` | Error detail text. |
| `gp_context_set_status_func(ctx, fn, data)` | `gp_context_status(ctx, fmt, …)` | Transient status line. |
| `gp_context_set_message_func(ctx, fn, data)` | `gp_context_message(ctx, fmt, …)` | Modal message. |
| `gp_context_set_question_func(ctx, fn, data)` | `gp_context_question(ctx, fmt, …)` → `GP_CONTEXT_FEEDBACK_OK/CANCEL` | Yes/no prompt. |
| `gp_context_set_cancel_func(ctx, fn, data)` | `gp_context_cancel(ctx)` → feedback | Cooperative cancellation (drivers poll this in long loops). |
| `gp_context_set_idle_func(ctx, fn, data)` | `gp_context_idle(ctx)` | Let the frontend pump its event loop. |

Lifecycle: `gp_context_new()` → returns a `GPContext*`; `gp_context_ref`/`gp_context_unref`.
You can pass `NULL` as a context to most APIs to opt out of callbacks.

---

## 7. Small utilities

### CameraList — `gphoto2-list.c`
`gp_list_new/ref/unref/free`, `gp_list_count`, `gp_list_append(list, name, value)`,
`gp_list_reset`, `gp_list_sort`, `gp_list_find_by_name(list, int *idx, name)`,
`gp_list_get_name/get_value(list, idx, const char**)`,
`gp_list_set_name/set_value(list, idx, str)`,
`gp_list_populate(list, const char *fmt, int count)` (fill with `printf`-formatted
generated names, e.g. `"image%03i.jpg"`).

### Settings — `gphoto2-setting.c`
Persistent key/value in `~/.gphoto/settings`, namespaced by driver id.
`gp_setting_get(id, key, value)` / `gp_setting_set(id, key, value)`; the storage
backend can be overridden with `gp_setting_set_get_func` / `gp_setting_set_set_func`.

### Result strings — `gphoto2-result.c`
`gp_result_as_string(int code)` → human string. High-level codes are in
`gphoto2-result.h` (`GP_ERROR_CORRUPTED_DATA`, `_FILE_EXISTS`, `_MODEL_NOT_FOUND`,
`_DIRECTORY_NOT_FOUND`, `_FILE_NOT_FOUND`, `_CAMERA_BUSY`, `_PATH_NOT_ABSOLUTE`,
`_CANCEL`, `_CAMERA_ERROR`, `_OS_FAILURE`, `_NO_SPACE`); lower-level ones come from the
port layer (doc 02).

### Version — `gphoto2-version.c`
`gp_library_version(GPVersionVerbosity)` → `NULL`-terminated string array (version
plus, in verbose mode, the compiled-in feature flags — gettext, libexif, libxml2, etc.).

---

Continue to [02-port-api.md](02-port-api.md).
