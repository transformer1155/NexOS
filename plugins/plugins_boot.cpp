/* plugins_boot.cpp - registers every Phase-2 plugin module into the manager.
 * Called once from plugin_manager_boot(), after the smoke-test plugin.
 * The manager resolves the dependency graph (see each plugin's deps) and
 * loads them in order.  gui.cpp is untouched, so GUI behaviour is unchanged.
 */
#include "plugin_manager.h"
#include "linux_guest_backend.h"

extern const Plugin g_gfx_core, g_gfx_glass, g_font_bitmap, g_font_vector,
                   g_font_cjk, g_wm_core, g_wm_anim, g_input_keyboard,
                   g_input_mouse, g_app_control_panel, g_app_file_explorer,
                   g_app_task_manager, g_app_calculator, g_app_terminal,
                   g_app_browser, g_theme_default, g_src_view, g_linux_loader,
                   g_vnc_client, g_linux_manager;

/* ---- Demo auto-connect hook -------------------------------------------
 * The connect itself is issued from vnc_poll()'s demo block on the FIRST GUI
 * tick (see vnc_client.cpp): at plugins_boot time the NE2000 is not yet
 * initialized, and an SYN sent that early goes out with a half-initialized
 * NIC (MAC ff:ff:ff:ff:ff:ff) and gets RST by the SLIRP gateway. */
#ifdef NEXOS_VNC_DEMO
static void vnc_demo_autorun(void) {
    /* no-op: connection is deferred to the first vnc.poll() */
}
#endif

void plugins_boot(void) {
    const Plugin* all[] = {
        &g_src_view,
        &g_gfx_core, &g_gfx_glass, &g_font_bitmap, &g_font_vector, &g_font_cjk,
        &g_wm_core, &g_wm_anim, &g_input_keyboard, &g_input_mouse,
        &g_app_control_panel, &g_app_file_explorer, &g_app_task_manager,
        &g_app_calculator, &g_app_terminal, &g_app_browser, &g_theme_default,
        &g_linux_loader, &g_vnc_client, &g_linux_manager,
    };
    int n = sizeof(all) / sizeof(all[0]);
    for (int i = 0; i < n; i++) {
        if (pm_register(all[i]) != 0)
            pm_serial("[PM] plugins_boot: register failed: ");
        else
            pm_serial("[PM] registered ");
        pm_serial(all[i]->name);
        pm_serial("\n");
    }
#ifdef NEXOS_VNC_DEMO
    vnc_demo_autorun();
#endif
}
