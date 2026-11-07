#ifndef NEXOS_AI_TOOLUSE_H
#define NEXOS_AI_TOOLUSE_H
/* ai_tooluse.h -- let the in-kernel AI CALL TOOLS instead of only answering.
 *
 * NEW FEATURE.  Until now the AI produced text; the agent pipeline could run
 * shell:/plugin: directives that a human (or the planner) had to put in the
 * goal.  This module closes the loop: the model itself picks a tool, the
 * kernel validates and dispatches it through the EXISTING MCP bus, feeds the
 * observation back, and repeats until the model says it is done.
 *
 * WHY A LINE PROTOCOL AND NOT JSON TOOL-CALLS
 *   The local models in this OS range from a 0.5B transformer to remote APIs.
 *   A 0.5B model cannot reliably emit OpenAI-style JSON function calls, but it
 *   CAN emit one directive per line -- the same shape net_agent_plan_exec()
 *   already uses for shell:.  Keeping one protocol for both means one parser
 *   and one set of failure modes to reason about.
 *
 *    tool: <name> <json-args>
 *    done: <final answer>
 *
 * SECURITY
 *   * The model never names a target app: the whitelist table maps a tool to
 *     its (app_id, tool_name), so the model cannot reach an app the operator
 *     did not enable.
 *   * Dispatch goes through mcp_dispatch_request(), so the resident-set
 *     identity check still applies.  The source id used is the one passed in
 *     by the caller (normally the market/agent app), never the model's text.
 *   * Every step is appended to a trace the GUI can render, so an operator can
 *     see exactly what the AI did.
 *
 * 64-BIT ONLY: no new code is added to the 32-bit image (its spare space is
 * about 350 bytes).  Callers must be inside #if defined(__x86_64__).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AI_TOOLUSE_MAX_STEPS   6
#define AI_TOOLUSE_MAX_TOOLS  12
#define AI_TOOLUSE_TRACE_MAX  2048

/* One allowed tool.  `app_id` + `tool` are what gets passed to the MCP bus;
 * `name` is the short name the model writes.  Keeping them separate is what
 * stops a hostile model from addressing an app directly. */
typedef struct {
    const char* name;      /* as the model writes it, e.g. "fs_list"     */
    const char* app_id;    /* target app on the MCP bus, e.g. "FileMgr" */
    const char* tool;      /* tool name on that app                      */
    const char* help;      /* one line, shown to the model               */
} ai_tool_spec;

/* Completion backend.  Returns bytes written to out (>0) or <0.  The caller
 * supplies this so the module works with the local engine, the host gateway,
 * or a direct HTTPS call without knowing which. */
typedef int (*ai_complete_fn)(const char* prompt, char* out, int out_cap);

typedef struct {
    ai_complete_fn complete;
    const char*    caller_app_id;   /* MCP identity used for dispatch     */
    const ai_tool_spec* tools;      /* NULL => use ai_tooluse_default_tools */
    int            tool_count;
    int            max_steps;
    char           trace[AI_TOOLUSE_TRACE_MAX];
    int            trace_len;
} ai_tooluse_ctx;

/* Built-in whitelist (read-only tools only). */
/* LOCAL tools (not MCP apps).
 *
 * A tool spec whose app_id is EMPTY is a kernel-local function, not an MCP
 * dispatch.  cooperation.run is the first of these: a cooperation template is
 * interpreted inside the kernel, so routing it through the MCP bus would mean
 * inventing a fake app whose only job is to bounce the call back.  The module
 * that owns the local tool registers one dispatcher here. */
typedef const char* (*ai_local_tool_fn)(const char* tool, const char* json_args);
void ai_tooluse_set_local_dispatch(ai_local_tool_fn fn);

const ai_tool_spec* ai_tooluse_default_tools(int* count_out);

/* Run the loop.  Returns bytes written to `out` (>0), or <0 on error.
 * `out` always receives the best answer produced so far, even on failure,
 * because a partial answer beats an empty string for the user. */
int ai_tooluse_run(ai_tooluse_ctx* ctx, const char* goal, char* out, int out_cap);

/* Trace access for the GUI (same style as agent_trace_get). */
int ai_tooluse_trace(const ai_tooluse_ctx* ctx, char* buf, int cap);

/* Parse one model response into directives.  Exposed for unit testing.
 * Returns the number of `tool:` lines found; sets *done to 1 if a `done:`
 * line was seen and copies its text into done_out. */
int ai_tooluse_parse(const char* response, int len,
                     char names[][32], char args[][192], int max_calls,
                     char* done_out, int done_cap);

#ifdef __cplusplus
}
#endif
#endif
