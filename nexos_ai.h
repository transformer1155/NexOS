#pragma once
/* nexos_ai.h -- AI-Native gateway + MCP bus kernel side
 *
 * Three things live here:
 *   1. AIMode / McpResult enums shared with NexOS.AI / NexOS.MCP C#.
 *   2. The AppRegistry: every app's declared AI quota tier and the MCP
 *      tools it SERVES (MCP_Server) plus the apps it is ALLOWED to CALL
 *      (MCP_Client_Access).  The registry is TABLE-DRIVEN: it is built
 *      entirely from /apps.json at boot -- there is no compiled-in seeding.
 *   3. The AIGateway: one host-side aggregator on 127.0.0.1:19387 that
 *      pools quota across every app (Fast=local, Balanced/Heavy=host).
 *
 * TWO-LAYER DESIGN
 *   /apps.json  declares the topology (which app, which tier, which tools,
 *               who may call whom).  Editing it changes behaviour with no
 *               kernel rebuild.
 *   dispatch table in nexos_ai.cpp maps a tool NAME to a C implementation.
 *               A tool declared in the manifest but absent from the table
 *               still registers and runs (generic echo), so a new app is a
 *               pure-data addition.  Only a tool that needs to *do* something
 *               native requires a table entry.
 *
 * SECURITY (anti-forgery)
 *   The ACL principal is the basename of the .mex the MiniCLR actually
 *   loaded, recorded into g_current_app_id by clr.cpp.  The CLR endorses it
 *   via app_registry_note_resident(); mcp_dispatch_request() rejects any
 *   caller outside that trusted resident set.  Managed code never supplies
 *   the caller id it is checked against, and a manifest entry can only
 *   describe policy -- it can never manufacture an identity.
 *
 * All structs are plain-old-data so the freestanding kernel can compile
 * them without the C++ runtime.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* ---- AI tiers (must match NexOS.AI.AIMode) ---- */
enum AIMode {
    AI_FAST     = 0,   /* local inference, no host round-trip */
    AI_BALANCED = 1,   /* host gateway, default quota tier   */
    AI_HEAVY    = 2    /* host gateway, premium model tier    */
};

/* ---- MCP dispatch result codes (must match NexOS.MCP.McpResult.code) ---- */
enum McpResult {
    MCP_OK         = 0,
    MCP_ERR_NO_SRC = 1,  /* caller app unknown                */
    MCP_ERR_NO_TGT = 2,  /* target app or tool not registered */
    MCP_ERR_FORBID= 3,   /* caller not in target's access list*/
    MCP_ERR_ARGS   = 4,  /* malformed arguments               */
    MCP_ERR_DISPATCH = 5 /* tool callback failed              */
};

struct McpTool;

/* A tool an app exposes on the MCP bus. */
struct McpTool {
    const char* tool_name;
    const char* description;
    const char* parameters_schema;   /* JSON Schema string */
    /* Returns a JSON result string (stored in a static buffer owned by the
       tool; the caller must copy it before the next call).  Must NOT be freed. */
    const char* (*dispatch)(const char* source_app_id,
                            const char* target_app_id,
                            const char* tool_name,
                            const char* json_args);
};

/* One registered application.  Populated from /apps.json; see the manifest
 * schema documented in nexos_ai.cpp's parse_apps_json(). */
struct AppRegistry {
    const char* app_id;                 /* e.g. "Browser" (matches .mex basename) */
    const char* name;
    const char* version;
    const char* author;
    int         ai_mode_required;       /* AIMode the app is entitled to          */
    const struct McpTool** mcp_server;  /* tools this app SERVES (MCP_Server)     */
    int         mcp_server_count;
    const char** mcp_client_access;     /* app_ids this app MAY CALL              */
    int         mcp_client_access_count;
    int         loaded;                 /* 1 once the CLR has endorsed its .mex   */
};

/* Host-side AI quota aggregator. */
struct AIGateway {
    int         system_mode;            /* current AIMode                         */
    const char* gateway_host;           /* host gateway address                  */
    int         gateway_port;           /* host gateway port (19387)             */
    int         quota_remaining;        /* pooled quota left for this boot       */
    int         initialized;
};

#define NEXOS_AI_MAX_APPS   32
#define NEXOS_AI_MAX_TOOLS  16
#define NEXOS_AI_MAX_ACCESS 16
#define NEXOS_AI_GATEWAY_PORT 19387

extern struct AppRegistry g_app_registry[NEXOS_AI_MAX_APPS];
extern int g_app_registry_count;
extern struct AIGateway g_ai_gateway;

/* ---- API ---- */
int  ai_gateway_init(void);                       /* parse /apps.json, build registry */
int  get_mode(void);                              /* current AIMode                  */
int  set_mode(int m);                             /* set current AIMode (+ai_engine)*/
int  quota_remaining(void);                       /* pooled quota left              */
int  ai_request(const char* prompt, int mode,
               char* out, int outsize);           /* returns JSON result string     */
/* Validate MCP_Client_Access, then dispatch.  Returns a JSON string
   {"code":N,"json":"<tool result>"} held in a static buffer. */
const char* mcp_dispatch_request(const char* source_app_id,
                                 const char* target_app_id,
                                 const char* tool_name,
                                 const char* json_args);
struct AppRegistry* app_registry_find(const char* app_id);

/* Called ONLY by the MiniCLR loader with the basename of a .mex it has just
   loaded.  This is what makes an app's identity trustworthy: mcp_dispatch_request
   accepts a caller only if it appears in this endorsed set, so managed code
   cannot forge its own app_id.  Also marks the registry entry loaded=1.
   Safe to call with NULL or "" (no-op). */
void app_registry_note_resident(const char* app_id);

#ifdef __cplusplus
}
#endif
