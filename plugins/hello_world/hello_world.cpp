/* hello_world plugin - Phase 1 smoke test.
 * init() prints "hello from plugin"; exit() is a no-op; call() returns
 * -1 ("unknown method") for everything. Requires nothing.
 */
#include "../plugin_manager.h"

static int hw_init(Plugin* self) {
    (void)self;
    pm_serial("hello from plugin\n");
    return 0;
}
static void hw_exit(Plugin* self) { (void)self; }
static int hw_call(Plugin* self, const char* method, void* args, void* out, int outcap) {
    (void)self; (void)method; (void)args; (void)out; (void)outcap;
    return -1; /* unknown method */
}

extern const Plugin g_helloworld = {
    "hello_world",            /* name    */
    0x0100,                   /* version */
    "hello_world.greet",      /* provides */
    "",                       /* deps */
    hw_init,
    hw_exit,
    hw_call,
};
