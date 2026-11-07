/* ai_tooluse.cpp -- see ai_tooluse.h.  Freestanding, no libc. */

#include "ai_tooluse.h"

extern "C" const char* mcp_dispatch_request(const char* source_app_id,
                                             const char* target_app_id,
                                             const char* tool_name,
                                             const char* json_args);

static int s_len(const char* s){ int n = 0; if (s) while (s[n]) n++; return n; }
static void s_cpy(char* d, const char* s, int cap){
    int i = 0; if (cap <= 0) return;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int s_cmp(const char* a, const char* b){
    while (*a && *a == *b){ a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int starts_with(const char* s, const char* p){
    while (*p){ if (*s != *p) return 0; s++; p++; }
    return 1;
}

/* Read-only tools only.  Anything that mutates state is deliberately absent:
 * an AI that can delete files unattended is not a feature, it is an incident.
 * Write tools belong behind an explicit operator confirmation, not here. */
static const ai_tool_spec g_tools[] = {
    { "fs_list",   "FileMgr",  "fs_list",   "list files: {\"fs\":0}" },
    { "fs_read",   "FileMgr",  "fs_read",   "read a file: {\"fs\":0,\"name\":\"x\"}" },
    { "browser_get_text", "Browser", "browser_get_text", "current page text: {}" },
    { "terminal_exec", "Terminal", "terminal_exec", "run a command: {\"cmd\":\"ls\"}" },
    /* LOCAL tool: empty app_id means "kernel function", not an MCP app. */
    { "cooperation.run", "", "cooperation.run",
      "run a cooperation template: {\"template\":\"bugfix-end-to-end\",\"symptom\":\"...\"}" },
};

static ai_local_tool_fn g_local_fn = 0;

void ai_tooluse_set_local_dispatch(ai_local_tool_fn fn){ g_local_fn = fn; }

const ai_tool_spec* ai_tooluse_default_tools(int* count_out){
    if (count_out) *count_out = (int)(sizeof(g_tools) / sizeof(g_tools[0]));
    return g_tools;
}

static void trace_add(ai_tooluse_ctx* c, const char* tag, const char* msg){
    if (!c) return;
    const char* parts[3]; parts[0] = tag; parts[1] = ": "; parts[2] = msg;
    for (int k = 0; k < 3; k++){
        const char* s = parts[k];
        while (s && *s && c->trace_len < AI_TOOLUSE_TRACE_MAX - 2){
            c->trace[c->trace_len++] = *s++;
        }
    }
    if (c->trace_len < AI_TOOLUSE_TRACE_MAX - 1) c->trace[c->trace_len++] = '\n';
    c->trace[c->trace_len] = 0;
}

int ai_tooluse_trace(const ai_tooluse_ctx* c, char* buf, int cap){
    if (!c || !buf || cap <= 0) return 0;
    s_cpy(buf, c->trace, cap);
    return s_len(buf);
}

/* Parse the line protocol.  Tolerant by design: unknown lines are ignored,
 * because a small model WILL emit prose around directives. */
int ai_tooluse_parse(const char* response, int len,
                     char names[][32], char args[][192], int max_calls,
                     char* done_out, int done_cap){
    int calls = 0;
    if (done_out && done_cap > 0) done_out[0] = 0;
    if (!response || len <= 0) return 0;

    int i = 0;
    while (i < len){
        int line_start = i;
        while (i < len && response[i] != '\n') i++;
        int line_len = i - line_start;
        i++;
        const char* line = response + line_start;

        /* skip leading spaces */
        int off = 0;
        while (off < line_len && (line[off] == ' ' || line[off] == '\t')) off++;
        if (off >= line_len) continue;

        if (starts_with(line + off, "tool:")){
            int p = off + 5;
            while (p < line_len && (line[p] == ' ' || line[p] == '\t')) p++;
            int ns = p;
            while (p < line_len && line[p] != ' ' && line[p] != '\t') p++;
            int nlen = p - ns;
            if (nlen <= 0 || nlen >= 32) continue;
            if (calls >= max_calls) break;
            int k = 0;
            for (; k < nlen; k++) names[calls][k] = line[ns + k];
            names[calls][k] = 0;
            while (p < line_len && (line[p] == ' ' || line[p] == '\t')) p++;
            int a = 0;
            for (; p < line_len && a < 191; p++) args[calls][a++] = line[p];
            args[calls][a] = 0;
            if (a == 0){ args[calls][0] = '{'; args[calls][1] = '}'; args[calls][2] = 0; }
            calls++;
        } else if (starts_with(line + off, "done:")){
            int p = off + 5;
            while (p < line_len && (line[p] == ' ' || line[p] == '\t')) p++;
            if (done_out && done_cap > 0){
                int a = 0;
                for (; p < line_len && a < done_cap - 1; p++) done_out[a++] = line[p];
                done_out[a] = 0;
            }
        }
    }
    return calls;
}

static const ai_tool_spec* find_tool(const ai_tooluse_ctx* c, const char* name){
    if (c->tools){
        for (int i = 0; i < c->tool_count; i++)
            if (s_cmp(c->tools[i].name, name) == 0) return &c->tools[i];
        return 0;
    }
    int n = 0;
    const ai_tool_spec* t = ai_tooluse_default_tools(&n);
    for (int i = 0; i < n; i++)
        if (s_cmp(t[i].name, name) == 0) return &t[i];
    return 0;
}

static int build_prompt(const ai_tooluse_ctx* c, const char* goal,
                        const char* history, char* buf, int cap){
    int at = 0;
    const char* head =
        "You are the NexOS agent. You may call tools, one per line:\n"
        "  tool: <name> <json-args>\n"
        "When finished, reply with exactly one line:\n"
        "  done: <answer>\n"
        "Do not invent tool names. Do not call a tool twice with the same args.\n"
        "Available tools:\n";
    for (const char* p = head; *p && at < cap - 2; p++) buf[at++] = *p;

    int n = 0;
    const ai_tool_spec* t = c->tools ? c->tools : ai_tooluse_default_tools(&n);
    int tc = c->tools ? c->tool_count : n;
    for (int i = 0; i < tc && at < cap - 2; i++){
        const char* parts[5];
        parts[0] = "  - "; parts[1] = t[i].name; parts[2] = "  ";
        parts[3] = t[i].help; parts[4] = "\n";
        for (int k = 0; k < 5; k++)
            for (const char* p = parts[k]; *p && at < cap - 2; p++) buf[at++] = *p;
    }
    const char* mid = "Goal: ";
    for (const char* p = mid; *p && at < cap - 2; p++) buf[at++] = *p;
    for (const char* p = goal; p && *p && at < cap - 2; p++) buf[at++] = *p;
    if (history && history[0]){
        const char* hh = "\nObservations so far:\n";
        for (const char* p = hh; *p && at < cap - 2; p++) buf[at++] = *p;
        for (const char* p = history; *p && at < cap - 2; p++) buf[at++] = *p;
    }
    const char* tail = "\nYour reply:\n";
    for (const char* p = tail; *p && at < cap - 2; p++) buf[at++] = *p;
    buf[at] = 0;
    return at;
}

int ai_tooluse_run(ai_tooluse_ctx* ctx, const char* goal, char* out, int out_cap){
    if (!ctx || !ctx->complete || !goal || !out || out_cap <= 0) return -1;
    out[0] = 0;
    ctx->trace[0] = 0;
    ctx->trace_len = 0;
    int steps = ctx->max_steps > 0 ? ctx->max_steps : AI_TOOLUSE_MAX_STEPS;

    static char prompt[1024];
    static char reply[1024];
    static char history[1024];
    history[0] = 0;
    int hist_len = 0;
    char best[256]; best[0] = 0;

    for (int step = 0; step < steps; step++){
        build_prompt(ctx, goal, history, prompt, (int)sizeof(prompt));
        int n = ctx->complete(prompt, reply, (int)sizeof(reply) - 1);
        if (n <= 0){
            trace_add(ctx, "error", "model call failed");
            if (best[0]) s_cpy(out, best, out_cap);
            return best[0] ? s_len(best) : -1;
        }
        reply[n] = 0;

        char names[4][32];
        char args[4][192];
        char done[256];
        int calls = ai_tooluse_parse(reply, n, names, args, 4, done, (int)sizeof(done));

        /* A `done:` line is authoritative for this step, but we keep looking
           for calls in earlier steps: a model often says done too early. */
        if (done[0]) s_cpy(best, done, (int)sizeof(best));

        if (calls == 0){
            if (done[0]){
                trace_add(ctx, "done", done);
                s_cpy(out, done, out_cap);
                return s_len(out);
            }
            /* No tool call and no done: treat the reply as the answer. */
            trace_add(ctx, "answer", reply);
            s_cpy(out, reply, out_cap);
            return s_len(out);
        }

        for (int i = 0; i < calls; i++){
            const ai_tool_spec* spec = find_tool(ctx, names[i]);
            if (!spec){
                trace_add(ctx, "reject", names[i]);
                const char* m = "(tool not allowed)";
                for (const char* p = m; *p && hist_len < (int)sizeof(history) - 2; p++)
                    history[hist_len++] = *p;
                if (hist_len < (int)sizeof(history) - 1) history[hist_len++] = '\n';
                history[hist_len] = 0;
                continue;
            }
            trace_add(ctx, "tool", names[i]);
            const char* res;
            if (!spec->app_id || !spec->app_id[0]){
                /* Local kernel tool: no MCP identity, and no MCP ACL -- the
                   registered local dispatcher is the trust boundary. */
                res = g_local_fn ? g_local_fn(spec->tool, args[i])
                                 : "{\"code\":5,\"error\":\"local dispatch not registered\"}";
            } else {
                res = ctx->caller_app_id
                    ? mcp_dispatch_request(ctx->caller_app_id, spec->app_id,
                                           spec->tool, args[i])
                    : "{\"code\":3,\"error\":\"no caller identity\"}";
            }
            if (!res) res = "(null)";

            /* Append the observation, capped so history cannot grow forever. */
            const char* labs[3]; labs[0] = names[i]; labs[1] = " => "; labs[2] = res;
            for (int k = 0; k < 3; k++)
                for (const char* p = labs[k]; *p && hist_len < (int)sizeof(history) - 2; p++)
                    history[hist_len++] = *p;
            if (hist_len < (int)sizeof(history) - 1) history[hist_len++] = '\n';
            history[hist_len] = 0;
        }

        /* If the model said done and we executed its calls, finish. */
        if (done[0]){
            trace_add(ctx, "done", done);
            s_cpy(out, done, out_cap);
            return s_len(out);
        }
    }

    trace_add(ctx, "limit", "max steps reached");
    if (best[0]){ s_cpy(out, best, out_cap); return s_len(out); }
    const char* stop = "(tool-use stopped: step limit reached)";
    s_cpy(out, stop, out_cap);
    return s_len(out);
}
