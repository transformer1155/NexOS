/* app_calculator plugin - Phase 2 facade + Phase 5 reloadable compute.
 * Exposes "app.calculator" service and pm_call("app_calculator","calc",a,b,op).
 * By default it computes a + b (op 0), a - b (op 1), a * b (op 2), a / b (op 3).
 * After `plugin pm reload app_calculator`, the behaviour is taken from the
 * loaded .bc's func 0 (which receives a,b,op on the stack and returns the
 * result), so editing /src/app_calculator.skill changes the calculator live.
 */
#include "plugin_manager.h"
#include "nexos_api.h"
#include "skill_vm.h"

static int calc_default(int a, int b, int op) {
    switch (op) {
        case 1: return a - b;
        case 2: return a * b;
        case 3: return b ? a / b : 0;
        default: return a + b;
    }
}

/* Reloaded VM state (set by theme_reload-style hook).  -1 func id == not loaded. */
static SkillVM   g_calc_vm;
static int       g_calc_fid = -1;

static int calc_reload(const uint8_t* bc, int len) {
    if (skill_load(&g_calc_vm, bc, len) != 0) { pm_serial("[CALC] reload: bad .bc\n"); return -1; }
    /* look the entry point up by name rather than assuming func 0 */
    g_calc_fid = skill_find(&g_calc_vm, "calc");
    if (g_calc_fid < 0) g_calc_fid = 0;
    pm_serial("[CALC] reload: bytecode loaded\n");
    return 0;
}

static int app_calc_svc(void* args, void* out, int outcap) {
    (void)outcap;
    int* a = (int*)args;
    int x = a ? a[0] : 0, y = a ? a[1] : 0, op = a ? a[2] : 0;
    int r = calc_default(x, y, op);
    if (out && outcap >= (int)sizeof(int)) *(int*)out = r;
    return r;
}

static int app_calculator_init(Plugin* self) {
    (void)self;
    svc_register("app.calculator", app_calc_svc);
    pm_set_reload("app_calculator", calc_reload);
    return 0;
}
static void app_calculator_exit(Plugin* self) {
    (void)self;
    svc_unregister("app.calculator");
}
static int app_calculator_call(Plugin* self, const char* method, void* args, void* out, int outcap) {
    (void)self;
    if (pm_strcmp(method, "calc") != 0) return -1;
    int* a = (int*)args;
    int x = a ? a[0] : 0, y = a ? a[1] : 0, op = a ? a[2] : 0;
    int r;
    if (g_calc_fid >= 0) {
        /* push a,b,op then run func 0 */
        g_calc_vm.stk[0] = x; g_calc_vm.stk[1] = y; g_calc_vm.stk[2] = op;
        g_calc_vm.sp = 3;
        int halt = 0;
        if (skill_run(&g_calc_vm, g_calc_fid, &halt) == 0) r = halt;
        else r = calc_default(x, y, op);
    } else {
        r = calc_default(x, y, op);
    }
    if (out && outcap >= (int)sizeof(int)) *(int*)out = r;
    pm_serial("[CALC] calc("); 
    { char nb[12]; int ni=0,t=x; if(t<0){nb[ni++]='-';t=-t;} if(t==0)nb[ni++]='0'; else{char tmp[10];int tn=0;while(t){tmp[tn++]=('0'+(t%10));t/=10;}while(tn)nb[ni++]=tmp[--tn];} nb[ni++]=0; pm_serial(nb); }
    pm_serial(",");
    { char nb[12]; int ni=0,t=y; if(t<0){nb[ni++]='-';t=-t;} if(t==0)nb[ni++]='0'; else{char tmp[10];int tn=0;while(t){tmp[tn++]=('0'+(t%10));t/=10;}while(tn)nb[ni++]=tmp[--tn];} nb[ni++]=0; pm_serial(nb); }
    pm_serial(") = ");
    { char nb[12]; int ni=0,t=r; if(t<0){nb[ni++]='-';t=-t;} if(t==0)nb[ni++]='0'; else{char tmp[10];int tn=0;while(t){tmp[tn++]=('0'+(t%10));t/=10;}while(tn)nb[ni++]=tmp[--tn];} nb[ni++]=0; pm_serial(nb); }
    pm_serial("\n");
    return r;
}
extern const Plugin g_app_calculator = {
    "app_calculator", 0x0100,
    "app.calculator",
    "wm_core",
    app_calculator_init, app_calculator_exit, app_calculator_call
};
