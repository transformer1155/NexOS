/* plugin_manager.h - NexOS plugin manager (freestanding, no stdlib)
 *
 * Architecture (Phase 1):
 *   - Plugins are statically linked .o files that export a const Plugin
 *     descriptor (hand-written vtable via function pointers, NOT C++ virtual).
 *   - The manager loads/orders/unloads plugins and hosts a service registry
 *     keyed by a 32-bit FNV-1a hash (the originating string is kept for
 *     diagnostics / collision checks).
 *
 * Constraints honoured: freestanding, no C++ stdlib, no new/delete
 * (kmalloc/kfree only), no exceptions, no RTTI, no virtual.
 */
#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PM_MAX_PLUGINS 64
#define PM_SVC_SLOTS   256   /* service registry capacity (open addressing) */

/* ---- Plugin interface (hand-written vtable) -------------------------------
 * A plugin is just a const struct of function pointers plus metadata.
 * init()  : called once, in dependency order. Registers services.
 * exit()  : called once, in reverse dependency order. Unregisters services.
 * call()  : invoke a named method. args is plugin-defined; out/outcap is a
 *           return buffer. Returns 0 on success, <0 on error/unknown method.
 */
struct Plugin;

typedef int  (*plugin_init_fn)(struct Plugin* self);
typedef void (*plugin_exit_fn)(struct Plugin* self);
typedef int  (*plugin_call_fn)(struct Plugin* self, const char* method,
                               void* args, void* out, int outcap);

typedef struct Plugin {
    const char*     name;      /* unique id, e.g. "hello_world"            */
    uint32_t        version;   /* 0x0100 == 1.0                            */
    const char*     provides;  /* comma-separated service names this offers*/
    const char*     deps;      /* comma-separated plugin names (deps)      */
    plugin_init_fn  init;
    plugin_exit_fn  exit;
    plugin_call_fn  call;
} Plugin;

/* A service is a named function pointer registered into the global registry. */
typedef int (*svc_fn)(void* args, void* out, int outcap);

/* A reload hook: given raw .bc bytes (NBC1), rebind the plugin's behaviour.
 * Return 0 on success, <0 on failure.  Used by `plugin pm reload` (Phase 5). */
typedef int (*plugin_reload_fn)(const uint8_t* bc, int len);

/* Register a runtime reload hook for an already-registered plugin. */
int pm_set_reload(const char* name, plugin_reload_fn fn);

/* Hot-reload a plugin from /src/<name>.bc (preferred) or /src/<name>.skill
 * packed in MKFS/SFS.  Returns 0 on success, <0 if not found / no hook. */
int pm_reload(const char* name);

/* ---- Manager lifecycle ---------------------------------------------------- */
int  pm_init(void);                       /* reset internal state             */
int  pm_register(const Plugin* p);        /* register a plugin descriptor     */
int  pm_resolve(void);                    /* topo-sort deps, detect cycles    */
int  pm_load_all(void);                   /* init() in dependency order       */
void pm_unload_all(void);                 /* exit() in reverse order          */
const Plugin* pm_find(const char* name);  /* find a registered plugin         */
int  pm_call(const char* plugin, const char* method,
             void* args, void* out, int outcap);

/* ---- Service registry ----------------------------------------------------- */
int  svc_register(const char* name, svc_fn fn);
svc_fn svc_lookup(const char* name);
int  svc_unregister(const char* name);
void svc_clear(void);

/* ---- Debug serial (plugins may use this; COM1, 0x3F8) --------------------- */
void pm_serial(const char* s);
void pm_serial_byte(uint8_t b);

/* bounded strcmp, shared by manager + plugins (no libc) */
int  pm_strcmp(const char* a, const char* b);

/* ---- Boot entry (called once from kmain after heap_init) ------------------ */
void plugin_manager_boot(void);

#ifdef __cplusplus
}
#endif
#endif /* PLUGIN_MANAGER_H */
