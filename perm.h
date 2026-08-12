#ifndef NexOS_PERM_H
#define NexOS_PERM_H

// =====================================================================
//  perm.h  -  Y/N permission prompt engine  (security doc v1.0, 3.3)
// ---------------------------------------------------------------------
//  The sandbox (3.2) answers "is this inside my own root?".  Everything
//  outside it used to be an unconditional DENY.  Real work needs to
//  cross that line sometimes -- an editor opening a document the user
//  picked, a tool reading a config.  3.3 turns that hard wall into an
//  explicit, attributable decision made by the HUMAN, not by the app.
//
//  Three properties this module is built around:
//
//   1. FAIL-SAFE.  Absence of an answer is a DENY.  No UI, no keyboard,
//      timeout, unknown class -> denied.  Never the other way round.
//
//   2. NOT EVERYTHING IS NEGOTIABLE.  A prompt is an attack surface:
//      the whole 银狐 playbook is convincing the user to click "yes".
//      Credential stores and autostart persistence are therefore
//      classified as non-negotiable and are refused WITHOUT asking.
//      You cannot socially-engineer a dialog that never appears.
//
//   3. ATTRIBUTABLE + CACHED.  A decision is recorded against the
//      requesting app and resource class, so the user is not trained to
//      spam "yes" by a prompt storm.  (Today the cache key is the app
//      name; once code signing (3.4) lands it should become the
//      publisher identity, which a dropped binary cannot forge.)
// =====================================================================

#include <stdint.h>

// ---- decisions ----
#define PERM_DENY        0
#define PERM_ALLOW       1
#define PERM_UI_NONE   (-1)      // returned by a UI backend that cannot ask

// ---- resource classes ----
#define PERM_CLS_SANDBOX     0   // inside the caller's own root: never prompts
#define PERM_CLS_USERDATA    1   // /home, /docs, /users        : ask (medium)
#define PERM_CLS_OTHERAPP    2   // another app's private dir   : ask (high)
#define PERM_CLS_SYSTEM      3   // /system/**                  : ask (high)
#define PERM_CLS_CREDENTIAL  4   // passwd / shadow / keys      : NEVER ask, deny
#define PERM_CLS_AUTORUN     5   // startup persistence         : NEVER ask, deny
#define PERM_CLS_UNKNOWN     6   // anything unrecognised       : ask (high)
#define PERM_CLS_COUNT       7

// ---- risk levels (drives how loud the prompt is) ----
#define PERM_RISK_LOW        0
#define PERM_RISK_MEDIUM     1
#define PERM_RISK_HIGH       2
#define PERM_RISK_CRITICAL   3

// What the UI backend is handed.  Deliberately a snapshot of facts the
// kernel knows for certain -- nothing here comes from the application.
struct PermRequest {
    const char* app;         // process name (kernel's record, not self-reported)
    uint32_t    pid;
    uint32_t    uid;
    const char* action;      // "READ" / "WRITE"
    const char* resource;    // fully resolved absolute path
    const char* category;    // human-readable class label
    int         risk;        // PERM_RISK_*
};

// A UI backend returns PERM_ALLOW / PERM_DENY, or PERM_UI_NONE if it is
// unable to ask (no keyboard, no display).  *remember is set to 1 when
// the user chose the "always" variant.
typedef int (*perm_ui_fn)(const PermRequest* req, int* remember);

void perm_init(perm_ui_fn ui);

// Classify a resolved absolute path relative to the caller's sandbox root.
int         perm_classify(const char* abspath, const char* root);
const char* perm_class_name(int cls);
int         perm_class_risk(int cls, int write);
int         perm_is_negotiable(int cls);

// The gate.  Consults the cache, otherwise raises the prompt.
// `write` is 1 for a write intent, 0 for read.
int  perm_request(int cls, const char* resource, int write);

void perm_dump(void);        // list remembered grants
void perm_reset(void);       // forget every remembered grant

#endif // NexOS_PERM_H
