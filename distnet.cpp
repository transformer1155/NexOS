// =====================================================================
//  distnet.cpp  -  Minimal distributed compute network (NexOS/MiniOS)
// ---------------------------------------------------------------------
//  See distnet.h for the protocol.  Freestanding C++: no STL, no new,
//  no exceptions.  All state is in static buffers.
// =====================================================================

#include <stddef.h>   // NULL
#include "distnet.h"

// ---- node discovery state (declared early: used by distnet_scheduler_ask
//      below, which is defined before the NODES section at the bottom) ----
#define DN_MAX_NODES 8
static uint32_t g_nodes[DN_MAX_NODES];
static int      g_node_count = 0;
static void nodes_on_beacon(uint32_t src_ip, uint16_t src_port,
                            const uint8_t* data, int len);

// ---- AI bridge (implemented in kernel.cpp / .attic64/kernel64.cpp, C linkage) ----
// kern_ai_ask boots the engine if needed, runs the native GGUF/Qwen forward
// pass, and copies the answer into `out` (NUL-terminated).  Returns:
//   -1 bad args   -2 no model (ai_init failed)   -3 generate failed
//   >=0 length of the copied answer.
// Using this (instead of ai_generate directly) avoids the static-buffer
// overwrite hazard: ai_generate returns a buffer that is recycled on the
// next call, so callers must copy immediately.
extern "C" int kern_ai_ask(const char* prompt, char* out, int outsz);

// ---- tiny string helpers (no libc) ----
static int  my_strlen(const char* s){ int n=0; while(s[n]) n++; return n; }
static void my_strcpy(char* d, const char* s){ while(*s) *d++=*s++; *d=0; }
static void my_strcat(char* d, const char* s){ while(*d) d++; while(*s) *d++=*s++; *d=0; }

// Append NUL-terminated s into out[] at *rp, advance *rp past the terminator.
// Index-based (no my_strlen in the hot path) -- robust under -O2 freestanding.
static void rcat(char* out, int* rp, const char* s){
    while (*s) out[(*rp)++] = *s++;
    out[*rp] = 0;
}

// Write an unsigned int at buf, return chars written (no terminator).
static int my_utoa(unsigned v, char* b){
    if (v == 0){ b[0]='0'; return 1; }
    char t[12]; int n=0;
    while(v){ t[n++] = (char)('0' + (v % 10)); v /= 10; }
    int p=0; while(n) b[p++] = t[--n];
    return p;
}

// Append the decimal form of v onto out[] at *rp (index-based, like rcat).
static void ucat(char* out, int* rp, unsigned v){
    char b[12]; int n = my_utoa(v, b);
    for (int i=0;i<n;i++) out[(*rp)++] = b[i];
    out[*rp] = 0;
}

// Parse a decimal token (len bytes, not NUL-terminated necessarily) to unsigned.
static unsigned parse_uint(const char* tok, int len){
    unsigned v=0;
    for (int j=0; j<len; j++) v = v*10 + (unsigned)(tok[j]-'0');
    return v;
}

// "a.b.c.d" -> host-order uint32_t matching net.cpp IPV4() storage.
static uint32_t parse_ip(const char* s){
    int parts[4]; int n=0; int cur=0;
    for (int i=0; s[i]; i++){
        if (s[i]=='.'){ if(n<4) parts[n++]=cur; cur=0; }
        else if (s[i]>='0' && s[i]<='9') cur = cur*10 + (s[i]-'0');
    }
    if (n<3) return 0;
    parts[n++]=cur;
    if (n!=4) return 0;
    return ((uint32_t)parts[0]<<24)|((uint32_t)parts[1]<<16)|
           ((uint32_t)parts[2]<<8) |(uint32_t)parts[3];
}

// Log an IPv4 (host order) as dotted decimal.
static void log_ip(uint32_t ip){
    char s[16]; int p=0;
    p += my_utoa((ip>>24)&0xFF, s+p); s[p++]='.';
    p += my_utoa((ip>>16)&0xFF, s+p); s[p++]='.';
    p += my_utoa((ip>>8)&0xFF,  s+p); s[p++]='.';
    p += my_utoa(ip&0xFF,        s+p); s[p]=0;
    net_log(s);
}

// Log an unsigned integer as decimal.
static void log_uint(unsigned v){
    char s[12]; int n = my_utoa(v, s);
    s[n] = 0;
    net_log(s);
}

// Split a buffer into at most max_tok tokens on spaces.  Returns token count.
// Each token is NUL-terminated in-place; out[] holds pointers + lengths.
static int tokenize(char* buf, char* out[], int* outlen, int max_tok){
    int n=0; char* p=buf;
    while (*p && n < max_tok){
        while (*p==' '||*p=='\t'||*p=='\n'||*p=='\r') *p++=0;
        if (!*p) break;
        out[n]=p; outlen[n]=0;
        while (*p && *p!=' ' && *p!='\t' && *p!='\n' && *p!='\r'){ outlen[n]++; p++; }
        n++;
    }
    return n;
}

// =====================================================================
//  COMPUTE role
// =====================================================================
static void compute_on_beacon(uint32_t src_ip, uint16_t src_port,
                              const uint8_t* data, int len){
    (void)data; (void)len; (void)src_port;
    char buf[32];
    my_strcpy(buf, "BEACON node0");
    net_udp_send(src_ip, DN_BEACON_PORT, DN_BEACON_PORT, (uint8_t*)buf, my_strlen(buf));
    net_log("[DISTNET] beacon -> "); log_ip(src_ip); net_log("\n");
}

// Execute a task and return a RESULT string into result[] ( NUL-terminated ).
// Buffer enlarged to 400 bytes so a real-GGUF answer (qwen) fits in one RESULT.
static void exec_task(char* task, char result[400]){
    int rp = 0;

    // ---- AI task (special case: prompt may contain spaces) ----
    // Wire form:  TASK <id> ai <prompt...>
    // We parse this by hand instead of via tokenize(), because tokenize()
    // NUL-terminates inter-token spaces and would shred a multi-word prompt.
    if (my_strlen(task) >= 4 && task[0]=='T' && task[1]=='A' &&
        task[2]=='S' && task[3]=='K'){
        char* p = task + 4;
        while (*p==' '||*p=='\t') p++;
        char* id = p;
        while (*p && *p!=' ' && *p!='\t') p++;
        int id_end = (int)(p - task);          // index of first space/id terminator
        while (*p==' '||*p=='\t') p++;          // skip spaces -> now at <type>
        if (p[0]=='a' && p[1]=='i' &&
            (p[2]==' '||p[2]=='\t'||p[2]==0)){
            // type == "ai": prompt is everything after "ai "
            task[id_end] = 0;                   // terminate <id> (safe: we return)
            char* q = p + 2;                    // skip "ai"
            while (*q==' '||*q=='\t') q++;      // skip spaces before prompt
            rcat(result, &rp, "RESULT ");
            rcat(result, &rp, id);
            rcat(result, &rp, " ok ");
            char ai_out[401];
            int rc = kern_ai_ask(q, ai_out, 401);
            if (rc < 0){
                // -2 == engine could not boot (no /boot/model.gguf): report
                // gracefully so the scheduler + user understand, instead of
                // hanging or faulting.
                rcat(result, &rp, (rc == -2) ? "err no_model" : "err ai_fail");
            } else {
                rcat(result, &rp, ai_out);
            }
            return;
        }
        // not "ai" -> fall through to the tokenized numeric/text path below
    }

    char* tok[16]; int tl[16];
    int nt = tokenize(task, tok, tl, 16);
    // expect: TASK <id> <type> <args...>
    if (nt < 4){ rcat(result, &rp, "RESULT ? err bad_task"); return; }
    const char* id   = tok[1];
    const char* type = tok[2];

    rcat(result, &rp, "RESULT ");
    rcat(result, &rp, id);
    rcat(result, &rp, " ok ");

    if (my_strlen(type) == 3 && type[0]=='s' && type[1]=='u' && type[2]=='m'){
        unsigned sum = 0;
        for (int i=3; i<nt; i++){
            unsigned v=0;
            for (int j=0; j<tl[i]; j++) v = v*10 + (unsigned)(tok[i][j]-'0');
            sum += v;
        }
        char vbuf[12]; int vl = my_utoa(sum, vbuf);
        for (int i=0;i<vl;i++) result[rp++] = vbuf[i];
        result[rp] = 0;
    }
    else if (my_strlen(type) == 4 && type[0]=='e' && type[1]=='c' && type[2]=='h' && type[3]=='o'){
        for (int i=3; i<nt; i++){
            for (int j=0;j<tl[i];j++) result[rp++] = tok[i][j];
            if (i < nt-1) result[rp++] = ' ';
        }
        result[rp] = 0;
    }
    else if (my_strlen(type) == 7 && type[0]=='c' && type[1]=='o' && type[2]=='m' &&
             type[3]=='p' && type[4]=='u' && type[5]=='t' && type[6]=='e'){
        unsigned n=0;
        if (nt>=4){ for (int j=0;j<tl[3];j++) n = n*10 + (unsigned)(tok[3][j]-'0'); }
        unsigned sq = n*n;
        char vbuf[12]; int vl = my_utoa(sq, vbuf);
        for (int i=0;i<vl;i++) result[rp++] = vbuf[i];
        result[rp] = 0;
    }
    else if (my_strlen(type) == 3 && type[0]=='n' && type[1]=='e' && type[2]=='g'){
        // neg <int> -> signed negation as a decimal string ("-N" / "0")
        if (nt >= 4){
            unsigned n = parse_uint(tok[3], tl[3]);
            if (n != 0) result[rp++] = '-';          /* magnitude is |n| */
            ucat(result, &rp, n);
        } else {
            rcat(result, &rp, "err bad_args");
        }
    }
    else if (my_strlen(type) == 3 && type[0]=='f' && type[1]=='i' && type[2]=='b'){
        // fib <N> -> Nth Fibonacci, F_0=0, F_1=1 (iterative, u64 width)
        if (nt >= 4){
            unsigned n = parse_uint(tok[3], tl[3]);
            unsigned long long a=0, b=1;
            for (unsigned i=0; i<n; i++){ unsigned long long t = a+b; a=b; b=t; }
            char fb[32]; int fl=0; unsigned long long v=a;
            if (v==0){ fb[fl++]='0'; }
            else { char tmp[32]; int tn=0; while(v){ tmp[tn++]=(char)('0'+(v%10)); v/=10; }
                   while(tn) fb[fl++]=tmp[--tn]; }
            for (int i=0;i<fl;i++) result[rp++]=fb[i];
            result[rp]=0;
        } else {
            rcat(result, &rp, "err bad_args");
        }
    }
    else if (my_strlen(type) == 5 && type[0]=='p' && type[1]=='r' && type[2]=='i' &&
             type[3]=='m' && type[4]=='e'){
        // prime <N> -> count of primes in [2..N] (trial division)
        if (nt >= 4){
            unsigned n = parse_uint(tok[3], tl[3]);
            unsigned cnt = 0;
            for (unsigned x=2; x<=n; x++){
                unsigned d; int isp = 1;
                for (d=2; d*d<=x; d++){ if (x % d == 0){ isp=0; break; } }
                if (isp) cnt++;
            }
            ucat(result, &rp, cnt);
        } else {
            rcat(result, &rp, "err bad_args");
        }
    }
    else if (my_strlen(type) == 3 && type[0]=='g' && type[1]=='c' && type[2]=='d'){
        // gcd <a> <b> -> greatest common divisor (Euclid)
        if (nt >= 5){
            unsigned a = parse_uint(tok[3], tl[3]);
            unsigned b = parse_uint(tok[4], tl[4]);
            while (b){ unsigned t = a % b; a = b; b = t; }
            ucat(result, &rp, a);
        } else {
            rcat(result, &rp, "err bad_args");
        }
    }
    else {
        rcat(result, &rp, "err unknown_type");
    }
}

static void compute_on_task(uint32_t src_ip, uint16_t src_port,
                            const uint8_t* data, int len){
    (void)src_port;
    // Copy into a mutable buffer for tokenization.  Sized larger than the
    // RESULT so an "ai" task with a long prompt fits.
    static char buf[512];
    int c = (len < 511) ? len : 511;
    for (int i=0;i<c;i++) buf[i] = (char)data[i];
    buf[c] = 0;

    char result[400];
    exec_task(buf, result);
    net_udp_send(src_ip, DN_RESULT_PORT, DN_RESULT_PORT, (uint8_t*)result, my_strlen(result));
    net_log("[DISTNET] task done -> "); log_ip(src_ip);
    net_log(" : "); net_log(result); net_log("\n");
}

void distnet_compute(void){
    net_udp_bind(DN_BEACON_PORT, compute_on_beacon);
    net_udp_bind(DN_TASK_PORT,   compute_on_task);
    net_log("[DISTNET] compute node online (beacon@5455 task@5456)\n");
    // Bounded event loop: stay responsive for a while, then return to shell.
    // (A production node would park this on a background thread / timer.)
    int guard = 0;
    while (guard++ < 8000000) net_poll();
    net_log("[DISTNET] compute node idle-exit\n");
}

// =====================================================================
//  SCHEDULER role
// =====================================================================
static uint32_t g_node_ip   = 0;
static int      g_got_beacon = 0;
static char     g_result[400];
static int      g_got_result = 0;

// ---- multi-node support (for distnet ask: merge several compute nodes) ----
#define DN_MAX_ANSWERS 4
static uint32_t g_ans_ip[DN_MAX_ANSWERS];
static char     g_answers[DN_MAX_ANSWERS][400];
static int      g_ans_count = 0;

static void sched_on_beacon(uint32_t src_ip, uint16_t src_port,
                            const uint8_t* data, int len){
    (void)data; (void)len; (void)src_port;
    g_node_ip = src_ip;
    g_got_beacon = 1;
    net_log("[DISTNET] discovered compute node "); log_ip(src_ip); net_log("\n");
}

static void sched_on_result(uint32_t src_ip, uint16_t src_port,
                            const uint8_t* data, int len){
    (void)src_ip; (void)src_port;
    int c = (len < 199) ? len : 199;
    for (int i=0;i<c;i++) g_result[i] = (char)data[i];
    g_result[c] = 0;
    g_got_result = 1;
}

// Multi-node RESULT collector: store every reply (up to DN_MAX_ANSWERS).
static void ask_on_result(uint32_t src_ip, uint16_t src_port,
                          const uint8_t* data, int len){
    (void)src_port;
    if (g_ans_count >= DN_MAX_ANSWERS) return;
    int idx = g_ans_count++;
    g_ans_ip[idx] = src_ip;
    int c = (len < 399) ? len : 399;
    for (int i=0;i<c;i++) g_answers[idx][i] = (char)data[i];
    g_answers[idx][c] = 0;
    net_log("[DISTNET] answer from "); log_ip(src_ip); net_log("\n");
}

// Run a full discovery + dispatch + wait cycle for an arbitrary TASK line.
// `task` is sent verbatim to the discovered compute node (its internal
// spaces are preserved, so it may be an "ai" task with a multi-word prompt).
static void scheduler_run(const char* task, const char* target_ip){
    g_node_ip = 0; g_got_beacon = 0; g_got_result = 0;
    g_result[0] = 0;

    net_udp_bind(DN_BEACON_PORT, sched_on_beacon);
    net_udp_bind(DN_RESULT_PORT, sched_on_result);

    // 1) discover -- periodically re-send QUERY (mDNS-style retry).
    //    A freshly-booted compute node receives the broadcast QUERY, but its
    //    first BEACON reply ARP-misses us and is dropped (ip_send() sends an
    //    ARP request and returns).  Our ARP reply lets the node learn our MAC,
    //    so the NEXT QUERY elicits a successful BEACON.  Re-sending covers
    //    that handshake latency instead of relying on a single shot.
    char q[16];
    my_strcpy(q, "QUERY");
    const int QUERY_INTERVAL = 20000;   // polls to wait between QUERYs
    const int QUERY_MAX = 15;           // give up after this many QUERYs
    int queries_sent = 0;
    while (!g_got_beacon && queries_sent < QUERY_MAX){
        if (target_ip && target_ip[0]){
            uint32_t tip = parse_ip(target_ip);
            if (!tip){ net_log("[DISTNET] bad target IP\n"); return; }
            net_udp_send(tip, DN_BEACON_PORT, DN_BEACON_PORT, (uint8_t*)q, my_strlen(q));
            net_log("[DISTNET] scheduler: QUERY -> "); log_ip(tip); net_log("\n");
        } else {
            net_udp_broadcast(DN_BEACON_PORT, DN_BEACON_PORT, (uint8_t*)q, my_strlen(q));
            net_log("[DISTNET] scheduler: QUERY broadcast\n");
        }
        queries_sent++;

        // Wait for a beacon, bounded; net_poll() also services the ARP
        // request the compute node fires when its first BEACON ARP-misses.
        int guard = 0;
        while (!g_got_beacon && guard++ < QUERY_INTERVAL) net_poll();
    }
    if (!g_got_beacon){ net_log("[DISTNET] no compute nodes found\n"); return; }

    // 2) dispatch the task to the discovered node.
    // Send from RESULT_PORT (5457) so the reply SLIRP routes back to our
    // RESULT socket (handle_udp dispatches by destination port).
    net_udp_send(g_node_ip, DN_RESULT_PORT, DN_TASK_PORT, (uint8_t*)task, my_strlen(task));
    net_log("[DISTNET] TASK sent, awaiting RESULT...\n");

    int guard = 0;
    // AI inference on the compute node is SLOW (Markov train + generate under
    // TCG can take many seconds), so give the RESULT window generous room.
    while (!g_got_result && guard++ < 3000000) net_poll();
    if (g_got_result){
        net_log("[DISTNET] RESULT: "); net_log(g_result); net_log("\n");
    } else {
        net_log("[DISTNET] timed out waiting for result\n");
    }
    net_log("[DISTNET] scheduler done\n");
}

// =====================================================================
//  ASK role:  discover ALL compute nodes, ask each the SAME question, then
//  MERGE their answers into one consolidated reply.  This demonstrates two
//  (or more) machines' compute power being combined to answer the user.
// =====================================================================
void distnet_scheduler_ask(const char* prompt){
    if (!prompt || !prompt[0]){
        net_log("[DISTNET] usage: distnet ask \"<question>\"\n");
        return;
    }

    // --- discover all nodes via repeated QUERY broadcasts ---
    for (int i=0;i<DN_MAX_NODES;i++) g_nodes[i]=0;
    g_node_count = 0;
    net_udp_bind(DN_BEACON_PORT, nodes_on_beacon);

    char q[16];
    my_strcpy(q, "QUERY");
    const int QUERY_INTERVAL = 20000;
    const int QUERY_MAX = 15;
    for (int tries=0; tries<QUERY_MAX && g_node_count<2; tries++){
        net_udp_broadcast(DN_BEACON_PORT, DN_BEACON_PORT, (uint8_t*)q, my_strlen(q));
        int guard=0;
        while (guard++ < QUERY_INTERVAL) net_poll();
    }
    if (g_node_count == 0){
        net_log("[DISTNET] ask: no compute nodes discovered\n");
        return;
    }
    net_log("[DISTNET] ask: discovered ");
    log_uint((unsigned)g_node_count);
    net_log(" node(s)\n");

    // --- ask every discovered node the SAME question ---
    net_udp_bind(DN_RESULT_PORT, ask_on_result);
    g_ans_count = 0;

    char task[256];
    int rp = 0;
    rcat(task, &rp, "TASK 1 ai ");
    int plen = my_strlen(prompt);
    int start = 0, end = plen;
    if (plen >= 2 && prompt[0]=='"' && prompt[plen-1]=='"'){ start=1; end=plen-1; }
    for (int i=start;i<end;i++) task[rp++] = prompt[i];
    task[rp] = 0;

    for (int n=0; n<g_node_count; n++){
        net_udp_send(g_nodes[n], DN_RESULT_PORT, DN_TASK_PORT,
                     (uint8_t*)task, my_strlen(task));
        net_log("[DISTNET] ask: dispatched to "); log_ip(g_nodes[n]); net_log("\n");
    }

    // --- collect all answers (give each node generous time) ---
    int guard=0;
    while (g_ans_count < g_node_count && guard++ < 6000000) net_poll();

    // --- merge / present ---
    net_log("[DISTNET] =======================================\n");
    net_log("[DISTNET] MERGED ANSWER (from ");
    log_uint((unsigned)g_ans_count);
    net_log(" node(s))\n");
    net_log("[DISTNET] =======================================\n");
    for (int n=0; n<g_ans_count; n++){
        net_log("[DISTNET] -- node "); log_ip(g_ans_ip[n]); net_log(" said:\n");
        net_log(g_answers[n]);
        net_log("\n");
    }
    net_log("[DISTNET] =======================================\n");
}

// Generalized scheduler entry: discover (target_ip==NULL => broadcast) and
// dispatch "TASK 1 <type> <args>".  If type is NULL, use the classic sum demo.
// `args` may contain spaces (it is appended verbatim; the compute node parses
// the rest of the line as the task payload, so "ai" prompts survive intact).
void distnet_scheduler(const char* target_ip, const char* type, const char* args){
    char task[256];
    int rp = 0;
    rcat(task, &rp, "TASK 1 ");
    if (type && type[0]){
        rcat(task, &rp, type);
        if (args && args[0]){
            rcat(task, &rp, " ");
            rcat(task, &rp, args);
        }
    } else {
        rcat(task, &rp, "sum 1 2 3 4 5");
    }
    scheduler_run(task, target_ip);
}

// Scheduler entry that dispatches an AI inference task:  TASK 1 ai <prompt>
// Surrounding double-quotes on the prompt are stripped (they are shell
// quoting, not part of the prompt).  Internal spaces are preserved.
void distnet_scheduler_ai(const char* prompt, const char* target_ip){
    if (!prompt || !prompt[0]){
        net_log("[DISTNET] usage: distnet ai \"<prompt>\"\n");
        return;
    }
    char task[256];
    int rp = 0;
    rcat(task, &rp, "TASK 1 ai ");
    int plen = my_strlen(prompt);
    int start = 0, end = plen;
    if (plen >= 2 && prompt[0]=='"' && prompt[plen-1]=='"'){ start = 1; end = plen - 1; }
    for (int i = start; i < end; i++) task[rp++] = prompt[i];
    task[rp] = 0;
    scheduler_run(task, target_ip);
}

// =====================================================================
//  NODES role: discovery-only scan that lists every compute peer heard.
//  (g_nodes / g_node_count / nodes_on_beacon declared near the top so the
//   scheduler_ask path above can see them.)
// =====================================================================
static void nodes_on_beacon(uint32_t src_ip, uint16_t src_port,
                            const uint8_t* data, int len){
    (void)data; (void)len; (void)src_port;
    // dedupe: add if not already in the list
    for (int i=0; i<g_node_count; i++) if (g_nodes[i]==src_ip) return;
    if (g_node_count < DN_MAX_NODES) g_nodes[g_node_count++] = src_ip;
    net_log("[DISTNET] nodes: discovered "); log_ip(src_ip); net_log("\n");
}

void distnet_nodes(const char* target_ip){
    g_node_count = 0;
    net_udp_bind(DN_BEACON_PORT, nodes_on_beacon);

    char q[16];
    my_strcpy(q, "QUERY");
    const int QUERY_INTERVAL = 20000;
    const int QUERY_MAX = 15;
    int queries_sent = 0;
    while (queries_sent < QUERY_MAX){
        if (target_ip && target_ip[0]){
            uint32_t tip = parse_ip(target_ip);
            if (!tip){ net_log("[DISTNET] bad target IP\n"); return; }
            net_udp_send(tip, DN_BEACON_PORT, DN_BEACON_PORT, (uint8_t*)q, my_strlen(q));
        } else {
            net_udp_broadcast(DN_BEACON_PORT, DN_BEACON_PORT, (uint8_t*)q, my_strlen(q));
        }
        queries_sent++;
        int guard = 0;
        while (guard++ < QUERY_INTERVAL) net_poll();
        if (g_node_count >= DN_MAX_NODES) break;
    }

    if (g_node_count == 0){
        net_log("[DISTNET] nodes: none found\n");
        return;
    }
    net_log("[DISTNET] nodes: ");
    for (int i=0;i<g_node_count;i++){
        if (i) net_log(", ");
        log_ip(g_nodes[i]);
    }
    net_log("\n");
    net_log("[DISTNET] nodes: total ");
    log_uint((unsigned)g_node_count);
    net_log("\n");
}

// =====================================================================
//  Shell command entry
// =====================================================================
// True if s (may not be NUL-terminated beyond a token boundary) looks like a
// dotted IPv4 "a.b.c.d" rather than a task type name.  Used to disambiguate
// "distnet scheduler <ip> ..." from "distnet scheduler <type> <args>...".
static int looks_like_ip(const char* s){
    int dots = 0, digits = 0;
    for (int i=0; s[i] && s[i]!=' ' && s[i]!='\t'; i++){
        if (s[i]=='.') dots++;
        else if (s[i]>='0' && s[i]<='9') digits++;
        else return 0;               /* letters or symbols => a type name */
    }
    return (dots == 3 && digits > 0);
}

void cmd_distnet(const char* args){
    // args points just past "distnet" (may be NULL or empty)
    const char* p = args ? args : "";
    while (*p==' '||*p=='\t') p++;

    if (*p == 0){
        net_log("distnet: usage\n");
        net_log("  distnet compute                    run as compute node\n");
        net_log("  distnet nodes [ip]                 discovery-only: list peers\n");
        net_log("  distnet scheduler [<type> <args>]  discover + dispatch a task\n");
        net_log("    types: sum/echo/compute/neg/fib/prime/gcd\n");
        net_log("    default (no type): sum 1 2 3 4 5\n");
        net_log("  distnet scheduler <ip> [<type> <args>]  unicast to <ip>\n");
        net_log("  distnet ai \"<prompt>\"              dispatch an AI inference task\n");
        net_log("  distnet ask \"<question>\"           ask ALL nodes, merge their answers\n");
        return;
    }
    if (p[0]=='c' && p[1]=='o' && p[2]=='m' && p[3]=='p'){
        distnet_compute();
        return;
    }
    if (p[0]=='n' && p[1]=='o' && p[2]=='d' && p[3]=='e' && p[4]=='s'){
        // distnet nodes [ip]
        const char* q = p + 5;
        while (*q==' '||*q=='\t') q++;
        distnet_nodes(*q ? q : NULL);
        return;
    }
    if (p[0]=='s' && p[1]=='c' && p[2]=='h'){
        // find end of "scheduler", then examine the rest.
        const char* q = p;
        while (*q && *q!=' ' && *q!='\t') q++;
        while (*q==' '||*q=='\t') q++;
        const char* target = NULL;
        if (looks_like_ip(q)){
            target = q;
            while (*q && *q!=' ' && *q!='\t') q++;
            while (*q==' '||*q=='\t') q++;
        }
        // Now q points at the first task-type token (or empty).
        if (*q == 0){
            distnet_scheduler(target, NULL, NULL);       /* default sum demo */
        } else {
            const char* type = q;
            while (*q && *q!=' ' && *q!='\t') q++;
            int type_end = (int)(q - type);
            // args = the rest of the line (preserves internal spaces for "ai").
            char typebuf[16]; int tlen = (type_end < 15) ? type_end : 15;
            for (int i=0;i<tlen;i++) typebuf[i]=type[i];
            typebuf[tlen]=0;
            const char* args2 = q;
            while (*args2==' '||*args2=='\t') args2++;
            distnet_scheduler(target, typebuf, args2);
        }
        return;
    }
    if (p[0]=='a' && p[1]=='i'){
        // everything after "ai" (skip spaces) is the prompt
        const char* q = p + 2;
        while (*q==' '||*q=='\t') q++;
        distnet_scheduler_ai(q, NULL);
        return;
    }
    if (p[0]=='a' && p[1]=='s' && p[2]=='k'){
        // everything after "ask" (skip spaces) is the question
        const char* q = p + 3;
        while (*q==' '||*q=='\t') q++;
        distnet_scheduler_ask(q);
        return;
    }
    net_log("distnet: unknown subcommand\n");
}
