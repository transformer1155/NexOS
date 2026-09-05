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
    // TCG/SLIRP 下 UDP 往返(guest -> host peer -> guest)真实耗时远大于 20000 次
    // 空转,BEACON 会晚于等待窗口到达(表现为 "no compute nodes" 但节点随后
    // 才被 discover)。放大等待窗口,让每次 QUERY 后有足够时间收到回程。
    const int QUERY_INTERVAL = 400000;  // polls to wait between QUERYs
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

// =====================================================================
//  Self-managing agent (自管理 Agent)
// ---------------------------------------------------------------------
//  说明:本子系统曾在一次误删事故中丢失(从未提交 git),这里是**重写**实现,
//  行为与对外输出格式按前端面板(win11-ui/nexos-desktop.html)已依赖的
//  [AGENT] 输出协议重建,保证 UI 无需改动即可继续解析。
//
//  能力:自动发现节点 → 维护节点表(role/weight/busy) → 队列任务 →
//       按角色与权重挑选空闲节点下发 → 支持身份移交(handoff)。
//  运行:dn_agent_tick() 由 net.cpp 的 net_poll() 每轮调用(见 net.cpp 的
//       g_agent_running / dn_agent_tick 外部声明)。
// =====================================================================
#define DN_AGENT_MAX_NODES  8
#define DN_AGENT_MAX_QUEUE  8
#define DN_AGENT_TASK_MAX   256
#define DN_ROLE_COMPUTE     0
#define DN_ROLE_AI          1
#define DN_ROLE_STORAGE     2

struct dn_agent_node { uint32_t ip; int role; int weight; int busy; };

static struct dn_agent_node g_agent_nodes[DN_AGENT_MAX_NODES];
static int      g_agent_node_count = 0;
int             g_agent_running = 0;               // net.cpp 外部引用
static char     g_agent_identity[32];
static char     g_agent_qtype[DN_AGENT_MAX_QUEUE][DN_AGENT_TASK_MAX];
static int      g_agent_qai[DN_AGENT_MAX_QUEUE];   // 1 = 需要 ai 角色节点
static int      g_agent_qcount = 0;                // 队列中的任务数
static uint32_t g_agent_handoff_ip = 0;
static unsigned long g_agent_tick_n = 0;

// 每 20000 轮 net_poll 做一次发现/推进(近似“心跳”,内核无定时器)
#define DN_AGENT_TICK_DISCOVER 20000UL

static const char* dn_role_name(int r){
    return (r == DN_ROLE_AI) ? "ai" : ((r == DN_ROLE_STORAGE) ? "storage" : "compute");
}
static int dn_role_from_name(const char* s){
    if (!s || !s[0]) return DN_ROLE_COMPUTE;
    if (s[0]=='a' && s[1]=='i') return DN_ROLE_AI;
    if (s[0]=='s') return DN_ROLE_STORAGE;
    return DN_ROLE_COMPUTE;
}

// 按 IP 查找节点(未找到返回 -1)
static int dn_agent_find(const char* ip){
    uint32_t a = parse_ip(ip);
    for (int i = 0; i < g_agent_node_count; i++)
        if (g_agent_nodes[i].ip == a) return i;
    return -1;
}

// 发现回包:登记/刷新节点
static void agent_on_beacon(uint32_t src_ip, uint16_t src_port,
                            const uint8_t* data, int len){
    (void)src_port; (void)data; (void)len;
    for (int i = 0; i < g_agent_node_count; i++){
        if (g_agent_nodes[i].ip == src_ip){
            net_log("[AGENT] node seen "); log_ip(src_ip); net_log("\n");
            return;
        }
    }
    if (g_agent_node_count >= DN_AGENT_MAX_NODES) return;
    g_agent_nodes[g_agent_node_count].ip     = src_ip;
    g_agent_nodes[g_agent_node_count].role   = DN_ROLE_COMPUTE;
    g_agent_nodes[g_agent_node_count].weight = 1;
    g_agent_nodes[g_agent_node_count].busy   = 0;
    g_agent_node_count++;
    net_log("[AGENT] added node "); log_ip(src_ip); net_log("\n");
}

// 结果回包:释放节点 busy 标志并打印结果
static void agent_on_result(uint32_t src_ip, uint16_t src_port,
                            const uint8_t* data, int len){
    (void)src_port;
    for (int i = 0; i < g_agent_node_count; i++)
        if (g_agent_nodes[i].ip == src_ip) g_agent_nodes[i].busy = 0;
    net_log("[AGENT] result from "); log_ip(src_ip); net_log(": ");
    int c = len < 199 ? len : 199;
    for (int i = 0; i < c; i++) { char t[2]; t[0] = (char)data[i]; t[1] = 0; net_log(t); }
    net_log("\n");
}

// 入队:type=任务类型(如 fib / sum),args=参数,is_ai=1 表示需要 ai 角色
static int dn_agent_enqueue(const char* type, const char* args, int is_ai){
    if (g_agent_qcount >= DN_AGENT_MAX_QUEUE) return -1;
    int slot = g_agent_qcount++;
    int rp = 0;
    rcat(g_agent_qtype[slot], &rp, type ? type : "sum");
    if (args && args[0]){ rcat(g_agent_qtype[slot], &rp, " "); rcat(g_agent_qtype[slot], &rp, args); }
    g_agent_qai[slot] = is_ai ? 1 : 0;
    return slot;
}

// 把队首任务下发给一个合适的空闲节点;成功返回 1
static int dn_agent_dispatch_head(void){
    if (g_agent_qcount <= 0) return 0;
    int need_ai = g_agent_qai[0];
    for (int i = 0; i < g_agent_node_count; i++){
        struct dn_agent_node* n = &g_agent_nodes[i];
        if (n->busy) continue;
        if (need_ai && n->role != DN_ROLE_AI) continue;
        // 取任务并下发(复用 scheduler_run 的 TASK 线格式)
        char task[DN_AGENT_TASK_MAX];
        int rp = 0;
        rcat(task, &rp, "TASK 1 ");
        rcat(task, &rp, g_agent_qtype[0]);
        // 出队(前移)
        for (int k = 1; k < g_agent_qcount; k++){
            int w = 0;
            g_agent_qtype[k-1][w = 0] = 0;
            for (int j = 0; g_agent_qtype[k][j]; j++) g_agent_qtype[k-1][j] = g_agent_qtype[k][j];
            g_agent_qtype[k-1][my_strlen(g_agent_qtype[k])] = 0;
            g_agent_qai[k-1] = g_agent_qai[k];
            (void)w;
        }
        g_agent_qcount--;
        n->busy = 1;
        net_udp_send(n->ip, DN_TASK_PORT, DN_TASK_PORT,
                     (const uint8_t*)task, my_strlen(task));
        net_log("[AGENT] dispatch "); net_log(task);
        net_log(" -> "); log_ip(n->ip); net_log("\n");
        return 1;
    }
    return 0;
}

// 广播一次 QUERY 以发现节点
static void dn_agent_discover_once(void){
    const char* q = "QUERY";
    net_udp_bind(DN_BEACON_PORT, agent_on_beacon);
    net_udp_bind(DN_RESULT_PORT, agent_on_result);
    net_udp_broadcast(DN_BEACON_PORT, DN_BEACON_PORT, (const uint8_t*)q, 5);
}

// 推进队列
static void dn_agent_progress(void){
    while (dn_agent_dispatch_head()) { /* 尽量把队列排空 */ }
}

// net_poll() 每轮调用
void dn_agent_tick(void){
    if (!g_agent_running) return;
    if (++g_agent_tick_n % DN_AGENT_TICK_DISCOVER == 0){
        dn_agent_discover_once();
        dn_agent_progress();
    }
}

// ---- 命令实现 ----
static void dn_agent_start(const char* identity){
    g_agent_running = 1;
    g_agent_tick_n = 0;
    my_strcpy(g_agent_identity, identity && identity[0] ? identity : "nexos-agent");
    net_log("[AGENT] started as "); net_log(g_agent_identity); net_log("\n");
    dn_agent_discover_once();
}
static void dn_agent_stop(void){
    g_agent_running = 0;
    net_log("[AGENT] stopped\n");
}
static void dn_agent_add(const char* ip, const char* role, const char* w){
    if (!ip || !ip[0]){ net_log("[AGENT] usage: add <ip> [role] [weight]\n"); return; }
    int idx = dn_agent_find(ip);
    if (idx < 0){
        if (g_agent_node_count >= DN_AGENT_MAX_NODES){ net_log("[AGENT] node table full\n"); return; }
        idx = g_agent_node_count++;
        g_agent_nodes[idx].ip = parse_ip(ip);
        g_agent_nodes[idx].busy = 0;
        g_agent_nodes[idx].role = DN_ROLE_COMPUTE;
        g_agent_nodes[idx].weight = 1;
    }
    if (role && role[0]) g_agent_nodes[idx].role = dn_role_from_name(role);
    if (w && w[0]){
        int v = 0; const char* s = w;
        while (*s >= '0' && *s <= '9'){ v = v * 10 + (*s - '0'); s++; }
        if (v > 0) g_agent_nodes[idx].weight = v;
    }
    net_log("[AGENT] node "); log_ip(g_agent_nodes[idx].ip);
    net_log(" role="); net_log(dn_role_name(g_agent_nodes[idx].role));
    net_log(" weight="); { char nb[16]; int rp=0; ucat(nb,&rp,(unsigned)g_agent_nodes[idx].weight); nb[rp]=0; net_log(nb); }
    net_log("\n");
}
static void dn_agent_del(const char* ip){
    int idx = dn_agent_find(ip);
    if (idx < 0){ net_log("[AGENT] node not found\n"); return; }
    for (int i = idx; i < g_agent_node_count - 1; i++) g_agent_nodes[i] = g_agent_nodes[i+1];
    g_agent_node_count--;
    net_log("[AGENT] removed "); net_log(ip); net_log("\n");
}
static void dn_agent_status(void){
    net_log("[AGENT] identity: "); net_log(g_agent_identity[0] ? g_agent_identity : "(unnamed)"); net_log("\n");
    net_log("[AGENT] running: "); net_log(g_agent_running ? "yes" : "no"); net_log("\n");
    net_log("[AGENT] nodes ("); { char nb[16]; int rp=0; ucat(nb,&rp,(unsigned)g_agent_node_count); nb[rp]=0; net_log(nb); } net_log("):\n");
    for (int i = 0; i < g_agent_node_count; i++){
        struct dn_agent_node* n = &g_agent_nodes[i];
        net_log("  "); log_ip(n->ip);
        net_log(" role="); net_log(dn_role_name(n->role));
        net_log(" weight="); { char nb[16]; int rp=0; ucat(nb,&rp,(unsigned)n->weight); nb[rp]=0; net_log(nb); }
        net_log(" busy="); net_log(n->busy ? "1" : "0"); net_log("\n");
    }
    net_log("[AGENT] queue ("); { char nb[16]; int rp=0; ucat(nb,&rp,(unsigned)g_agent_qcount); nb[rp]=0; net_log(nb); } net_log(")\n");
    if (g_agent_handoff_ip){
        net_log("[AGENT] pending handoff -> "); log_ip(g_agent_handoff_ip); net_log("\n");
    }
}

//  Sharded inference - orchestrator role (model sharding)
// =====================================================================
static const uint16_t dn_shard_ports[DN_SHARD_MAX_NODES] = { 5501, 5502, 5503 };

struct dn_shnode { uint32_t ip; uint16_t port; int weight; int start; int end; int last; };
static struct dn_shnode g_shn[DN_SHARD_MAX_NODES];
static int      g_shn_found = 0;
static int      g_sh_layers = 0, g_sh_dim = 0;
static char     g_sh_job[16];
static int      g_sh_got_out = 0;
static char     g_sh_out[600];

static int dn_atoi(const char* s){
    int v = 0, neg = 0;
    if (*s == '-'){ neg = 1; s++; }
    while (*s >= '0' && *s <= '9'){ v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

// 把 0.1*tenths 格式化成 "d.dddddd"(整数运算,不依赖 %f)
static void dn_put_tenths(char* out, int* rp, int tenths){
    char t[24]; int k = 0;
    int ip = tenths / 10;
    int fp = (tenths % 10) * 100000;
    if (ip == 0) { t[k++] = '0'; }
    else { char tmp[8]; int m = 0; while (ip){ tmp[m++] = (char)('0' + (ip % 10)); ip /= 10; } while (m) t[k++] = tmp[--m]; }
    t[k++] = '.';
    for (int p = 100000; p >= 1; p /= 10) t[k++] = (char)('0' + ((fp / p) % 10));
    t[k] = 0;
    rcat(out, rp, t);
}

static struct dn_shnode* dn_shnode_by(uint32_t ip, uint16_t port){
    for (int i = 0; i < DN_SHARD_MAX_NODES; i++)
        if (g_shn[i].ip == ip && g_shn[i].port == port) return &g_shn[i];
    return 0;
}

// 统一收包:按前缀分发 BEACON / OUT / CKPT
static void dn_shard_recv(uint32_t src_ip, uint16_t src_port,
                          const uint8_t* data, int len){
    char buf[620];
    int n = len < 619 ? len : 619;
    for (int i = 0; i < n; i++) buf[i] = (char)data[i];
    buf[n] = 0;
    (void)src_port;

    if (buf[0]=='B' && buf[1]=='E' && buf[2]=='A' && buf[3]=='C' && buf[4]=='O' && buf[5]=='N'){
        // "BEACON compute <weight>"
        char* tok[8]; int tl[8];
        int nt = tokenize(buf, tok, tl, 8);
        int w = (nt >= 3) ? dn_atoi(tok[2]) : 1;
        if (w <= 0) w = 1;
        struct dn_shnode* n2 = dn_shnode_by(src_ip, src_port);
        if (!n2){
            for (int i = 0; i < DN_SHARD_MAX_NODES; i++){
                if (g_shn[i].weight == 0){
                    g_shn[i].ip = src_ip; g_shn[i].port = src_port;
                    g_shn[i].weight = w; g_shn_found++; n2 = &g_shn[i]; break;
                }
            }
        }
        if (n2) n2->weight = w;
        net_log("[DISTNET] shard: BEACON "); log_ip(src_ip);
        net_log(" w="); { char nb[16]; int rp=0; ucat(nb,&rp,(unsigned)w); nb[rp]=0; net_log(nb); net_log("\n"); }
        return;
    }
    if (buf[0]=='O' && buf[1]=='U' && buf[2]=='T'){
        if (!g_sh_got_out){
            int k = 0;
            while (k < n && k < 599){ g_sh_out[k] = buf[k]; k++; }
            g_sh_out[k] = 0;
            g_sh_got_out = 1;
        }
        return;
    }
    // CKPT: 记录检查点可用于后续容错续跑,这里先忽略(层计算在节点侧)
}

static void dn_shard_plan(void){
    int tot = 0;
    for (int i = 0; i < DN_SHARD_MAX_NODES; i++) if (g_shn[i].weight > 0) tot += g_shn[i].weight;
    if (tot <= 0) tot = 1;
    int cur = 0, used = 0;
    for (int i = 0; i < DN_SHARD_MAX_NODES; i++){
        if (g_shn[i].weight <= 0) continue;
        int n = (g_sh_layers * g_shn[i].weight) / tot;
        if (used == g_shn_found - 1) n = g_sh_layers - cur;   // 末段吃下余数
        if (n <= 0) n = 1;
        g_shn[i].start = cur; g_shn[i].end = cur + n - 1;
        g_shn[i].last = (used == g_shn_found - 1);
        cur += n; used++;
    }
}

// 发现节点(QUERY/BEACON)
static int dn_shard_discover(uint32_t ip){
    g_shn_found = 0;
    for (int i = 0; i < DN_SHARD_MAX_NODES; i++){
        g_shn[i].ip = 0; g_shn[i].port = 0; g_shn[i].weight = 0;
        g_shn[i].start = 0; g_shn[i].end = -1; g_shn[i].last = 0;
    }
    net_udp_bind(DN_SHARD_PORT, dn_shard_recv);
    const char* q = "QUERY";
    for (int i = 0; i < DN_SHARD_MAX_NODES; i++)
        net_udp_send(ip, DN_SHARD_PORT, dn_shard_ports[i], (const uint8_t*)q, 5);
    // 等待回包(内核无定时器,用有界轮询;SLIRP 往返较慢故给足窗口)
    unsigned long guard = 0;
    while (g_shn_found < DN_SHARD_MAX_NODES && guard++ < 1500000UL) net_poll();
    return g_shn_found;
}

void distnet_shard_probe(uint32_t node_ip){
    int n = dn_shard_discover(node_ip);
    net_log("[DISTNET] shard nodes found: ");
    { char nb[16]; int rp=0; ucat(nb,&rp,(unsigned)n); nb[rp]=0; net_log(nb); net_log("\n"); }
    for (int i = 0; i < DN_SHARD_MAX_NODES; i++){
        if (g_shn[i].weight <= 0) continue;
        net_log("  "); log_ip(g_shn[i].ip);
        net_log(":"); { char nb[16]; int rp=0; ucat(nb,&rp,(unsigned)g_shn[i].port); nb[rp]=0; net_log(nb); }
        net_log(" w="); { char nb[16]; int rp=0; ucat(nb,&rp,(unsigned)g_shn[i].weight); nb[rp]=0; net_log(nb); net_log("\n"); }
    }
}

// 跑一次分片推理:layers 层模型切给发现的节点协同推理
void distnet_shard_run(int layers, int dim, uint32_t node_ip){
    if (layers <= 0) layers = 12;
    if (dim <= 0) dim = 16;
    g_sh_layers = layers; g_sh_dim = dim;
    g_sh_got_out = 0; g_sh_out[0] = 0;
    my_strcpy(g_sh_job, "k1");

    int n = dn_shard_discover(node_ip);
    if (n <= 0){ net_log("[DISTNET] shard: no shard nodes discovered\n"); return; }
    net_log("[DISTNET] shard: ");
    { char nb[16]; int rp=0; ucat(nb,&rp,(unsigned)n); nb[rp]=0; net_log(nb); }
    net_log(" nodes, layers=");
    { char nb[16]; int rp=0; ucat(nb,&rp,(unsigned)layers); nb[rp]=0; net_log(nb); net_log("\n"); }

    dn_shard_plan();

    // 下发分片:SHARD <job> <s> <e> <next_ip> <next_port> <last>
    for (int i = 0; i < DN_SHARD_MAX_NODES; i++){
        if (g_shn[i].weight <= 0) continue;
        struct dn_shnode* nx = 0;
        for (int j = i + 1; j < DN_SHARD_MAX_NODES; j++) if (g_shn[j].weight > 0){ nx = &g_shn[j]; break; }
        char msg[96]; int rp = 0;
        rcat(msg, &rp, "SHARD "); rcat(msg, &rp, g_sh_job); rcat(msg, &rp, " ");
        { char nb[16]; int q=0; ucat(nb,&q,(unsigned)g_shn[i].start); nb[q]=0; rcat(msg,&rp,nb); }
        rcat(msg, &rp, " ");
        { char nb[16]; int q=0; ucat(nb,&q,(unsigned)g_shn[i].end); nb[q]=0; rcat(msg,&rp,nb); }
        rcat(msg, &rp, " ");
        if (g_shn[i].last){ rcat(msg, &rp, "127.0.0.1 "); { char nb[16]; int q=0; ucat(nb,&q,(unsigned)DN_SHARD_PORT); nb[q]=0; rcat(msg,&rp,nb); } rcat(msg,&rp," 1"); }
        else if (nx){ rcat(msg, &rp, "127.0.0.1 "); { char nb[16]; int q=0; ucat(nb,&q,(unsigned)nx->port); nb[q]=0; rcat(msg,&rp,nb); } rcat(msg,&rp," 0"); }
        else { rcat(msg, &rp, "127.0.0.1 0 1"); }
        msg[rp] = 0;
        net_udp_send(g_shn[i].ip, DN_SHARD_PORT, g_shn[i].port, (const uint8_t*)msg, rp);
        net_log("[DISTNET] shard: -> "); log_ip(g_shn[i].ip);
        net_log(" L"); { char nb[16]; int q=0; ucat(nb,&q,(unsigned)g_shn[i].start); nb[q]=0; net_log(nb); }
        net_log("-");   { char nb[16]; int q=0; ucat(nb,&q,(unsigned)g_shn[i].end); nb[q]=0; net_log(nb); net_log("\n"); }
    }

    // 注入初始激活值:与宿主实现一致 vec[i] = 0.1*(i+1)
    char act[512]; int ap = 0;
    rcat(act, &ap, "AACT "); rcat(act, &ap, g_sh_job); rcat(act, &ap, " 0");
    for (int i = 0; i < g_sh_dim; i++){ rcat(act, &ap, " "); dn_put_tenths(act, &ap, i + 1); }
    act[ap] = 0;
    int first = -1;
    for (int i = 0; i < DN_SHARD_MAX_NODES; i++) if (g_shn[i].weight > 0){ first = i; break; }
    if (first < 0) return;
    net_udp_send(g_shn[first].ip, DN_SHARD_PORT, g_shn[first].port, (const uint8_t*)act, ap);

    // 等待最终结果(层计算在节点侧,内核只等 OUT)
    unsigned long guard = 0;
    while (!g_sh_got_out && guard++ < 6000000UL) net_poll();
    if (g_sh_got_out){ net_log("[DISTNET] shard inference OK: "); net_log(g_sh_out); net_log("\n"); }
    else net_log("[DISTNET] shard inference: timeout waiting for OUT\n");
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
        net_log("  distnet agent start [identity]     start self-managing agent\n");
        net_log("  distnet agent stop                 stop the agent\n");
        net_log("  distnet agent status               show identity/nodes/queue\n");
        net_log("  distnet agent add <ip> [role] [w]  register node (compute|ai|storage)\n");
        net_log("  distnet agent del <ip>             remove node\n");
        net_log("  distnet agent set <ip> weight|role <v>  reconfigure\n");
        net_log("  distnet agent run <type> <args>    queue a task\n");
        net_log("  distnet agent ai \"<prompt>\"        queue an AI task\n");
        net_log("  distnet agent handoff <ip>         transfer identity to a node\n");
        return;
    }
    // ---- distnet agent ... ----
    if (p[0]=='a' && p[1]=='g' && p[2]=='e' && p[3]=='n' && p[4]=='t'){
        const char* q = p + 5;
        while (*q==' '||*q=='\t') q++;
        // 取子命令
        char sub[16] = {0}; int sl = 0;
        while (*q && *q!=' ' && *q!='\t' && sl < 15) sub[sl++] = *q++;
        while (*q==' '||*q=='\t') q++;
        // 取后续参数(最多 3 个)
        char* tok[3]; int tl2[3];
        int nt = tokenize((char*)q, tok, tl2, 3);

        if (sub[0]=='s' && sub[1]=='t' && sub[2]=='a' && sub[3]=='r'){       // start [identity]
            dn_agent_start(nt > 0 ? tok[0] : NULL);
            return;
        }
        if (sub[0]=='s' && sub[1]=='t' && sub[2]=='o' && sub[3]=='p'){       // stop
            dn_agent_stop();
            return;
        }
        if (sub[0]=='s' && sub[1]=='t' && sub[2]=='a' && sub[3]=='t'){       // status
            dn_agent_status();
            return;
        }
        if (sub[0]=='a' && sub[1]=='d' && sub[2]=='d'){                      // add <ip> [role] [w]
            dn_agent_add(nt > 0 ? tok[0] : NULL,
                         nt > 1 ? tok[1] : NULL,
                         nt > 2 ? tok[2] : NULL);
            return;
        }
        if (sub[0]=='d' && sub[1]=='e' && sub[2]=='l'){                      // del <ip>
            dn_agent_del(nt > 0 ? tok[0] : NULL);
            return;
        }
        if (sub[0]=='s' && sub[1]=='e' && sub[2]=='t'){                      // set <ip> weight|role <v>
            if (nt < 3){ net_log("[AGENT] usage: set <ip> weight|role <v>\n"); return; }
            int idx = dn_agent_find(tok[0]);
            if (idx < 0){ net_log("[AGENT] node not found\n"); return; }
            if (tok[1][0]=='w'){
                int v = 0; const char* s = tok[2];
                while (*s >= '0' && *s <= '9'){ v = v * 10 + (*s - '0'); s++; }
                if (v > 0) g_agent_nodes[idx].weight = v;
            } else {
                g_agent_nodes[idx].role = dn_role_from_name(tok[2]);
            }
            dn_agent_status();
            return;
        }
        if (sub[0]=='r' && sub[1]=='u' && sub[2]=='n'){                      // run <type> <args>
            if (nt < 1){ net_log("[AGENT] usage: run <type> <args>\n"); return; }
            int id = dn_agent_enqueue(tok[0], nt > 1 ? tok[1] : NULL, 0);
            net_log(id >= 0 ? "[AGENT] queued\n" : "[AGENT] queue full\n");
            dn_agent_progress();
            return;
        }
        if (sub[0]=='a' && sub[1]=='i'){                                     // ai "<prompt>"
            if (nt < 1){ net_log("[AGENT] usage: ai \"<prompt>\"\n"); return; }
            int id = dn_agent_enqueue("ai", tok[0], 1);
            net_log(id >= 0 ? "[AGENT] queued ai task\n" : "[AGENT] queue full\n");
            dn_agent_progress();
            return;
        }
        if (sub[0]=='h' && sub[1]=='a' && sub[2]=='n'){                      // handoff <ip>
            if (nt < 1){ net_log("[AGENT] usage: handoff <ip>\n"); return; }
            g_agent_handoff_ip = parse_ip(tok[0]);
            net_log("[AGENT] pending handoff -> "); log_ip(g_agent_handoff_ip); net_log("\n");
            return;
        }
        net_log("[AGENT] unknown subcommand: "); net_log(sub); net_log("\n");
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
            // 拷贝目标 IP 到局部缓冲:原 q 指向的命令行缓冲在 net_poll 循环
            // 期间可能被覆盖,留指针会把 IP 读坏(如 10.0.2.2 变成 10.0.2.230)。
            static char targetbuf[16] = {0};
            int tl = 0;
            while (*q && *q!=' ' && *q!='\t' && tl < 15) targetbuf[tl++] = *q++;
            targetbuf[tl] = 0;
            target = targetbuf;
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
