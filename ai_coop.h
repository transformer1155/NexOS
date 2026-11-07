#ifndef NEXOS_AI_COOP_H
#define NEXOS_AI_COOP_H
/* ai_coop.h -- Cooperation Templates: data-driven team workflows.
 *
 * WHAT THIS IS
 *   A cooperation template is a DATA FILE that describes how a team of
 *   agents works together: which ROLES are needed, in which PHASES, and what
 *   each phase must hand over before the next may start.  The kernel reads
 *   and interprets it; adding a template is adding a file, not rebuilding.
 *
 * WHY ROLES AND NOT NAMES
 *   Writing teammate names into a template binds it to one staffing snapshot.
 *   A template that names someone breaks the moment that person is busy,
 *   renamed, or gone.  Capabilities survive all three -- and "hot pluggable"
 *   ultimately means "works with whoever is actually available".
 *
 * STAGE 1 (this code): READ-ONLY INTERPRETER
 *   Parse + validate + produce a plan (suggested allocation, per-phase handoff
 *   checklist, missing-artifact warnings).  It does NOT create tasks, send
 *   messages, or wake anyone.  Those are stage 2, and only after the plan has
 *   proven useful in practice.
 *
 * 64-BIT ONLY: adds nothing to the 32-bit image (420 B spare there).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sizes are deliberately tight: every one of these multiplies into the
 * 64-bit kernel's .bss, and an over-large .bss is what pushed past HEAP_START
 * (heap then overlapped kernel data and the boot died silently at the first
 * big allocation).  The first template uses 5 phases / 5 roles / max 3 context
 * refs, so these caps keep 60%+ headroom while costing a fraction of the old
 * footprint.  Raising a cap here is a memory decision, not just a config. */
/* Buffer sizes are from MEASUREMENT, not guesswork.  The first trim set
 * tpl_text=2560 while the shipped template is 3,085 B on disk: every call would
 * have parsed a truncated file and failed in a way that looks like broken code.
 * If a template outgrows COOP_TPL_CAP, coop_run_local must report the overflow
 * explicitly rather than parse half a file. */
#define COOP_TPL_CAP       4096
#define COOP_PLAN_CAP      2048
#define COOP_OUT_CAP       2688

#define COOP_MAX_PHASES    8
#define COOP_MAX_ROLES     6
#define COOP_MAX_NAMES     16
#define COOP_MAX_REFS      4
#define COOP_STR           48
#define COOP_NAME          32
#define COOP_GOAL          96

typedef struct {
    char role[COOP_NAME];
    char capability[COOP_MAX_REFS][COOP_NAME];
    int  n_caps;
    int  count;
    char fallback[COOP_NAME];   /* "" = none (fail closed) */
} coop_role;

typedef struct {
    char phase[COOP_NAME];
    char role[COOP_NAME];
    char goal[COOP_GOAL];
    char action_kind[16];       /* task | report | template */
    char action_subject[COOP_GOAL];
    char action_template[COOP_NAME];
    char context_in[COOP_MAX_REFS][COOP_NAME];
    int  n_in;
    char context_out[COOP_MAX_REFS][COOP_NAME];
    int  n_out;
    /* Named `need`, not `requires`: `requires` is a C++20 keyword, so the old
       name would break the build the moment anyone adds -std=gnu++20. */
    char need[COOP_MAX_REFS][COOP_NAME];
    int  n_requires;
    char handoff_to[COOP_NAME];
} coop_phase;

typedef struct {
    int  schema;
    char id[COOP_NAME];
    char title[COOP_GOAL];
    char version[16];
    char summary[COOP_GOAL];
    int  skill_exposed;
    int  max_phases;
    int  require_handoff_artifacts;
    int  fail_closed;
    coop_role  roles[COOP_MAX_ROLES];
    int        n_roles;
    coop_phase phases[COOP_MAX_PHASES];
    int        n_phases;
    /* inputs: name -> required flag */
    char inputs[COOP_MAX_NAMES][COOP_NAME];
    int  input_required[COOP_MAX_NAMES];
    int  n_inputs;
} coop_template;

/* Parse `text` (one template JSON document).  Returns 0 on success, negative
 * on error, and writes a static reason into *reason.  A template that fails
 * to parse is REJECTED, never partially applied (fail closed). */
int coop_parse(coop_template* out, const char* text, const char** reason);

/* Validate references and guards.  Separate from parse so a caller can show
 * what is wrong with an already-parsed template.  Returns 0 if usable. */
int coop_validate(const coop_template* t, const char** reason);

/* Produce the read-only plan for `inputs` (name/value pairs, n may be 0).
 * This is the stage-1 deliverable: what WOULD happen, and what each phase
 * must hand over.  Returns bytes written into `out`. */
int coop_plan(const coop_template* t, const char* symptom_hint,
              char* out, int cap);

/* Capability matching: which of `agent_names` satisfies `role`?  Read-only,
 * returns the chosen index or -1 (no match).  `agent_caps` is a parallel
 * array of comma-separated capability strings. */
/* ---- integration hooks (NO guessed kernel symbols) ------------------
 *
 * Reading a template from SFS needs a filesystem entry point.  Rather than
 * declaring whatever name we *think* the VFS exports (the mistake that cost
 * us the whole proc_isolation module), the integrator wires the real reader
 * in here.  Same pattern, same reason. */
typedef int (*coop_read_fn)(const char* name, char* buf, int cap);
void coop_set_reader(coop_read_fn fn);

#ifndef COOP_HOST_TEST
/* Installs the real SFS reader.  Symbols confirmed with the kernel owner:
 *   kern_fs_ready(void), kern_fs_read(name,buf,cap)  -- .attic64/kernel64.cpp
 * kern_fs_read takes a FLAT name and handles the leading slash itself, so we
 * try the bare name and then the slash-prefixed one (the same pattern
 * nexos_ai.cpp uses for apps.json). */
void coop_install_kernel_reader(void);
#endif

/* The local tool behind `cooperation.run` in the ai_tooluse table.
 * args: JSON with { "template": "<id>", "symptom": "..." } (symptom optional).
 * Returns a static JSON string; never NULL.  Stage 1 = plan only. */
const char* coop_run_local(const char* tool, const char* json_args);

/* One-call installation of everything ai_coop needs:
 *   * the SFS reader (coop_install_kernel_reader)
 *   * the local tool dispatcher (ai_tooluse_set_local_dispatch)
 * Called from ai_plugin_init(), which the 64-bit kernel already invokes exactly
 * once at boot -- so integrating this module touches NO kernel64.cpp. */
void ai_coop_install(void);

int coop_match_role(const coop_template* t, const coop_role* r,
                    const char* const* agent_names,
                    const char* const* agent_caps, int n_agents);

#ifdef __cplusplus
}
#endif
#endif
