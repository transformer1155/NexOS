/* =====================================================================
 *  plugin_loader.cpp  -  NexOS 内核态插件加载器实现
 * ---------------------------------------------------------------------
 *  约束（与 Makefile 的 CXX64FLAGS 一致）：
 *    -ffreestanding -fno-exceptions -fno-rtti -nostdlib
 *    不包含 <string.h>/<stdlib.h>/<stdio.h>（宿主自测模式除外）
 *
 *  所有内存都从内核堆 kmalloc(uint32_t) 取；宿主自测时替换为 malloc。
 *  这不是“可选优化”，而是内核里根本没有 libc。
 * ===================================================================== */
#include "plugin_loader.h"

/* <stdint.h> 不保证定义 NULL（freestanding 下尤其如此）。 */
#ifndef NULL
#  define NULL 0
#endif

#if defined(NEX_PL_HOST_TEST)
#  include <stdio.h>
#  include <stdlib.h>
#else
extern "C" void* kmalloc(uint32_t size);
extern "C" void  kfree(void* ptr);
extern "C" void  serial_puts(const char* s);
#endif

/* ------------------------------------------------------------ 平台层 */
static void* pl_alloc(uint32_t n)
{
#if defined(NEX_PL_HOST_TEST)
    return malloc((size_t)n);
#else
    return kmalloc(n);
#endif
}

static void pl_free(void* p)
{
    if (!p) return;
#if defined(NEX_PL_HOST_TEST)
    free(p);
#else
    kfree(p);
#endif
}

static void pl_emit(const char* s)
{
    if (!s) return;
#if defined(NEX_PL_HOST_TEST)
    fputs(s, stdout);
#else
    serial_puts(s);
#endif
}

#if defined(NEX_PL_HOST_TEST)
/* 内核侧只打印固定串；整数格式化只服务自测，避免把无用的分支带进 .text。 */
static void pl_emit_int(int v)
{
    char t[12];
    int  k = 12;
    int  neg = 0;
    if (v == 0) { pl_emit("0"); return; }
    if (v < 0) { neg = 1; v = -v; }
    while (v > 0 && k > 0) { t[--k] = (char)('0' + (v % 10)); v /= 10; }
    if (neg && k > 0) t[--k] = '-';
    char out[13];
    int  n = 12 - k;
    for (int i = 0; i < n; i++) out[i] = t[k + i];
    out[n] = 0;
    pl_emit(out);
}
#endif /* NEX_PL_HOST_TEST */

/* -------------------------------------------------- 手写字符串/内存原语 */
void* memset_c(void* dst, int v, uint32_t n)
{
    unsigned char* d = (unsigned char*)dst;
    if (!d) return dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (unsigned char)v;
    return dst;
}

char* strcpy_c(char* dst, const char* src)
{
    if (!dst) return dst;
    if (!src) { dst[0] = 0; return dst; }
    uint32_t i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}

char* strncpy_c(char* dst, const char* src, uint32_t cap)
{
    if (!dst || cap == 0) return dst;
    uint32_t i = 0;
    if (src) {
        while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    }
    dst[i] = 0;
    return dst;
}

int strcmp_c(const char* a, const char* b)
{
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

uint32_t strlen_c(const char* s)
{
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int str_ieq_c(const char* a, const char* b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0) ? 1 : 0;
}

int str_starts_c(const char* s, const char* prefix)
{
    if (!s || !prefix) return 0;
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

/* ------------------------------------------------------------ 全局状态 */
struct PlSlot {
    uint32_t       used;
    uint32_t       gen;
    int32_t        free_next;
    PluginContext* ctx;
};

static PlSlot          g_plugin_table[PL_MAX_PLUGINS];
static int32_t         g_free_head;
static volatile int    g_pl_lock;
static volatile int    g_live_count;
static pl_fs_read_fn   g_fs_read;
static pl_fs_write_fn  g_fs_write;
static pl_net_fn       g_net;

/* 内核里没有 pthread_mutex；单核 + 自旋锁足够。派发期间不持锁调用插件，
 * 因此插件回调里可以安全地再次进入 pl_lookup()/pl_count()。 */
static void pl_lock(void)
{
    while (__atomic_test_and_set(&g_pl_lock, __ATOMIC_ACQUIRE)) {
        __asm__ __volatile__("pause");
    }
}

static void pl_unlock(void)
{
    __atomic_clear(&g_pl_lock, __ATOMIC_RELEASE);
}

void pl_init(void)
{
    pl_lock();
    for (int i = 0; i < PL_MAX_PLUGINS; i++) {
        g_plugin_table[i].used      = 0;
        g_plugin_table[i].gen       = 0;
        g_plugin_table[i].ctx       = NULL;
        g_plugin_table[i].free_next = (i + 1 < PL_MAX_PLUGINS) ? (i + 1) : -1;
    }
    g_free_head  = 0;
    g_live_count = 0;
    pl_unlock();
}

void pl_set_fs_hooks(pl_fs_read_fn rd, pl_fs_write_fn wr)
{
    g_fs_read  = rd;
    g_fs_write = wr;
}

void pl_set_net_hook(pl_net_fn fn)
{
    g_net = fn;
}

/* ------------------------------------------------------------ 路径边界 */
/* 规范化规则：
 *   - 必须是绝对路径（以 '/' 开头）
 *   - 连续 '/' 折叠
 *   - "." 段丢弃；".." 段**直接拒绝**（不做“回退后再比较”，避免解析歧义）
 *   - '\\' 与控制字符拒绝
 *   - 结果不带结尾 '/'（根 "/" 除外）
 */
static int pl_norm_path(const char* in, char* out, uint32_t cap)
{
    if (!in || !out || cap < 2) return -1;
    if (in[0] != '/') return -1;

    uint32_t o = 0;
    out[o++] = '/';
    uint32_t i = 1;
    while (in[i]) {
        while (in[i] == '/') i++;
        if (!in[i]) break;

        uint32_t s = i;
        while (in[i] && in[i] != '/') {
            unsigned char c = (unsigned char)in[i];
            if (c < 0x20 || c == '\\') return -1;
            i++;
        }
        uint32_t len = i - s;

        if (len == 1 && in[s] == '.') continue;
        if (len == 2 && in[s] == '.' && in[s + 1] == '.') return -1;

        if (o > 1) {
            if (o + 1 >= cap) return -1;
            out[o++] = '/';
        }
        if (o + len >= cap) return -1;
        for (uint32_t k = 0; k < len; k++) out[o++] = in[s + k];
    }
    out[o] = 0;
    return 0;
}

int bnd_prefix_hit(const char* root, const char* path)
{
    char r[PL_MAX_PATH];
    char p[PL_MAX_PATH];
    if (pl_norm_path(root, r, sizeof(r)) != 0) return 0;
    if (pl_norm_path(path, p, sizeof(p)) != 0) return 0;

    /* 根 "/" 等于没有沙盒；加载器永远不会把 "/" 授予插件，这里也拒绝。 */
    if (strcmp_c(r, "/") == 0) return 0;

    uint32_t rl = strlen_c(r);
    if (!str_starts_c(p, r)) return 0;
    return (p[rl] == 0 || p[rl] == '/') ? 1 : 0;
}

/* ------------------------------------------------------------ JSON 读取 */
/* 最小 JSON 字符串读取器：只处理 "key": "value" 形式，足够解析工具参数。
 * 支持 \" \\ \/ \b \f \n \r \t 与 \uXXXX（BMP -> UTF-8）。
 * 遇到代理对时输出 U+FFFD（内核不需要完整 emoji 支持）。 */
static int pl_utf8_put(char* out, uint32_t cap, uint32_t* o, uint32_t cp)
{
    if (cp <= 0x7F) {
        if (*o + 1 >= cap) return -1;
        out[(*o)++] = (char)cp;
    } else if (cp <= 0x7FF) {
        if (*o + 2 >= cap) return -1;
        out[(*o)++] = (char)(0xC0 | (cp >> 6));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*o + 3 >= cap) return -1;
        out[(*o)++] = (char)(0xE0 | (cp >> 12));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    }
    return 0;
}

static int pl_hex4(const char* s, uint32_t* out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return -1;
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

/* 从 *pp 指向的 '"' 开始读一个 JSON 字符串；结束时 *pp 指向闭引号之后。 */
static int pl_json_read_string(const char** pp, char* out, uint32_t cap)
{
    const char* p = *pp;
    if (!p || *p != '"') return -1;
    p++;
    uint32_t o = 0;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            out[o] = 0;
            *pp = p + 1;
            return 0;
        }
        if (c == '\\') {
            p++;
            if (!*p) return -1;
            char e = *p++;
            char lit = 0;
            if (e == '"') lit = '"';
            else if (e == '\\') lit = '\\';
            else if (e == '/') lit = '/';
            else if (e == 'b') lit = '\b';
            else if (e == 'f') lit = '\f';
            else if (e == 'n') lit = '\n';
            else if (e == 'r') lit = '\r';
            else if (e == 't') lit = '\t';
            else if (e == 'u') {
                uint32_t cp = 0;
                if (pl_hex4(p, &cp) != 0) return -1;
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                if (pl_utf8_put(out, cap, &o, cp) != 0) return -1;
                continue;
            } else {
                return -1;
            }
            if (o + 1 >= cap) return -1;
            out[o++] = lit;
        } else {
            if (o + 1 >= cap) return -1;
            out[o++] = (char)c;
            p++;
        }
    }
    return -1;
}

/* 在任意深度的对象里找 key（扫描键位置，不会误匹配字符串值内部）。 */
static int pl_json_get_string(const char* json, const char* key,
                              char* out, uint32_t cap)
{
    if (!json || !key || !out || cap == 0) return -1;
    out[0] = 0;

    const char* p = json;
    char kbuf[64];
    char vbuf[PL_TMP_BUF];

    while (*p) {
        if (*p != '"') { p++; continue; }

        const char* save = p;
        if (pl_json_read_string(&p, kbuf, sizeof(kbuf)) != 0) {
            /* 非法字符串：跳过这个引号继续扫描，让上层最终报 BADARG。 */
            p = save + 1;
            continue;
        }
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != '"') continue;   /* 只取字符串值；数字/对象忽略 */

        if (pl_json_read_string(&p, vbuf, sizeof(vbuf)) != 0) return -1;
        if (strcmp_c(kbuf, key) == 0) {
            strncpy_c(out, vbuf, cap);
            return 0;
        }
    }
    return -1;
}

/* 依次尝试多个键名，命中即返回。 */
static int pl_json_get_first(const char* json, const char* const* keys,
                             int nkeys, char* out, uint32_t cap)
{
    for (int i = 0; i < nkeys; i++) {
        if (pl_json_get_string(json, keys[i], out, cap) == 0) return 0;
    }
    return -1;
}

/* ------------------------------------------------------------ JSON 输出 */
struct JW {
    char* buf;
    int   cap;
    int   n;
    int   fail;
};

static void jw_ch(JW* w, char c)
{
    if (w->n + 1 < w->cap) w->buf[w->n++] = c;
    else w->fail = 1;
}

static void jw_raw(JW* w, const char* s)
{
    if (!s) return;
    while (*s) jw_ch(w, *s++);
}

static void jw_uint(JW* w, int v)
{
    char t[12];
    int  k = 12;
    unsigned int u;
    if (v == 0) { jw_ch(w, '0'); return; }
    if (v < 0) { jw_ch(w, '-'); u = (unsigned int)(-v); }
    else u = (unsigned int)v;
    while (u > 0 && k > 0) { t[--k] = (char)('0' + (u % 10)); u /= 10; }
    while (k < 12) jw_ch(w, t[k++]);
}

static void jw_esc(JW* w, const char* s)
{
    if (!s) return;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c == '"') jw_raw(w, "\\\"");
        else if (c == '\\') jw_raw(w, "\\\\");
        else if (c == '\n') jw_raw(w, "\\n");
        else if (c == '\r') jw_raw(w, "\\r");
        else if (c == '\t') jw_raw(w, "\\t");
        else if (c < 0x20) jw_raw(w, "\\u0000");
        else jw_ch(w, (char)c);
    }
}

static void jw_end(JW* w)
{
    if (w->cap <= 0) return;
    if (w->n >= w->cap) w->n = w->cap - 1;
    w->buf[w->n] = 0;
}

static void jw_init(JW* w, char* buf, int cap)
{
    w->buf  = buf;
    w->cap  = cap;
    w->n    = 0;
    w->fail = 0;
    if (cap > 0) buf[0] = 0;
}

/* ------------------------------------------------------------ 工具能力表 */
static uint32_t pl_tool_cap(const char* tool)
{
    if (!tool) return 0;

    /* MCP 的 tools/call 名字必须是 ^[a-z][a-z0-9_]{1,63}$（见 docs/
     * MCP_PLUGIN_PROTOCOL.md），内核内部的历史写法是点分名。两种都认：
     * 总线传 "sys_info"，C 侧旧代码传 "sys.info"，映射到同一位。 */
    if (strcmp_c(tool, "fs.read")   == 0 || strcmp_c(tool, "fs_read")   == 0 ||
        strcmp_c(tool, "fs.list")   == 0 || strcmp_c(tool, "fs_list")   == 0 ||
        strcmp_c(tool, "fs.stat")   == 0 || strcmp_c(tool, "fs_stat")   == 0)
        return CAP_FS_READ;

    if (strcmp_c(tool, "fs.write")  == 0 || strcmp_c(tool, "fs_write")  == 0 ||
        strcmp_c(tool, "fs.delete") == 0 || strcmp_c(tool, "fs_delete") == 0 ||
        strcmp_c(tool, "fs.mkdir")  == 0 || strcmp_c(tool, "fs_mkdir")  == 0)
        return CAP_FS_WRITE;

    if (strcmp_c(tool, "net.http")  == 0 || strcmp_c(tool, "net_http")  == 0 ||
        strcmp_c(tool, "net.fetch") == 0 || strcmp_c(tool, "net_fetch") == 0 ||
        strcmp_c(tool, "net.dns")   == 0 || strcmp_c(tool, "net_dns")   == 0)
        return CAP_NET;

    if (strcmp_c(tool, "sys.info")  == 0 || strcmp_c(tool, "sys_info")  == 0 ||
        strcmp_c(tool, "sys.tasks") == 0 || strcmp_c(tool, "sys_tasks") == 0 ||
        strcmp_c(tool, "sys.mem")   == 0 || strcmp_c(tool, "sys_mem")   == 0)
        return CAP_SYSINFO;

    if (strcmp_c(tool, "ui.patch")  == 0 || strcmp_c(tool, "ui_patch")  == 0 ||
        strcmp_c(tool, "ui.toast")  == 0 || strcmp_c(tool, "ui_toast")  == 0)
        return CAP_UI;

    if (strcmp_c(tool, "ai.infer")  == 0 || strcmp_c(tool, "ai_infer")  == 0 ||
        strcmp_c(tool, "ai.embed")  == 0 || strcmp_c(tool, "ai_embed")  == 0)
        return CAP_AI;

    if (strcmp_c(tool, "dev.ioctl") == 0 || strcmp_c(tool, "dev_ioctl") == 0)
        return CAP_DEV;

    return 0;
}

static int pl_tool_is_fs(const char* tool)
{
    return str_starts_c(tool, "fs.") || str_starts_c(tool, "fs_");
}

static int pl_tool_is_net(const char* tool)
{
    return str_starts_c(tool, "net.") || str_starts_c(tool, "net_");
}

/* ------------------------------------------------------------ 域名白名单 */
static int pl_host_clean(const char* host, char* out, uint32_t cap)
{
    if (!host || !out || cap < 3) return -1;
    uint32_t o = 0;
    for (const char* p = host; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == '@' || c == ':' || c == '?' || c == '#' ||
            c == '[' || c == ']' || c == ' ' || c == '\t' ||
            c == '\r' || c == '\n') return -1;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (o + 1 >= cap) return -1;
        out[o++] = (char)c;
    }
    out[o] = 0;
    if (o < 3) return -1;
    if (out[0] == '.' || out[o - 1] == '.') return -1;
    return 0;
}

/* 从 "https://api.example.com:443/v1" 提取 "api.example.com"。
 * 只接受 http/https；其它 scheme 一律拒绝。 */
static int pl_url_host(const char* url, char* out, uint32_t cap)
{
    if (!url) return -1;
    const char* p = url;
    if (str_starts_c(p, "https://")) p += 8;
    else if (str_starts_c(p, "http://")) p += 7;
    else return -1;

    const char* end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;

    char auth[PL_MAX_HOST];
    uint32_t n = 0;
    for (const char* q = p; q < end; q++) {
        if (n + 1 >= sizeof(auth)) return -1;
        auth[n++] = *q;
    }
    auth[n] = 0;
    if (n == 0) return -1;

    /* 去掉端口（最后一个 ':' 之后全是数字才接受）。 */
    char* colon = NULL;
    for (char* q = auth; *q; q++) if (*q == ':') colon = q;
    if (colon) {
        if (colon == auth) return -1;
        int digits = 0;
        for (char* q = colon + 1; *q; q++) {
            if (*q < '0' || *q > '9') return -1;
            digits++;
        }
        if (digits == 0 || digits > 5) return -1;
        *colon = 0;
    }
    return pl_host_clean(auth, out, cap);
}

int pl_check_host(const PluginContext* ctx, const char* host)
{
    if (!ctx || !ctx->manifest || !host) return 0;
    const char* list = ctx->manifest->allow_hosts;
    if (!list || !list[0]) return 0;

    char h[PL_MAX_HOST];
    if (pl_host_clean(host, h, sizeof(h)) != 0) return 0;

    const char* p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        const char* s = p;
        while (*p && *p != ',') p++;
        const char* e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;

        char pat[PL_MAX_HOST];
        uint32_t n = 0;
        for (const char* q = s; q < e; q++) {
            if (n + 1 >= sizeof(pat)) { n = 0; break; }
            unsigned char c = (unsigned char)*q;
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
            pat[n++] = (char)c;
        }
        pat[n] = 0;
        if (n == 0) continue;

        if (pat[0] == '*' && pat[1] == '.') {
            /* *.example.com 只匹配至少一级子域，不匹配 example.com 本身。 */
            const char* suffix = pat + 1;          /* ".example.com" */
            uint32_t hl = strlen_c(h);
            uint32_t sl = strlen_c(suffix);
            if (hl > sl && strcmp_c(h + (hl - sl), suffix) == 0) return 1;
        } else {
            if (str_ieq_c(h, pat)) return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ 槽位管理 */
static PluginContext* pl_lookup_locked(uint32_t h)
{
    if (h == 0) return NULL;
    uint32_t idx = (h & 0xFFu) - 1u;
    if (idx >= PL_MAX_PLUGINS) return NULL;
    PlSlot* s = &g_plugin_table[idx];
    if (!s->used || !s->ctx) return NULL;
    if (s->gen != (h >> 8)) return NULL;
    return s->ctx;
}

PluginContext* pl_lookup(uint32_t h)
{
    pl_lock();
    PluginContext* c = pl_lookup_locked(h);
    pl_unlock();
    return c;
}

int pl_count(void)
{
    pl_lock();
    int n = (int)g_live_count;
    pl_unlock();
    return n;
}

int pl_list(uint32_t* out, int cap)
{
    if (!out || cap <= 0) return 0;
    int n = 0;
    pl_lock();
    for (int i = 0; i < PL_MAX_PLUGINS && n < cap; i++) {
        if (g_plugin_table[i].used && g_plugin_table[i].ctx)
            out[n++] = g_plugin_table[i].ctx->h_t;
    }
    pl_unlock();
    return n;
}

/* ------------------------------------------------------------ id 校验 */
static int pl_id_ok(const char* id)
{
    if (!id) return 0;
    uint32_t n = strlen_c(id);
    if (n < 2 || n >= PL_MAX_ID) return 0;
    char c = id[0];
    if (c < 'a' || c > 'z') return 0;
    for (uint32_t i = 1; i < n; i++) {
        c = id[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '.' || c == '_' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------ 加载/卸载 */
int pl_load_plugin(const PluginManifest* m, int is_signed, uint32_t* out_handle)
{
    if (out_handle) *out_handle = 0;
    if (!m || !out_handle) return PL_ERR_BADARG;
    if (!pl_id_ok(m->id)) return PL_ERR_BADARG;
    if (!m->entry_point && !m->call_point) return PL_ERR_BADARG;
    if (m->capabilities & ~CAP_KNOWN) return PL_ERR_CAP;
    if (m->capabilities & CAP_IPC) return PL_ERR_CAP;   /* 单进程零 IPC */

    uint32_t abi = m->abi_version ? m->abi_version : 1u;
    if (abi > 1u) return PL_ERR_STATE;

    PluginContext* c = (PluginContext*)pl_alloc((uint32_t)sizeof(PluginContext));
    if (!c) return PL_ERR_NOSPACE;
    memset_c(c, 0, (uint32_t)sizeof(PluginContext));

    uint32_t caps = m->capabilities;
    if (!is_signed) {
        /* 铁律：未签名插件永远不能写文件系统；根目录也强制改到
         * /sandbox/<plugin_id>，manifest 里写什么都不算数。 */
        caps &= ~CAP_FS_WRITE;
    }

    strncpy_c(c->id, m->id, sizeof(c->id));

    /* 签名插件可以申请自己的根，但必须仍在 /sandbox/ 之下。
     * 否则（未签名 / 非法根）一律回落到 /sandbox/<id>。 */
    char fallback[PL_MAX_ROOT];
    strcpy_c(fallback, "/sandbox/");
    strncpy_c(fallback + 9, m->id, sizeof(fallback) - 9);

    int root_ok = 0;
    if (is_signed && m->sandbox_root &&
        str_starts_c(m->sandbox_root, "/sandbox/") &&
        bnd_prefix_hit("/sandbox", m->sandbox_root)) {
        char norm[PL_MAX_PATH];
        if (pl_norm_path(m->sandbox_root, norm, sizeof(norm)) == 0 &&
            strlen_c(norm) < PL_MAX_ROOT) {
            strcpy_c(c->sandbox_root, norm);
            root_ok = 1;
        }
    }
    if (!root_ok) strcpy_c(c->sandbox_root, fallback);

    c->caps        = caps;
    c->abi_version = abi;
    c->is_signed   = (uint8_t)(is_signed ? 1 : 0);
    c->manifest    = m;
    c->state       = 2;
    c->busy        = 0;
    c->call_count  = 0;

    pl_lock();
    if (g_free_head < 0) {
        pl_unlock();
        memset_c(c, 0, (uint32_t)sizeof(PluginContext));
        pl_free(c);
        return PL_ERR_NOSPACE;
    }

    int32_t idx  = g_free_head;
    PlSlot* slot = &g_plugin_table[idx];
    g_free_head  = slot->free_next;

    uint32_t gen = slot->gen + 1u;
    gen &= 0x00FFFFFFu;
    if (gen == 0) gen = 1;                 /* 24 位回绕时跳过 0 */

    slot->used      = 1;
    slot->gen       = gen;
    slot->free_next = -1;
    slot->ctx       = c;

    uint32_t h = (gen << 8) | ((uint32_t)idx + 1u);
    c->h_t = h;
    g_live_count++;
    pl_unlock();

    if (m->entry_point) {
        char tmp[64];
        int rc = m->entry_point(h, m->ui_layout ? m->ui_layout : "", tmp, (int)sizeof(tmp));
        if (rc < 0) {
            pl_unload(h);
            return rc;
        }
    }

    *out_handle = h;
    return PL_OK;
}

int pl_unload(uint32_t h)
{
    pl_lock();
    PluginContext* c = pl_lookup_locked(h);
    if (!c) {
        pl_unlock();
        return PL_ERR_STALE;
    }
    if (c->busy > 0) {
        pl_unlock();
        return PL_ERR_STATE;
    }

    uint32_t idx = (h & 0xFFu) - 1u;
    PlSlot* s = &g_plugin_table[idx];

    c->state = 0;
    c->h_t   = 0;
    s->used  = 0;
    s->ctx   = NULL;
    s->free_next = g_free_head;
    g_free_head  = (int32_t)idx;
    if (g_live_count > 0) g_live_count--;
    pl_unlock();

    memset_c(c, 0, (uint32_t)sizeof(PluginContext));
    pl_free(c);
    return PL_OK;
}

/* ------------------------------------------------------------ MCP 封装 */
static int pl_rpc_code(int rc)
{
    switch (rc) {
        case PL_ERR_BADARG:
        case PL_ERR_OVERFLOW: return -32602;   /* Invalid params  */
        case PL_ERR_DENIED:
        case PL_ERR_SANDBOX:
        case PL_ERR_NET:
        case PL_ERR_CAP:      return -32001;   /* Policy denied   */
        case PL_ERR_NOENT:    return -32004;   /* Not found       */
        case PL_ERR_STALE:    return -32002;   /* Stale handle    */
        default:              return -32603;   /* Internal error  */
    }
}

static int pl_write_error(char* out, int outcap, int rc,
                          const char* plugin, const char* tool, const char* path)
{
    JW w;
    jw_init(&w, out, outcap);
    jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":");
    jw_uint(&w, pl_rpc_code(rc));
    jw_raw(&w, ",\"message\":\"");
    jw_esc(&w, pl_strerror(rc));
    jw_raw(&w, "\",\"data\":{\"errno\":");
    jw_uint(&w, rc);
    jw_raw(&w, ",\"plugin\":\"");
    jw_esc(&w, plugin);
    jw_raw(&w, "\",\"tool\":\"");
    jw_esc(&w, tool);
    if (path && path[0]) {
        jw_raw(&w, "\",\"path\":\"");
        jw_esc(&w, path);
    }
    jw_raw(&w, "\"}}}");
    jw_end(&w);
    return w.fail ? PL_ERR_OVERFLOW : rc;
}

static int pl_write_result(char* out, int outcap,
                           const char* structured_or_null,
                           const char* text,
                           int is_error)
{
    JW w;
    jw_init(&w, out, outcap);
    jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":null,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"");
    jw_esc(&w, text ? text : "ok");
    jw_raw(&w, "\"}],\"structuredContent\":");
    if (structured_or_null && structured_or_null[0])
        jw_raw(&w, structured_or_null);
    else
        jw_raw(&w, "null");
    jw_raw(&w, ",\"isError\":");
    jw_raw(&w, is_error ? "true" : "false");
    jw_raw(&w, "}}");
    jw_end(&w);
    return w.fail ? PL_ERR_OVERFLOW : PL_OK;
}

/* 无 handler 时的内核原生执行（fs.write / net.http）。
 * 成功时把 JSON 结果写进 result_out。 */
static int pl_exec_native(const PluginContext* c, const char* tool,
                          const char* args, char* result_out, int resultcap)
{
    if (!c || !tool || !args) return PL_ERR_BADARG;

    if (strcmp_c(tool, "fs.write") == 0 || strcmp_c(tool, "fs_write") == 0) {
        if (!g_fs_write) return PL_ERR_NOENT;

        char path[PL_MAX_PATH];
        if (pl_json_get_string(args, "path", path, sizeof(path)) != 0)
            return PL_ERR_BADARG;

        char data[PL_TMP_BUF];
        if (pl_json_get_string(args, "data", data, sizeof(data)) != 0) {
            /* 允许空内容写入：data 缺省视为 ""。 */
            data[0] = 0;
        }
        int rc = g_fs_write(c->h_t, path, (const uint8_t*)data,
                            strlen_c(data));
        if (rc < 0) return rc;
        strncpy_c(result_out, "{\"written\":true}", (uint32_t)resultcap);
        return PL_OK;
    }

    if (strcmp_c(tool, "net.http") == 0 || strcmp_c(tool, "net_http") == 0 ||
        strcmp_c(tool, "net.fetch") == 0 || strcmp_c(tool, "net_fetch") == 0) {
        if (!g_net) return PL_ERR_NOENT;

        char url[PL_MAX_PATH];
        if (pl_json_get_string(args, "url", url, sizeof(url)) != 0)
            return PL_ERR_BADARG;

        char host[PL_MAX_HOST];
        if (pl_url_host(url, host, sizeof(host)) != 0) return PL_ERR_NET;
        if (!pl_check_host(c, host)) return PL_ERR_NET;

        const char* after = url;
        if (str_starts_c(after, "https://")) after += 8;
        else after += 7;
        while (*after && *after != '/' && *after != '?' && *after != '#') after++;
        const char* pth = (*after == 0) ? "/" : after;

        char body[PL_TMP_BUF];
        if (pl_json_get_string(args, "body", body, sizeof(body)) != 0)
            body[0] = 0;

        return g_net(c->h_t, "POST", host, pth, body, result_out, resultcap);
    }

    return PL_ERR_NOENT;
}

/* ------------------------------------------------------------ 派发实现 */
static int pl_dispatch_locked_ctx(PluginContext* c, const char* tool,
                                  const char* args_json,
                                  char* out, int outcap)
{
    if (!c || !tool || !out || outcap < 8) return PL_ERR_BADARG;

    uint32_t need = pl_tool_cap(tool);
    if (need == 0)
        return pl_write_error(out, outcap, PL_ERR_NOENT, c->id, tool, NULL);

    if ((c->caps & need) != need)
        return pl_write_error(out, outcap, PL_ERR_DENIED, c->id, tool, NULL);

    char path[PL_MAX_PATH];
    path[0] = 0;

    if (pl_tool_is_fs(tool)) {
        static const char* const keys[] = { "path", "file", "dest", "target", "src" };
        if (pl_json_get_first(args_json, keys, 5, path, sizeof(path)) != 0)
            return pl_write_error(out, outcap, PL_ERR_BADARG, c->id, tool, NULL);

        /* 关键防线：无论插件把 path 写成 /etc/passwd、../../x 还是
         * //sandbox/../etc，bnd_prefix_hit 都只认规范化后仍在根内的路径。 */
        if (!bnd_prefix_hit(c->sandbox_root, path))
            return pl_write_error(out, outcap, PL_ERR_SANDBOX, c->id, tool, path);
    }

    if (pl_tool_is_net(tool)) {
        char target[PL_MAX_PATH];
        if (pl_json_get_string(args_json, "url", target, sizeof(target)) == 0) {
            char host[PL_MAX_HOST];
            if (pl_url_host(target, host, sizeof(host)) != 0)
                return pl_write_error(out, outcap, PL_ERR_NET, c->id, tool, target);
            if (!pl_check_host(c, host))
                return pl_write_error(out, outcap, PL_ERR_NET, c->id, tool, target);
        } else if (pl_json_get_string(args_json, "host", target, sizeof(target)) == 0) {
            if (!pl_check_host(c, target))
                return pl_write_error(out, outcap, PL_ERR_NET, c->id, tool, target);
        } else {
            return pl_write_error(out, outcap, PL_ERR_BADARG, c->id, tool, NULL);
        }
    }

    /* 有插件 handler：策略通过后交给它；没有则走内核原生钩子。 */
    char tmp[PL_TMP_BUF];
    tmp[0] = 0;
    int rc;

    if (c->manifest && c->manifest->call_point) {
        rc = c->manifest->call_point(c->h_t, tool,
                                     args_json ? args_json : "{}",
                                     tmp, (int)sizeof(tmp));
    } else {
        rc = pl_exec_native(c, tool, args_json ? args_json : "{}",
                            tmp, (int)sizeof(tmp));
        if (rc >= 0) {
            /* 原生钩子可能只写了一个裸字符串；不是 JSON 时退化为 text。 */
            if (tmp[0] == 0) strncpy_c(tmp, "null", sizeof(tmp));
        }
    }
    c->call_count++;

    if (rc < 0)
        return pl_write_error(out, outcap, rc, c->id, tool,
                              path[0] ? path : NULL);

    return pl_write_result(out, outcap, tmp, "ok", 0);
}

int mcp_dispatch(uint32_t h, const char* tool, const char* args_json,
                 char* out, int outcap)
{
    if (!out || outcap < 8) return PL_ERR_BADARG;
    out[0] = 0;

    pl_lock();
    PluginContext* c = pl_lookup_locked(h);
    if (!c) {
        pl_unlock();
        return pl_write_error(out, outcap, PL_ERR_STALE, "", tool, NULL);
    }
    c->busy++;
    pl_unlock();

    int rc = pl_dispatch_locked_ctx(c, tool, args_json, out, outcap);

    pl_lock();
    c->busy--;
    pl_unlock();
    return rc;
}

/* ------------------------------------------------------------ 错误文本 */
const char* pl_strerror(int code)
{
    switch (code) {
        case PL_OK:            return "ok";
        case PL_ERR_NOSPACE:   return "no free plugin slot";
        case PL_ERR_BADARG:    return "invalid argument";
        case PL_ERR_NOENT:     return "plugin or tool not found";
        case PL_ERR_STALE:     return "stale plugin handle";
        case PL_ERR_DENIED:    return "capability denied";
        case PL_ERR_OVERFLOW:  return "output buffer too small";
        case PL_ERR_CAP:       return "invalid capability request";
        case PL_ERR_SANDBOX:   return "path escapes sandbox root";
        case PL_ERR_STATE:     return "plugin busy or abi mismatch";
        case PL_ERR_NET:       return "host not in allow_hosts";
        default:               return "unknown plugin error";
    }
}

/* =====================================================================
 *  run_selftest()
 * ---------------------------------------------------------------------
 *  故意做成“内核可调用、宿主也可运行”：
 *    make plugin-loader-selftest   -> g++ -DNEX_PL_HOST_TEST
 *  内核里可以在 kmain64 的插件初始化之后调用一次，输出走 COM1。
 * ===================================================================== */
#if defined(NEX_PL_HOST_TEST)
/* 自测用假文件系统：只记录最后一次写入，证明策略通过后确实能落盘。 */
static char g_test_last_path[PL_MAX_PATH];
static char g_test_last_data[256];

static int st_fs_read(uint32_t h, const char* path, uint8_t* buf,
                      uint32_t cap, uint32_t* out_len)
{
    (void)h; (void)path; (void)buf; (void)cap;
    if (out_len) *out_len = 0;
    return 0;
}

static int st_fs_write(uint32_t h, const char* path,
                       const uint8_t* data, uint32_t len)
{
    (void)h;
    strncpy_c(g_test_last_path, path, sizeof(g_test_last_path));
    if (len > sizeof(g_test_last_data) - 1) len = sizeof(g_test_last_data) - 1;
    for (uint32_t i = 0; i < len; i++) g_test_last_data[i] = (char)data[i];
    g_test_last_data[len] = 0;
    return 0;
}

static int st_net(uint32_t h, const char* method, const char* host,
                  const char* path, const char* body,
                  char* out, int outcap)
{
    (void)h; (void)method; (void)host; (void)path; (void)body;
    strncpy_c(out, "{\"status\":200}", (uint32_t)outcap);
    return 0;
}

/* 报表计数 */
static int g_st_pass;
static int g_st_fail;

static void st_check(int cond, const char* name)
{
    if (cond) { g_st_pass++; pl_emit("[SELFTEST] PASS  "); }
    else      { g_st_fail++; pl_emit("[SELFTEST] FAIL  "); }
    pl_emit(name);
    pl_emit("\n");
}

/* 插件 handler：证明“策略通过后 handler 真的被调用”。 */
static int st_handler(uint32_t h, const char* tool, const char* args,
                      char* out, int outcap)
{
    (void)h; (void)tool; (void)args;
    strncpy_c(out, "{\"ok\":true}", (uint32_t)outcap);
    return 0;
}

static int st_entry(uint32_t h, const char* args, char* out, int outcap)
{
    (void)h; (void)args;
    strncpy_c(out, "ready", (uint32_t)outcap);
    return 0;
}

int run_selftest(void)
{
    g_st_pass = 0;
    g_st_fail = 0;

    pl_init();
    pl_set_fs_hooks(st_fs_read, st_fs_write);
    pl_set_net_hook(st_net);

    pl_emit("\n===== NexOS plugin_loader selftest =====\n");

    /* ---------- 用例 1：未签名插件越权被拦截 ---------- */
    PluginManifest m;
    memset_c(&m, 0, sizeof(m));
    m.id             = "sysinfo";
    m.name           = "System Info (unsigned)";
    m.version        = "1.0.0";
    m.sandbox_root   = "/";                 /* 恶意/错误的申请，必须被忽略 */
    m.capabilities   = CAP_FS_READ | CAP_FS_WRITE | CAP_NET | CAP_SYSINFO;
    m.allow_hosts    = "api.example.com,*.trusted.dev";
    m.ui_layout      = "ui/layout.json";
    m.entry_point    = st_entry;
    m.call_point     = st_handler;
    m.abi_version    = 1;

    uint32_t h1 = 0;
    int rc = pl_load_plugin(&m, 0 /* unsigned */, &h1);
    st_check(rc == PL_OK && h1 != 0, "unsigned plugin loads");

    PluginContext* c1 = pl_lookup(h1);
    st_check(c1 != NULL, "handle lookup is O(1) and resolves");
    st_check(c1 && (c1->caps & CAP_FS_WRITE) == 0,
             "unsigned plugin: CAP_FS_WRITE stripped");
    st_check(c1 && strcmp_c(c1->sandbox_root, "/sandbox/sysinfo") == 0,
             "unsigned plugin: sandbox_root forced to /sandbox/<id>");

    char out[PL_MAX_OUT];
    rc = mcp_dispatch(h1, "fs.write",
                      "{\"path\":\"/etc/passwd\",\"data\":\"root:x\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_ERR_DENIED,
             "unsigned plugin: fs.write DENIED (CAP_FS_WRITE stripped)");

    /* 要求的原文输出（UTF-8 到 COM1/stdout，纯字节，不依赖字体） */
    pl_emit("[SELFTEST] 未签名插件越权被拦截: fs.write /etc/passwd -> ");
    pl_emit_int(rc);
    pl_emit(" (");
    pl_emit(pl_strerror(rc));
    pl_emit(")\n");

    /* 路径边界本身也必须独立生效：未签名插件仍保留 CAP_FS_READ，
     * 读系统文件必须被 bnd_prefix_hit 拒绝（证明不是只靠能力位）。 */
    rc = mcp_dispatch(h1, "fs.read",
                      "{\"path\":\"/etc/passwd\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_ERR_SANDBOX, "unsigned plugin: fs.read /etc/passwd DENIED by sandbox");

    rc = mcp_dispatch(h1, "fs.read",
                      "{\"path\":\"/sandbox/sysinfo/notes.txt\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_OK, "unsigned plugin: read inside sandbox ALLOWED");

    /* 未签名插件没有 CAP_FS_WRITE，沙盒内的写也必须拒绝。 */
    rc = mcp_dispatch(h1, "fs.write",
                      "{\"path\":\"/sandbox/sysinfo/cache.txt\",\"data\":\"ok\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_ERR_DENIED,
             "unsigned plugin: write inside sandbox still DENIED (no cap)");

    /* 各种绕过手法都应失败。 */
    rc = mcp_dispatch(h1, "fs.read",
                      "{\"path\":\"/sandbox/sysinfo/../../etc/shadow\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_ERR_SANDBOX, "traversal ../../etc/shadow DENIED");

    rc = mcp_dispatch(h1, "fs.read",
                      "{\"path\":\"/sandbox/sysinfo2/x\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_ERR_SANDBOX, "sibling prefix /sandbox/sysinfo2 DENIED");

    rc = mcp_dispatch(h1, "fs.read",
                      "{\"path\":\"/sandbox/sysinfo\\\\..\\\\etc\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_ERR_SANDBOX, "backslash traversal DENIED");

    /* 未签名插件：网络白名单仍然生效。 */
    rc = mcp_dispatch(h1, "net.http",
                      "{\"url\":\"https://evil.example/x\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_ERR_NET, "net.http to non-allowlisted host DENIED");

    rc = mcp_dispatch(h1, "net.http",
                      "{\"url\":\"https://api.example.com:443/v1\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_OK, "net.http to allowlisted host ALLOWED");

    rc = mcp_dispatch(h1, "net.http",
                      "{\"url\":\"https://x.trusted.dev/a\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_OK, "wildcard *.trusted.dev matches subdomain");

    rc = mcp_dispatch(h1, "net.http",
                      "{\"url\":\"https://trusted.dev/a\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_ERR_NET, "wildcard does not match apex domain");

    rc = mcp_dispatch(h1, "net.http",
                      "{\"url\":\"https://api.example.com.evil.test/a\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_ERR_NET, "suffix confusion api.example.com.evil DENIED");

    /* 未签名插件不申请 FS_WRITE 时，fs.write 直接 DENIED。 */
    rc = mcp_dispatch(h1, "sys.info", "{}", out, (int)sizeof(out));
    st_check(rc == PL_OK, "sys.info with CAP_SYSINFO ALLOWED");

    /* ---------- 用例 2：卸载后旧句柄悬空报错 ---------- */
    uint32_t h_old = h1;
    rc = pl_unload(h1);
    st_check(rc == PL_OK, "unload returns OK");

    st_check(pl_lookup(h_old) == NULL, "old handle no longer resolves");
    rc = mcp_dispatch(h_old, "sys.info", "{}", out, (int)sizeof(out));
    st_check(rc == PL_ERR_STALE, "old handle rejected as STALE");

    pl_emit("[SELFTEST] 卸载后旧句柄悬空报错: h=0x");
    {
        /* 十六进制打印，避免依赖 printf("%x")。 */
        char hex[9];
        for (int i = 0; i < 8; i++) {
            uint32_t nib = (h_old >> ((7 - i) * 4)) & 0xFu;
            hex[i] = (char)(nib < 10 ? ('0' + nib) : ('a' + (nib - 10)));
        }
        hex[8] = 0;
        pl_emit(hex);
    }
    pl_emit(" -> ");
    pl_emit_int(rc);
    pl_emit(" (");
    pl_emit(pl_strerror(rc));
    pl_emit(")\n");

    /* ---------- 句柄代际：复用槽位也必须换 handle ---------- */
    uint32_t h2 = 0, h3 = 0;
    rc = pl_load_plugin(&m, 1, &h2);
    st_check(rc == PL_OK && h2 != 0 && h2 != h_old,
             "slot reuse bumps generation (no ABA)");

    rc = mcp_dispatch(h_old, "sys.info", "{}", out, (int)sizeof(out));
    st_check(rc == PL_ERR_STALE, "old handle cannot alias the new plugin");

    /* ---------- 签名插件：可写，但根仍必须合法 ---------- */
    PluginManifest ms;
    memset_c(&ms, 0, sizeof(ms));
    ms.id           = "signedapp";
    ms.name         = "Signed App";
    ms.version      = "1.0.0";
    ms.sandbox_root = "/sandbox/signedapp";
    ms.capabilities = CAP_FS_WRITE;
    ms.entry_point  = st_entry;
    ms.call_point   = st_handler;
    ms.abi_version  = 1;

    rc = pl_load_plugin(&ms, 1, &h3);
    st_check(rc == PL_OK, "signed plugin loads");

    PluginContext* c3 = pl_lookup(h3);
    st_check(c3 && (c3->caps & CAP_FS_WRITE) != 0,
             "signed plugin keeps CAP_FS_WRITE");

    rc = mcp_dispatch(h3, "fs.write",
                      "{\"path\":\"/sandbox/signedapp/a.txt\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_OK, "signed plugin writes inside its root");

    rc = mcp_dispatch(h3, "fs.write",
                      "{\"path\":\"/sandbox/other/a.txt\"}",
                      out, (int)sizeof(out));
    st_check(rc == PL_ERR_SANDBOX, "signed plugin cannot escape its root");

    /* 非法根（请求 /）必须回落到 /sandbox/<id>。 */
    ms.sandbox_root = "/";
    uint32_t h4 = 0;
    rc = pl_load_plugin(&ms, 1, &h4);
    PluginContext* c4 = pl_lookup(h4);
    st_check(rc == PL_OK && c4 &&
             strcmp_c(c4->sandbox_root, "/sandbox/signedapp") == 0,
             "illegal root '/' falls back to /sandbox/<id>");

    /* ---------- 能力位：IPC 必须被拒绝 ---------- */
    ms.capabilities = CAP_IPC;
    uint32_t h5 = 0;
    rc = pl_load_plugin(&ms, 1, &h5);
    st_check(rc == PL_ERR_CAP, "CAP_IPC rejected (single-process, zero IPC)");

    /* ---------- 容量：第 129 个必须 NOSPACE ---------- */
    pl_unload(h2);
    pl_unload(h3);
    pl_unload(h4);
    pl_init();

    PluginManifest mc;
    memset_c(&mc, 0, sizeof(mc));
    mc.name         = "capacity";
    mc.version      = "1.0.0";
    mc.capabilities = CAP_SYSINFO;
    mc.entry_point  = NULL;
    mc.call_point   = st_handler;
    mc.abi_version  = 1;

    char cap_id[PL_MAX_ID];
    for (int i = 0; i < PL_MAX_ID; i++) cap_id[i] = 0;
    strcpy_c(cap_id, "p0");
    mc.id = cap_id;

    int loaded = 0;
    for (int i = 0; i < PL_MAX_PLUGINS; i++) {
        cap_id[0] = 'p';
        cap_id[1] = (char)('0' + (i / 100) % 10);
        cap_id[2] = (char)('0' + (i / 10) % 10);
        cap_id[3] = (char)('0' + (i % 10));
        cap_id[4] = 0;
        uint32_t hh = 0;
        if (pl_load_plugin(&mc, 0, &hh) == PL_OK) loaded++;
    }
    st_check(loaded == PL_MAX_PLUGINS, "128 concurrent plugins fit");

    uint32_t overflow_h = 0;
    rc = pl_load_plugin(&mc, 0, &overflow_h);
    st_check(rc == PL_ERR_NOSPACE, "129th plugin returns PL_ERR_NOSPACE");
    st_check(pl_count() == PL_MAX_PLUGINS, "registry still holds 128 live");

    /* ---------- 清理 ---------- */
    uint32_t all[PL_MAX_PLUGINS];
    int n = pl_list(all, PL_MAX_PLUGINS);
    st_check(n == PL_MAX_PLUGINS, "pl_list exports every live handle");
    for (int i = 0; i < n; i++) pl_unload(all[i]);
    st_check(pl_count() == 0, "all plugins unloaded");

    pl_emit("===== selftest done: ");
    pl_emit_int(g_st_pass);
    pl_emit(" passed, ");
    pl_emit_int(g_st_fail);
    pl_emit(" failed =====\n\n");
    return g_st_fail;
}
#else
/* 内核构建：不把 selftest 的假 FS / 字符串表编进 .text。
 * 需要自测时用宿主目标 `make plugin-loader-selftest`。 */
int run_selftest(void)
{
    pl_emit("[PLUGIN] run_selftest() is host-only; build with "
            "-DNEX_PL_HOST_TEST\n");
    return 0;
}
#endif
