// =====================================================================
//  perm.cpp  -  Y/N permission prompt engine  (security doc v1.0, 3.3)
// =====================================================================

#include "perm.h"
#include "proc.h"
#include <stdint.h>

// ---- serial ----
static inline void outb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static void serial_puts(const char* s){ while(*s) outb(0x3F8,(uint8_t)*s++); }
static void serial_putdec(int v){
    char b[12]; int i = 0;
    if (v == 0) b[i++] = '0';
    else { int t = v; if (v < 0){ b[i++]='-'; t=-t; } while(t){ b[i++]=(char)('0'+t%10); t/=10; } }
    for (int j = i-1; j >= 0; j--) outb(0x3F8,(uint8_t)b[j]);
}

// ---- tiny string helpers ----
static int  pstreq(const char* a, const char* b){ while(*a && *a==*b){a++;b++;} return *a==*b; }
static void pstrcpy_n(char* d, const char* s, int cap){
    int i=0; while(s[i] && i<cap-1){ d[i]=s[i]; i++; } d[i]=0;
}
// Does `s` start with `pre`, ending on a path boundary ('/' or end)?
static int starts_with_dir(const char* s, const char* pre){
    int i = 0;
    while (pre[i]){ if (s[i] != pre[i]) return 0; i++; }
    return s[i] == 0 || s[i] == '/';
}
// Case-sensitive substring search (paths in this OS are lower-case).
static int contains(const char* hay, const char* needle){
    for (int i = 0; hay[i]; i++){
        int j = 0;
        while (needle[j] && hay[i+j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}
static const char* base_of(const char* p){
    const char* b = p;
    for (const char* q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

// ---------------------------------------------------------------------
//  Remembered decisions
// ---------------------------------------------------------------------
//  Keyed by (app name, resource class).  Name-keying is a placeholder:
//  a name is trivially forgeable by a dropped binary, so once 3.4 code
//  signing exists this must become the publisher identity.  Noted in the
//  header; not silently pretended to be stronger than it is.
// ---------------------------------------------------------------------
#define PERM_MAX_GRANTS 32
struct PermGrant {
    int  used;
    char app[64];
    int  cls;
    int  decision;
};
static PermGrant  g_grants[PERM_MAX_GRANTS];
static perm_ui_fn g_ui = 0;

void perm_init(perm_ui_fn ui){
    g_ui = ui;
    for (int i = 0; i < PERM_MAX_GRANTS; i++) g_grants[i].used = 0;
    serial_puts("[PERM] Y/N prompt engine armed (fail-safe: no answer = DENY)\n");
}

static int grant_lookup(const char* app, int cls, int* decision){
    for (int i = 0; i < PERM_MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].cls == cls && pstreq(g_grants[i].app, app)){
            *decision = g_grants[i].decision;
            return 1;
        }
    return 0;
}

static void grant_store(const char* app, int cls, int decision){
    for (int i = 0; i < PERM_MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].cls == cls && pstreq(g_grants[i].app, app)){
            g_grants[i].decision = decision;
            return;
        }
    for (int i = 0; i < PERM_MAX_GRANTS; i++)
        if (!g_grants[i].used){
            g_grants[i].used = 1;
            g_grants[i].cls = cls;
            g_grants[i].decision = decision;
            pstrcpy_n(g_grants[i].app, app, 64);
            return;
        }
    serial_puts("[PERM] grant table full - decision not remembered\n");
}

// ---------------------------------------------------------------------
//  Classification
// ---------------------------------------------------------------------
int perm_classify(const char* abspath, const char* root){
    (void)root;   // containment already decided by the VFS; we classify the target

    const char* leaf = base_of(abspath);

    // --- non-negotiable: credential material -------------------------
    // These are exactly the objects the 银狐 chain wants, and exactly the
    // ones a user cannot meaningfully consent to on a one-line prompt.
    if (pstreq(leaf, "passwd") || pstreq(leaf, "shadow") ||
        pstreq(leaf, "sam")    || pstreq(leaf, "credentials") ||
        contains(abspath, "/keys/") || starts_with_dir(abspath, "/system/security"))
        return PERM_CLS_CREDENTIAL;

    // --- non-negotiable: boot / autostart persistence ----------------
    if (starts_with_dir(abspath, "/system/startup") ||
        starts_with_dir(abspath, "/system/autorun") ||
        contains(abspath, "/run/") || pstreq(leaf, "autoexec"))
        return PERM_CLS_AUTORUN;

    // --- negotiable ---------------------------------------------------
    if (starts_with_dir(abspath, "/system"))                     return PERM_CLS_SYSTEM;
    if (starts_with_dir(abspath, "/home") ||
        starts_with_dir(abspath, "/docs") ||
        starts_with_dir(abspath, "/users"))                      return PERM_CLS_USERDATA;
    if (starts_with_dir(abspath, "/apps"))                       return PERM_CLS_OTHERAPP;
    return PERM_CLS_UNKNOWN;
}

const char* perm_class_name(int cls){
    switch (cls){
        case PERM_CLS_SANDBOX:    return "own sandbox";
        case PERM_CLS_USERDATA:   return "your documents";
        case PERM_CLS_OTHERAPP:   return "another app's private data";
        case PERM_CLS_SYSTEM:     return "system files";
        case PERM_CLS_CREDENTIAL: return "credential store";
        case PERM_CLS_AUTORUN:    return "startup persistence";
        default:                  return "unclassified location";
    }
}

int perm_class_risk(int cls, int write){
    int r;
    switch (cls){
        case PERM_CLS_SANDBOX:    r = PERM_RISK_LOW;      break;
        case PERM_CLS_USERDATA:   r = PERM_RISK_MEDIUM;   break;
        case PERM_CLS_OTHERAPP:   r = PERM_RISK_HIGH;     break;
        case PERM_CLS_SYSTEM:     r = PERM_RISK_HIGH;     break;
        case PERM_CLS_CREDENTIAL: r = PERM_RISK_CRITICAL; break;
        case PERM_CLS_AUTORUN:    r = PERM_RISK_CRITICAL; break;
        default:                  r = PERM_RISK_HIGH;     break;
    }
    if (write && r < PERM_RISK_CRITICAL) r++;   // writing is always worse than reading
    return r;
}

int perm_is_negotiable(int cls){
    return !(cls == PERM_CLS_CREDENTIAL || cls == PERM_CLS_AUTORUN);
}

// ---------------------------------------------------------------------
//  perm_request  -  the gate
// ---------------------------------------------------------------------
int perm_request(int cls, const char* resource, int write){
    const char* app = (g_current && g_current->name[0]) ? g_current->name : "(unknown)";
    uint32_t    pid = g_current ? g_current->pid : 0;
    uint32_t    uid = g_current ? g_current->uid : 0;
    int         risk = perm_class_risk(cls, write);

    // 1. Non-negotiable classes are refused without ever drawing a prompt.
    //    A dialog that never appears cannot be talked through.
    if (!perm_is_negotiable(cls)){
        serial_puts("[PERM] BLOCKED (non-negotiable) app="); serial_puts(app);
        serial_puts(" pid=");    serial_putdec((int)pid);
        serial_puts(" class=");  serial_puts(perm_class_name(cls));
        serial_puts(" path=");   serial_puts(resource);
        serial_puts(" -- no prompt is offered for this class\n");
        return PERM_DENY;
    }

    // 2. Previously remembered answer for this app + class.
    int cached = 0;
    if (grant_lookup(app, cls, &cached)){
        serial_puts("[PERM] CACHED ");
        serial_puts(cached == PERM_ALLOW ? "ALLOW" : "DENY");
        serial_puts(" app=");   serial_puts(app);
        serial_puts(" class="); serial_puts(perm_class_name(cls));
        serial_puts(" path=");  serial_puts(resource);
        serial_puts("\n");
        return cached;
    }

    // 3. Ask the human.
    if (!g_ui){
        serial_puts("[PERM] DENIED (no UI backend) path="); serial_puts(resource); serial_puts("\n");
        return PERM_DENY;
    }

    PermRequest req;
    req.app       = app;
    req.pid       = pid;
    req.uid       = uid;
    req.action    = write ? "WRITE" : "READ";
    req.resource  = resource;
    req.category  = perm_class_name(cls);
    req.risk      = risk;

    serial_puts("[PERM] PROMPT app=");  serial_puts(app);
    serial_puts(" pid=");               serial_putdec((int)pid);
    serial_puts(" action=");            serial_puts(req.action);
    serial_puts(" class=");             serial_puts(req.category);
    serial_puts(" risk=");              serial_putdec(risk);
    serial_puts(" path=");              serial_puts(resource);
    serial_puts("\n");

    int remember = 0;
    int answer   = g_ui(&req, &remember);

    // Fail-safe: anything that is not an explicit ALLOW is a DENY.
    if (answer != PERM_ALLOW) answer = PERM_DENY;

    if (remember) grant_store(app, cls, answer);

    serial_puts("[PERM] USER ");
    serial_puts(answer == PERM_ALLOW ? "ALLOWED" : "DENIED");
    serial_puts(remember ? " (remembered)" : " (this time only)");
    serial_puts(" app=");  serial_puts(app);
    serial_puts(" path="); serial_puts(resource);
    serial_puts("\n");
    return answer;
}

// ---------------------------------------------------------------------
void perm_dump(void){
    serial_puts("[PERM] remembered grants:\n");
    int n = 0;
    for (int i = 0; i < PERM_MAX_GRANTS; i++)
        if (g_grants[i].used){
            n++;
            serial_puts("       ");        serial_puts(g_grants[i].app);
            serial_puts(" -> ");           serial_puts(perm_class_name(g_grants[i].cls));
            serial_puts(" = ");            serial_puts(g_grants[i].decision == PERM_ALLOW ? "ALLOW" : "DENY");
            serial_puts("\n");
        }
    if (!n) serial_puts("       (none)\n");
}

void perm_reset(void){
    for (int i = 0; i < PERM_MAX_GRANTS; i++) g_grants[i].used = 0;
    serial_puts("[PERM] all remembered grants cleared\n");
}
