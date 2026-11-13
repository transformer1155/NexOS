// =====================================================================
//  app_registry.h -- one table for every independently resident .mex app
// ---------------------------------------------------------------------
//  Before this header an application had several unrelated identities:
//    Kind.* in csharp/apps/Shell/Shell.cs, AppType in src/gui/gui_legacy.cpp,
//    a hard-coded "app == 7" in mforms.cpp, a telemetry id, and kName[] in
//    Desktop.cs.  app==7 is APP_MEM_OPTIMIZER while APP_NOTEPAD is 11, which
//    is why the standalone launch path never fired.
//
//  Add an application HERE (and give it a real .mex build rule) instead of
//  adding another hard-coded number in mforms.cpp.
// =====================================================================
#ifndef NEXOS_APP_REGISTRY_H
#define NEXOS_APP_REGISTRY_H

#include "clr.h"

// All capacity limits for the managed application layer live here.
// CLR_MAX_VMS (clr.h) is the slot pool size and includes slot 0 (the shell),
// so at most CLR_MAX_VMS-1 application images can be resident at once.
#define NEX_APP_MAX_SLOTS   CLR_MAX_VMS
// Keep in sync with MAX_WINDOWS in src/gui/gui_legacy.cpp.  The static window
// table lives in .bss, which linker.ld caps at 0x9C0000; 16 is the largest
// value that still links with the current globals.
#define NEX_APP_MAX_WINDOWS 16

// Standalone window handles live in their own numeric range so they can
// never collide with the shell-managed app ids (0..15) returned by
// mforms_open().  gui_legacy.cpp only passes managed_app around as an opaque
// id, never as an array index.
#define NEX_APP_HANDLE_BASE 64

// Native AppType values -- must match enum AppType in
// src/gui/gui_legacy.cpp (APP_WIN32 / APP_MANAGED are inserted at 9/10,
// which is what shifted Notepad from Kind 7 to AppType 11).
#define NEX_APP_NOTEPAD 11

// Managed Kind values -- must match Kind.* in csharp/apps/Shell/Shell.cs.
#define NEX_KIND_NOTEPAD 7

struct NexAppEntry {
    int         apptype;    // native AppType
    int         kind;       // managed Kind
    const char* mex;        // basename in sfs_files/
};

// The COMPLETE list of independently resident applications.  The kernel
// parses sfs_files/apps.man at boot and that manifest is the runtime
// source of truth; this table is the compile-time mirror used by the
// native launcher, so both must name the same .mex for the same AppType.
//
// Add an application HERE and in apps.man (and give it a .mex build rule).
static const NexAppEntry g_nex_apps[] = {
    {  1,  0, "ControlPanel.mex" },   // APP_CONTROL_PANEL
    {  2,  1, "FileExplorer.mex" },   // APP_FILE_EXPLORER
    {  3,  2, "TaskManager.mex" },    // APP_TASK_MANAGER
    {  4,  3, "Terminal.mex" },       // APP_TERMINAL
    {  5,  4, "Calculator.mex" },     // APP_CALCULATOR
    {  6,  5, "About.mex" },          // APP_ABOUT
    {  7,  6, "MemOptimizer.mex" },   // APP_MEM_OPTIMIZER
    { NEX_APP_NOTEPAD, NEX_KIND_NOTEPAD, "Notepad.mex" },
    {  8,  8, "Browser.mex" },        // APP_BROWSER
    { 12,  9, "AiSetup.mex" },        // APP_AISETUP
    { 13, 10, "AiAgent.mex" },        // APP_AIAGENT
    { 14, 11, "Demo.mex" },           // APP_DEMO
    { 15, 12, "NetTools.mex" },       // APP_NETTOOLS
    { 16, 13, "AiLauncher.mex" },     // APP_AILAUNCHER
    { 19, 14, "NetDefense.mex" },     // APP_NETDEFENSE
    { 20, 15, "WebView.mex" },        // APP_WEBVIEW
    { 21, 16, "VideoPlayer.mex" },    // APP_VIDEOPLAYER
    { 22, 17, "AudioPlayer.mex" },    // APP_AUDIOPLAYER
    { 23, 18, "AiChat.mex" },         // APP_AICHAT
    { 24, 19, "BugReport.mex" },      // APP_BUGREPORT
    { 25, 20, "AiWorkbench.mex" },    // APP_AIWORKBENCH
};

static inline int nex_app_count(void) {
    return (int)(sizeof(g_nex_apps) / sizeof(g_nex_apps[0]));
}

static inline const NexAppEntry* nex_app_find(int apptype) {
    for (int i = 0; i < nex_app_count(); i++)
        if (g_nex_apps[i].apptype == apptype) return &g_nex_apps[i];
    return 0;
}

// Index into the standalone handle table, or -1 when id is not a standalone
// handle.  Kept here so every mforms entry point uses the same conversion.
static inline int nex_handle_index(int id) {
    if (id >= NEX_APP_HANDLE_BASE &&
        id <  NEX_APP_HANDLE_BASE + NEX_APP_MAX_SLOTS)
        return id - NEX_APP_HANDLE_BASE;
    return -1;
}

#endif // NEXOS_APP_REGISTRY_H
