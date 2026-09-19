# plugin.json format

Each plugin directory contains a `plugin.json` describing the plugin to the
build system, the MKFS source viewer (Phase 3), and the `skillc` compiler
(Phase 4+). It is **documentation / metadata** in Phase 1 — the kernel does
not parse it; the plugin's `const Plugin` descriptor (C struct) is what is
actually linked and registered.

```json
{
  "name":     "hello_world",                 // unique plugin id; matches Plugin.name
  "version":  "1.0",                         // human version; Plugin.version = 0x0100
  "provides": ["hello_world.greet"],         // services this plugin offers
  "requires": [],                            // plugin names this depends on (deps)
  "entry":    "g_helloworld",                // symbol name of the const Plugin descriptor
  "author":   "NexOS",
  "desc":     "smoke test"
}
```

## Field rules
- `name`   — must equal the `Plugin.name` in the `.cpp`. Used by the dependency
  resolver (`requires` lists refer to other plugins' `name`).
- `version`— dotted string; encoded as `0xMMmm` into `Plugin.version`
  (e.g. "1.0" -> 0x0100, "1.2" -> 0x0102).
- `provides` — list of service names. The plugin registers these via
  `svc_register()` inside its `init()`.
- `requires` — list of plugin `name`s. The manager topologically orders plugins
  so every required plugin `init()`s before this one. A cycle fails closed.
- `entry`  — the linker symbol (a `const Plugin`) that `pm_register()` receives.

## Directory layout (target end-state)
```
plugins/
  plugin_manager.h / .cpp
  hello_world/  hello_world.cpp  plugin.json
  gfx/          gfx_core.cpp     gfx_glass.cpp   plugin.json
  font/         font_bitmap.cpp  font_vector.cpp font_cjk.cpp  plugin.json
  wm/           wm_core.cpp      wm_anim.cpp     plugin.json
  apps/         app_*.cpp        plugin.json
  input/        input_*.cpp      plugin.json
  theme/        theme_default.cpp plugin.json
```
