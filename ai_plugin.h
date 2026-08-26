// =====================================================================
//  ai_plugin.h  -  NexOS plugin catalogue / manager
// ---------------------------------------------------------------------
//  Implements the decoupled, on-demand plugin model described in the
//  NexOS plugin manifest: every feature is a named plugin with an id,
//  human name, dependency note, memory budget and a lifecycle state
//  (planned / basic / available).  The catalogue is the single source
//  of truth consumed by both the `plugin` shell command and the
//  Settings > Plugins UI.
//
//  The only "real" plugin wired to a baked-in engine is the AI inference
//  engine (nexos.ai.inference); the others are catalogued so the manager
//  can list / load / unload them as the OS grows.
// =====================================================================
#ifndef AI_PLUGIN_H
#define AI_PLUGIN_H

struct AiPlugin {
    const char* id;       // stable plugin id, e.g. "nexos.ai.inference"
    const char* name;     // display name
    const char* deps;     // short dependency note (human readable)
    int         mem_kb;   // resource requirement (KB)
    int         state;    // 0 planned, 1 basic/implemented, 2 available
    int         loaded;   // runtime loaded flag (0/1)
};

extern AiPlugin g_plugins[];
extern int      g_plugin_count;

// Seed the catalogue; sets each plugin's loaded flag from its state.
// Idempotent -- call once at boot.
void ai_plugin_init(void);

// Index of a plugin by id, or -1 if unknown.
int  ai_plugin_find(const char* id);

// Explicitly set the loaded flag; returns the new value or -1 if not found.
int  ai_plugin_set(const char* id, int loaded);

// Flip the loaded flag; returns the new value or -1 if not found.
int  ai_plugin_toggle(const char* id);

// Human-readable table for the terminal.  Returns bytes written.
int  ai_plugin_list(char* buf, int bufsize);

// Pipe-delimited "id|name|state|loaded" per line, for the file the GUI
// plugin manager reads and parses.  Returns bytes written.
int  ai_plugin_serialize(char* buf, int bufsize);

#endif
