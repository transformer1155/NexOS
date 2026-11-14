/* =====================================================================
 *  plugin_core.cpp  -  NexOS 微内核级插件核心实现
 * ---------------------------------------------------------------------
 *  编译约束（与 Makefile 的 CXX64FLAGS 一致）：
 *      -ffreestanding -fno-exceptions -fno-rtti -nostdlib
 *  本文件不包含任何 C/C++ 标准库头文件（宿主自测的 pthread 除外，
 *  且只出现在 run_core_selftest 内部）。
 *
 *  设计要点
 *  -------------------------------------------------------------------
 *   [1] 自旋锁：__atomic CAS + pause；内核态另有 cli/sti 版本。
 *   [2] 安全拷贝：先查空指针/容量，再决定是否写；用户指针必须先过
 *       is_valid_user_ptr()，保证坏指针在进入拷贝循环前就被拒绝。
 *   [3] 影子页表：每个插件一份 PagePermission[]；未签名插件硬重置为
 *       唯一一段 sandbox 读写页，EXEC / 额外页一律丢弃。
 *   [4] 路由：FNV-1a + 开放寻址线性探测；删除用 Knuth 回移，不留墓碑；
 *       每个插件维护一条路由链，unload 时按句柄精确注销。
 *   [5] 生命周期：位图分配槽位 O(1)，句柄带 24 位代际；unload Gen++，
 *       旧句柄立即 PC_ERR_STALE；inflight 计数实现并发配额。
 *   [6] 所有失败路径统一走 pc_fail()，一定调用 osapi_set_last_error()。
 * ===================================================================== */
#include "plugin_core.h"
#include "osapi.h"

#ifndef NULL
#  define NULL 0
#endif

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

/* 串口输出：内核里由 serial_puts 提供；宿主自测用 hook 注入。
 * 这里声明成 weak，宿主没有强符号时链接为 0，不会拖进 libc。 */
#if defined(__GNUC__) || defined(__clang__)
extern "C" void serial_puts(const char* s) __attribute__((weak));
#endif

static void (*g_emit_hook)(const char* s) = NULL;

void pc_set_emit_hook(void (*fn)(const char* s))
{
    g_emit_hook = fn;
}

static void pc_emit(const char* s)
{
    if (!s) return;
    if (g_emit_hook) { g_emit_hook(s); return; }
#if defined(__GNUC__) || defined(__clang__)
    if (serial_puts) serial_puts(s);
#endif
}

/* =====================================================================
 *  错误通道
 * ===================================================================== */
static int pc_fail(int rc, int os_err)
{
    osapi_set_last_error(os_err);
    return rc;
}

static int pc_ok(void)
{
    osapi_set_last_error(ERR_OK);
    return PC_OK;
}

static int pc_os_from(int rc)
{
    switch (rc) {
        case PC_OK:          return ERR_OK;
        case PC_ERR_NOSPACE: return ERR_NOSPACE;
        case PC_ERR_BADARG:  return ERR_BADARG;
        case PC_ERR_NOENT:   return ERR_NOTFOUND;
        case PC_ERR_STALE:   return ERR_STALE;
        case PC_ERR_DENIED:  return ERR_PERMDENIED;
        case PC_ERR_OVERFLOW:return ERR_TOOBIG;
        case PC_ERR_CAP:     return ERR_PERMDENIED;
        case PC_ERR_SANDBOX: return ERR_PERMDENIED;
        case PC_ERR_STATE:   return ERR_BROKEN;
        case PC_ERR_BUSY:    return ERR_BUSY;
        case PC_ERR_NOMEM:   return ERR_NOMEM;
        case PC_ERR_QUOTA:   return ERR_QUOTA;
        default:             return ERR_IO;
    }
}

const char* pc_strerror(int code)
{
    switch (code) {
        case PC_OK:           return "ok";
        case PC_ERR_NOSPACE:  return "no free slot / router full";
        case PC_ERR_BADARG:   return "bad argument";
        case PC_ERR_NOENT:    return "not found";
        case PC_ERR_STALE:    return "stale handle";
        case PC_ERR_DENIED:   return "policy denied";
        case PC_ERR_OVERFLOW: return "buffer overflow (truncated)";
        case PC_ERR_CAP:      return "illegal capability";
        case PC_ERR_SANDBOX:  return "path escapes sandbox";
        case PC_ERR_STATE:    return "invalid state";
        case PC_ERR_BUSY:     return "busy / concurrency quota";
        case PC_ERR_NOMEM:    return "out of memory";
        case PC_ERR_QUOTA:    return "invalid quota";
        default:              return "unknown error";
    }
}

/* =====================================================================
 *  模块一：原子操作与自旋锁
 * ===================================================================== */
static inline int pc_cas_u32(volatile uint32_t* slot,
                             uint32_t expect, uint32_t desire)
{
#if defined(_MSC_VER)
    return _InterlockedCompareExchange((volatile long*)slot,
                                       (long)desire, (long)expect)
           == (long)expect;
#elif defined(__GNUC__) || defined(__clang__)
    /* __atomic 在 x86 上内联为 lock cmpxchg；ARM 上内联为 ldaxr/stlxr。 */
    return __atomic_compare_exchange_n(slot, &expect, desire, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#elif defined(__x86_64__) || defined(__i386__)
    /* 没有 __atomic 的 x86 编译器走手写汇编。 */
    unsigned char ok;
    __asm__ __volatile__("lock cmpxchgl %2, %1; sete %0"
                         : "=q"(ok), "+m"(*slot)
                         : "r"(desire), "a"(expect)
                         : "memory");
    return (int)ok;
#else
#  error "no CAS primitive for this compiler/arch"
#endif
}

static inline uint32_t pc_load_u32(const volatile uint32_t* p)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(p, __ATOMIC_RELAXED);
#else
    return *p;
#endif
}

static inline void pc_store_u32(volatile uint32_t* p, uint32_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
#else
    *p = v;
#endif
}

static inline void pc_cpu_relax(void)
{
#if defined(_MSC_VER)
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* 关中断并返回 EFLAGS。只在 ring0 使用；宿主自测返回 0。 */
static inline uint32_t pc_irq_save(void)
{
#if (defined(__x86_64__) || defined(__i386__)) && \
    !defined(NEX_PC_HOST_TEST) && (defined(__GNUC__) || defined(__clang__))
    uint64_t f;
    __asm__ __volatile__("pushfq\n\tpopq %0\n\tcli" : "=r"(f) :: "memory");
    return (uint32_t)f;
#else
    return 0;
#endif
}

static inline void pc_irq_restore(uint32_t flags)
{
#if (defined(__x86_64__) || defined(__i386__)) && \
    !defined(NEX_PC_HOST_TEST) && (defined(__GNUC__) || defined(__clang__))
    if (flags & 0x200u)          /* 原本 IF=1 才恢复，避免提前开中断 */
        __asm__ __volatile__("sti" ::: "memory");
#else
    (void)flags;
#endif
}

void spin_init(spinlock_t* l)
{
    if (!l) return;
    pc_store_u32(&l->lock, 0);
}

int spin_trylock(spinlock_t* l)
{
    if (!l) return 0;
    return pc_cas_u32(&l->lock, 0, 1) ? 1 : 0;
}

void spin_lock(spinlock_t* l)
{
    if (!l) return;
    uint32_t spins = 0;
    while (!pc_cas_u32(&l->lock, 0, 1)) {
        while (pc_load_u32(&l->lock)) {
            pc_cpu_relax();
            spins++;
            if ((spins & 0x3FFu) == 0) pc_cpu_relax();
        }
    }
}

void spin_unlock(spinlock_t* l)
{
    if (!l) return;
    pc_store_u32(&l->lock, 0);
}

uint32_t spin_lock_irqsave(spinlock_t* l)
{
    uint32_t flags = pc_irq_save();
    spin_lock(l);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t* l, uint32_t flags)
{
    spin_unlock(l);
    pc_irq_restore(flags);
}

/* =====================================================================
 *  模块一：安全内存/字符串
 * ===================================================================== */
void* safe_memset(void* dst, int v, uint32_t n)
{
    unsigned char* d = (unsigned char*)dst;
    if (!d) return dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (unsigned char)v;
    return dst;
}

int safe_memcpy(void* dst, uint32_t dst_cap, const void* src, uint32_t n)
{
    if (!dst) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    if (n == 0) return pc_ok();
    if (!src) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    if (n > dst_cap) return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return pc_ok();
}

int safe_strcpy(char* dst, uint32_t dst_cap, const char* src)
{
    if (!dst || dst_cap == 0) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    if (!src) return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    uint32_t i = 0;
    while (i + 1u < dst_cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    if (src[i] != 0) return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
    return pc_ok();
}

int safe_strncpy(char* dst, uint32_t dst_cap, const char* src, uint32_t n)
{
    if (!dst || dst_cap == 0) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    if (!src) return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    uint32_t i = 0;
    while (i + 1u < dst_cap && i < n && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    if (i == n && src[i] != 0) return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
    if (i + 1u == dst_cap && src[i] != 0)
        return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
    return pc_ok();
}

uint32_t safe_strlen(const char* s, uint32_t cap)
{
    if (!s) return 0;
    uint32_t i = 0;
    while (i < cap && s[i]) i++;
    return i;
}

int safe_strcmp(const char* a, const char* b)
{
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int safe_strncmp(const char* a, const char* b, uint32_t n)
{
    if (a == b || n == 0) return 0;
    if (!a) return -1;
    if (!b) return 1;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
    }
    return 0;
}

int safe_streq(const char* a, const char* b)
{
    return safe_strcmp(a, b) == 0 ? 1 : 0;
}

/* =====================================================================
 *  模块三：FNV-1a
 * ===================================================================== */
uint32_t fnv1a32(const void* data, uint32_t len)
{
    const unsigned char* p = (const unsigned char*)data;
    uint32_t h = 2166136261u;              /* FNV offset basis */
    if (!p) return h;
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;                    /* FNV prime */
    }
    return h;
}

uint32_t fnv1a32_str(const char* s)
{
    if (!s) return 2166136261u;
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint32_t)(unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}
/* =====================================================================
 *  模块三：一层 JSON 解析器
 * ---------------------------------------------------------------------
 *  只接受顶层对象 {"key":value,...}。嵌套对象/数组会被完整跳过并做
 *  语法校验，但不会展开——这正是 MCP tools/call 的参数形状。
 * ===================================================================== */
typedef enum PcJsonType {
    PCJSON_NULL = 0,
    PCJSON_BOOL,
    PCJSON_NUM,
    PCJSON_STR,
    PCJSON_OBJ,
    PCJSON_ARR
} PcJsonType;

typedef struct PcJsonSpan {
    PcJsonType  type;
    const char* start;      /* 值首字节（含引号）                       */
    const char* end;        /* 值后一字节                                */
} PcJsonSpan;

static int pc_hexval(char c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static const char* pc_skip_ws(const char* p, const char* end)
{
    while (p < end) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { p++; continue; }
        break;
    }
    return p;
}

/* 只校验并跳过一个 JSON 字符串，不产出内容。 */
static int pc_json_skip_string(const char** pp, const char* end)
{
    const char* p = *pp;
    if (p >= end || *p != '"') return 0;
    p++;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') { *pp = p; return 1; }
        if (c < 0x20u) return 0;
        if (c == '\\') {
            if (p >= end) return 0;
            char e = *p++;
            if (e == '"' || e == '\\' || e == '/' ||
                e == 'b' || e == 'f' || e == 'n' ||
                e == 'r' || e == 't') {
                continue;
            }
            if (e == 'u') {
                if (p + 4 > end) return 0;
                for (int i = 0; i < 4; i++) {
                    if (pc_hexval(p[i]) < 0) return 0;
                }
                p += 4;
                continue;
            }
            return 0;
        }
    }
    return 0;
}

/* 校验并跳过一个数字（严格 JSON 文法，不做数值转换）。 */
static int pc_json_skip_number(const char** pp, const char* end)
{
    const char* p = *pp;
    if (p < end && *p == '-') p++;
    if (p >= end) return 0;

    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < end && *p >= '0' && *p <= '9') p++;
    } else {
        return 0;
    }

    if (p < end && *p == '.') {
        p++;
        if (p >= end || *p < '0' || *p > '9') return 0;
        while (p < end && *p >= '0' && *p <= '9') p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (p >= end || *p < '0' || *p > '9') return 0;
        while (p < end && *p >= '0' && *p <= '9') p++;
    }
    *pp = p;
    return 1;
}

static int pc_json_match(const char** pp, const char* end, const char* lit)
{
    const char* p = *pp;
    while (*lit) {
        if (p >= end || *p != *lit) return 0;
        p++; lit++;
    }
    *pp = p;
    return 1;
}

/* 递归跳过任意值；depth 上限 PC_JSON_MAX_DEPTH，防止恶意深层嵌套。 */
static int pc_json_skip_value(const char** pp, const char* end,
                              uint32_t depth, PcJsonType* type)
{
    if (depth > PC_JSON_MAX_DEPTH) return 0;
    const char* p = pc_skip_ws(*pp, end);
    if (p >= end) return 0;

    char c = *p;
    if (c == '"') {
        if (!pc_json_skip_string(&p, end)) return 0;
        *type = PCJSON_STR;
    } else if (c == '{') {
        p++;
        p = pc_skip_ws(p, end);
        if (p < end && *p == '}') { p++; *type = PCJSON_OBJ; *pp = p; return 1; }
        for (;;) {
            p = pc_skip_ws(p, end);
            if (!pc_json_skip_string(&p, end)) return 0;
            p = pc_skip_ws(p, end);
            if (p >= end || *p != ':') return 0;
            p++;
            if (!pc_json_skip_value(&p, end, depth + 1, type)) return 0;
            p = pc_skip_ws(p, end);
            if (p >= end) return 0;
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; break; }
            return 0;
        }
        *type = PCJSON_OBJ;
    } else if (c == '[') {
        p++;
        p = pc_skip_ws(p, end);
        if (p < end && *p == ']') { p++; *type = PCJSON_ARR; *pp = p; return 1; }
        for (;;) {
            if (!pc_json_skip_value(&p, end, depth + 1, type)) return 0;
            p = pc_skip_ws(p, end);
            if (p >= end) return 0;
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; break; }
            return 0;
        }
        *type = PCJSON_ARR;
    } else if (c == 't') {
        if (!pc_json_match(&p, end, "true")) return 0;
        *type = PCJSON_BOOL;
    } else if (c == 'f') {
        if (!pc_json_match(&p, end, "false")) return 0;
        *type = PCJSON_BOOL;
    } else if (c == 'n') {
        if (!pc_json_match(&p, end, "null")) return 0;
        *type = PCJSON_NULL;
    } else {
        if (!pc_json_skip_number(&p, end)) return 0;
        *type = PCJSON_NUM;
    }
    *pp = p;
    return 1;
}

/* 完整解码一个字符串 token（p 指向 '"'），写入 out。
 * 成功返回 PC_OK；\u0000、坏转义、缓冲区不足都返回负值。 */
static int pc_json_decode_string(const char** pp, const char* end,
                                 char* out, uint32_t outcap,
                                 uint32_t* outlen)
{
    const char* p = *pp;
    if (p >= end || *p != '"') return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    p++;

    uint32_t o = 0;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            if (outcap == 0) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
            out[o] = 0;
            *pp = p;
            if (outlen) *outlen = o;
            return pc_ok();
        }
        if (c < 0x20u) return pc_fail(PC_ERR_BADARG, ERR_BADARG);

        uint32_t cp = c;
        if (c == '\\') {
            if (p >= end) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
            char e = *p++;
            switch (e) {
                case '"':  cp = '"';  break;
                case '\\': cp = '\\'; break;
                case '/':  cp = '/';  break;
                case 'b':  cp = 0x08; break;
                case 'f':  cp = 0x0C; break;
                case 'n':  cp = 0x0A; break;
                case 'r':  cp = 0x0D; break;
                case 't':  cp = 0x09; break;
                case 'u': {
                    if (p + 4 > end) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
                    uint32_t hi = 0;
                    for (int i = 0; i < 4; i++) {
                        int v = pc_hexval(p[i]);
                        if (v < 0) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
                        hi = (hi << 4) | (uint32_t)v;
                    }
                    p += 4;
                    cp = hi;
                    if (hi >= 0xD800u && hi <= 0xDBFFu) {
                        /* 高代理项后面必须跟 \uDC00-\uDFFF */
                        if (p + 6 > end || p[0] != '\\' || p[1] != 'u')
                            return pc_fail(PC_ERR_BADARG, ERR_BADARG);
                        uint32_t lo = 0;
                        for (int i = 0; i < 4; i++) {
                            int v = pc_hexval(p[2 + i]);
                            if (v < 0) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
                            lo = (lo << 4) | (uint32_t)v;
                        }
                        if (lo < 0xDC00u || lo > 0xDFFFu)
                            return pc_fail(PC_ERR_BADARG, ERR_BADARG);
                        p += 6;
                        cp = 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
                    } else if (hi >= 0xDC00u && hi <= 0xDFFFu) {
                        return pc_fail(PC_ERR_BADARG, ERR_BADARG); /* 孤立低代理 */
                    }
                    break;
                }
                default:
                    return pc_fail(PC_ERR_BADARG, ERR_BADARG);
            }
        }

        /* \u0000 会让 C 字符串截断，进而绕过路径检查，直接拒绝。 */
        if (cp == 0) return pc_fail(PC_ERR_BADARG, ERR_BADARG);

        /* UTF-8 编码 */
        if (cp <= 0x7Fu) {
            if (o + 1u >= outcap) return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
            out[o++] = (char)cp;
        } else if (cp <= 0x7FFu) {
            if (o + 2u >= outcap) return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
            out[o++] = (char)(0xC0u | (cp >> 6));
            out[o++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp <= 0xFFFFu) {
            if (o + 3u >= outcap) return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
            out[o++] = (char)(0xE0u | (cp >> 12));
            out[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[o++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            if (o + 4u >= outcap) return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
            out[o++] = (char)(0xF0u | (cp >> 18));
            out[o++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            out[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[o++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    return pc_fail(PC_ERR_BADARG, ERR_BADARG);
}

/* 顶层查找。found=1 时 out 指向原始值跨度；解析错误返回负值。 */
static int pc_json_find(const char* json, const char* key,
                        PcJsonSpan* out, int* found)
{
    if (found) *found = 0;
    if (!json || !key || !out) return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    uint32_t key_len = safe_strlen(key, PC_MAX_JSON_KEY);
    if (key_len == 0 || key_len >= PC_MAX_JSON_KEY)
        return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    uint32_t len = safe_strlen(json, PC_MAX_JSON);
    if (len >= PC_MAX_JSON) return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
    if (len == 0) return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    const char* p = json;
    const char* end = json + len;
    p = pc_skip_ws(p, end);
    if (p >= end || *p != '{') return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    p++;
    p = pc_skip_ws(p, end);

    if (p < end && *p == '}') {
        p++;
        p = pc_skip_ws(p, end);
        if (p != end) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
        return pc_ok();
    }

    for (;;) {
        p = pc_skip_ws(p, end);
        if (p >= end || *p != '"') return pc_fail(PC_ERR_BADARG, ERR_BADARG);

        const char* key_start = p + 1;
        if (!pc_json_skip_string(&p, end))
            return pc_fail(PC_ERR_BADARG, ERR_BADARG);
        const char* key_end = p - 1;       /* 指向结尾引号 */

        p = pc_skip_ws(p, end);
        if (p >= end || *p != ':')
            return pc_fail(PC_ERR_BADARG, ERR_BADARG);
        p++;
        p = pc_skip_ws(p, end);

        const char* vstart = p;
        PcJsonType vtype = PCJSON_NULL;
        if (!pc_json_skip_value(&p, end, 0, &vtype))
            return pc_fail(PC_ERR_BADARG, ERR_BADARG);

        /* 键里若有转义，逐字节比较会漏掉；安全做法是解码后比较。
         * 这里只用无转义的常见键（path/ptr/len/...），遇到转义键
         * 就按“不匹配”处理——攻击者无法用 "\u0070ath" 绕过。 */
        int key_ok = 0;
        uint32_t raw_len = (uint32_t)(key_end - key_start);
        if (raw_len == key_len) {
            key_ok = 1;
            for (uint32_t i = 0; i < key_len; i++) {
                if (key_start[i] != key[i]) { key_ok = 0; break; }
            }
        }
        if (key_ok && !*found) {
            out->type  = vtype;
            out->start = vstart;
            out->end   = p;
            *found = 1;
        }

        p = pc_skip_ws(p, end);
        if (p >= end) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    }

    p = pc_skip_ws(p, end);
    if (p != end) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    return pc_ok();
}

int json_has_key(const char* json, const char* key)
{
    PcJsonSpan span;
    int found = 0;
    int rc = pc_json_find(json, key, &span, &found);
    if (rc != PC_OK) return 0;
    return found ? 1 : 0;
}

int json_get_string(const char* json, const char* key,
                    char* out, uint32_t outcap)
{
    if (!out || outcap == 0) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    PcJsonSpan span;
    int found = 0;
    int rc = pc_json_find(json, key, &span, &found);
    if (rc != PC_OK) return rc;
    if (!found) return pc_fail(PC_ERR_NOENT, ERR_NOTFOUND);
    if (span.type != PCJSON_STR) return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    const char* p = span.start;
    return pc_json_decode_string(&p, span.end, out, outcap, NULL);
}

int json_get_int64(const char* json, const char* key, int64_t* out)
{
    if (!out) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    PcJsonSpan span;
    int found = 0;
    int rc = pc_json_find(json, key, &span, &found);
    if (rc != PC_OK) return rc;
    if (!found) return pc_fail(PC_ERR_NOENT, ERR_NOTFOUND);
    if (span.type != PCJSON_NUM) return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    const char* p = span.start;
    const char* end = span.end;
    int neg = 0;
    if (p < end && *p == '-') { neg = 1; p++; }

    uint64_t mag = 0;
    const uint64_t neg_limit = 9223372036854775808ULL; /* 2^63 */
    while (p < end && *p >= '0' && *p <= '9') {
        uint32_t d = (uint32_t)(*p - '0');
        if (mag > (neg ? neg_limit : 9223372036854775807ULL) / 10ULL) {
            return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
        }
        mag = mag * 10ULL + d;
        if (mag > (neg ? neg_limit : 9223372036854775807ULL))
            return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
        p++;
    }
    if (p < end) return pc_fail(PC_ERR_BADARG, ERR_BADARG); /* 小数/指数 */

    if (neg) {
        if (mag == 0) { *out = 0; return pc_ok(); }
        if (mag == 9223372036854775808ULL)
            *out = (-9223372036854775807LL - 1LL);  /* INT64_MIN，避免 UB */
        else
            *out = -(int64_t)mag;
    } else {
        *out = (int64_t)mag;
    }
    return pc_ok();
}

int json_get_bool(const char* json, const char* key, int* out)
{
    if (!out) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    PcJsonSpan span;
    int found = 0;
    int rc = pc_json_find(json, key, &span, &found);
    if (rc != PC_OK) return rc;
    if (!found) return pc_fail(PC_ERR_NOENT, ERR_NOTFOUND);
    if (span.type != PCJSON_BOOL) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    *out = (span.start < span.end && *span.start == 't') ? 1 : 0;
    return pc_ok();
}

/* =====================================================================
 *  JSON 输出（错误响应）
 * ===================================================================== */
typedef struct PcWriter {
    char*    buf;
    uint32_t cap;
    uint32_t o;
    int      overflow;
} PcWriter;

static void jw_init(PcWriter* w, char* buf, uint32_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->o = 0;
    w->overflow = 0;
    if (buf && cap) buf[0] = 0;
}

static void jw_ch(PcWriter* w, char c)
{
    if (!w->buf || w->cap == 0) { w->overflow = 1; return; }
    if (w->o + 1u >= w->cap) { w->overflow = 1; return; }
    w->buf[w->o++] = c;
    w->buf[w->o] = 0;
}

static void jw_raw(PcWriter* w, const char* s)
{
    if (!s) return;
    while (*s) jw_ch(w, *s++);
}

static void jw_int(PcWriter* w, int64_t v)
{
    char tmp[24];
    uint32_t n = 0;
    int neg = (v < 0);
    uint64_t mag = neg ? ((uint64_t)(-(v + 1)) + 1ULL) : (uint64_t)v;
    if (mag == 0) { jw_ch(w, '0'); return; }
    while (mag > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (uint32_t)(mag % 10ULL));
        mag /= 10ULL;
    }
    if (neg) jw_ch(w, '-');
    while (n > 0) jw_ch(w, tmp[--n]);
}

int json_escape_into(char* out, uint32_t outcap, const char* s)
{
    if (!out || outcap == 0) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    uint32_t o = 0;
    if (!s) { out[0] = 0; return pc_ok(); }

    static const char* hexd = "0123456789abcdef";
    for (uint32_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (o + 2u >= outcap) return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20u) {
            if (o + 6u >= outcap) return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
            out[o++] = '\\';
            out[o++] = 'u';
            out[o++] = '0';
            out[o++] = '0';
            out[o++] = hexd[(c >> 4) & 0x0F];
            out[o++] = hexd[c & 0x0F];
        } else {
            if (o + 1u >= outcap) return pc_fail(PC_ERR_OVERFLOW, ERR_TOOBIG);
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return pc_ok();
}

static void jw_esc(PcWriter* w, const char* s)
{
    if (!s) return;
    static const char* hexd = "0123456789abcdef";
    for (uint32_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            jw_ch(w, '\\');
            jw_ch(w, (char)c);
        } else if (c < 0x20u) {
            jw_ch(w, '\\'); jw_ch(w, 'u'); jw_ch(w, '0'); jw_ch(w, '0');
            jw_ch(w, hexd[(c >> 4) & 0x0F]);
            jw_ch(w, hexd[c & 0x0F]);
        } else {
            jw_ch(w, (char)c);
        }
    }
}

static void jw_end(PcWriter* w)
{
    if (w->buf && w->cap) {
        if (w->o >= w->cap) w->o = w->cap - 1;
        w->buf[w->o] = 0;
    }
}

static int pc_rpc_code(int rc)
{
    switch (rc) {
        case PC_ERR_BADARG:
        case PC_ERR_OVERFLOW: return -32602;   /* Invalid params */
        case PC_ERR_DENIED:
        case PC_ERR_SANDBOX:
        case PC_ERR_CAP:      return -32001;   /* Policy denied  */
        case PC_ERR_NOENT:    return -32004;   /* Not found      */
        case PC_ERR_STALE:    return -32002;   /* Stale handle   */
        case PC_ERR_BUSY:     return -32003;   /* Busy           */
        default:              return -32603;   /* Internal error */
    }
}

static void pc_write_error(char* out, uint32_t outcap, int rc, const char* tool)
{
    if (!out || outcap == 0) return;
    PcWriter w;
    jw_init(&w, out, outcap);
    jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":");
    jw_int(&w, (int64_t)pc_rpc_code(rc));
    jw_raw(&w, ",\"message\":\"");
    jw_esc(&w, pc_strerror(rc));
    jw_raw(&w, "\",\"data\":{\"errno\":");
    jw_int(&w, (int64_t)rc);
    jw_raw(&w, ",\"tool\":\"");
    jw_esc(&w, tool ? tool : "");
    jw_raw(&w, "\"}}}");
    jw_end(&w);
}
/* =====================================================================
 *  模块二：页权限影子 + 用户指针校验
 * ===================================================================== */
static int pc_page_contains(const PagePermission* pg, uint64_t start,
                            uint64_t end, uint32_t prot)
{
    if (!pg || pg->size == 0) return 0;
    if ((pg->prot & prot) != prot) return 0;
    if (pg->base > 0xFFFFFFFFFFFFFFFFULL - pg->size) return 0;
    uint64_t pg_end = pg->base + pg->size - 1ULL;
    return (start >= pg->base && end <= pg_end) ? 1 : 0;
}

int is_valid_user_ptr(const PcPlugin* p, const void* ptr,
                      uint32_t len, uint32_t prot)
{
    if (!p) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    if (prot == 0 || (prot & ~(PC_PROT_READ | PC_PROT_WRITE | PC_PROT_EXEC)) != 0)
        return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    if (!ptr || len == 0) return pc_fail(PC_ERR_DENIED, ERR_PERMDENIED);

    uint64_t start = (uint64_t)(uintptr_t)ptr;
    uint64_t end   = start + (uint64_t)len - 1ULL;
    if (end < start) return pc_fail(PC_ERR_DENIED, ERR_PERMDENIED); /* 回绕 */
    if (start < PC_USER_PTR_MIN || end > PC_USER_PTR_MAX)
        return pc_fail(PC_ERR_DENIED, ERR_PERMDENIED);

    for (uint32_t i = 0; i < p->page_count && i < PC_MAX_PAGES; i++) {
        const PagePermission* pg = &p->pages[i];
        if (pg->owner != 0 && pg->owner != p->h) continue;
        if (pc_page_contains(pg, start, end, prot)) {
            pc_ok();
            return 1;
        }
    }
    return pc_fail(PC_ERR_DENIED, ERR_PERMDENIED);
}

int safe_copy_from_user(const PcPlugin* p, void* dst, uint32_t dst_cap,
                        const void* user_src, uint32_t len)
{
    if (!dst || dst_cap == 0) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    if (len > dst_cap) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    /* is_valid_user_ptr 返回 1 合法、负值拒绝；不能用 !rc，否则 -5 是真值。 */
    if (is_valid_user_ptr(p, user_src, len, PC_PROT_READ) != 1)
        return PC_ERR_DENIED;
    return safe_memcpy(dst, dst_cap, user_src, len);
}

/* =====================================================================
 *  路径规范化与沙盒
 * ===================================================================== */
static int pc_norm_path(const char* in, char* out, uint32_t cap)
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
            if (c < 0x20u || c == '\\') return -1;
            i++;
        }
        uint32_t len = i - s;

        if (len == 1 && in[s] == '.') continue;
        if (len == 2 && in[s] == '.' && in[s + 1] == '.') return -1;

        if (o > 1) {
            if (o + 1u >= cap) return -1;
            out[o++] = '/';
        }
        if (o + len >= cap) return -1;
        for (uint32_t k = 0; k < len; k++) out[o++] = in[s + k];
    }
    out[o] = 0;
    return 0;
}

/* 1 = path 在 root 之内（root 自身也算）。 */
static int pc_path_in_sandbox(const char* root, const char* path)
{
    char r[PC_MAX_PATH];
    char p[PC_MAX_PATH];
    if (pc_norm_path(root, r, sizeof(r)) != 0) return 0;
    if (pc_norm_path(path, p, sizeof(p)) != 0) return 0;
    if (safe_streq(r, "/")) return 0;

    uint32_t rl = safe_strlen(r, PC_MAX_PATH);
    if (safe_strncmp(p, r, rl) != 0) return 0;
    return (p[rl] == 0 || p[rl] == '/') ? 1 : 0;
}

static int pc_id_ok(const char* id)
{
    if (!id) return 0;
    uint32_t n = safe_strlen(id, PC_MAX_ID);
    if (n < 2 || n >= PC_MAX_ID) return 0;
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

static void pc_build_root(const char* id, char* out, uint32_t cap)
{
    safe_strcpy(out, cap, "/sandbox/");
    uint32_t base = safe_strlen(out, cap);
    safe_strncpy(out + base, cap - base, id, cap - base - 1u);
}

/* =====================================================================
 *  模块四：全局状态与槽位位图
 * ===================================================================== */
static PcPlugin   g_slots[PC_MAX_PLUGINS];
static uint32_t   g_slot_bits[(PC_MAX_PLUGINS + 31) / 32];
static spinlock_t g_core_lock;
static uint32_t   g_live;
static uint32_t   g_oom_tripped;
static int        g_initialized;
static McpRouter  g_router;

static int pc_ctz32(uint32_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(x);
#else
    int n = 0;
    if (!x) return 32;
    while ((x & 1u) == 0) { x >>= 1; n++; }
    return n;
#endif
}

/* 返回槽位号；-1 = 满。O(1)：最多扫 4 个 32 位字。 */
static int pc_slot_alloc_locked(void)
{
    for (uint32_t w = 0; w < (PC_MAX_PLUGINS + 31) / 32; w++) {
        uint32_t avail = ~g_slot_bits[w];
        if (!avail) continue;
        int b = pc_ctz32(avail);
        uint32_t idx = w * 32u + (uint32_t)b;
        if (idx >= PC_MAX_PLUGINS) continue;
        if (g_slot_bits[w] & (1u << b)) continue;
        g_slot_bits[w] |= (1u << b);
        return (int)idx;
    }
    return -1;
}

static void pc_slot_free_locked(uint32_t idx)
{
    if (idx >= PC_MAX_PLUGINS) return;
    g_slot_bits[idx >> 5] &= ~(1u << (idx & 31u));
}

static uint32_t pc_gen_next(uint32_t gen)
{
    gen = (gen + 1u) & 0x00FFFFFFu;   /* 24 位 */
    if (gen == 0) gen = 1;            /* 0 保留给无效句柄 */
    return gen;
}

/* O(1)：低 8 位定位槽位，高位比较代际。 */
static PcPlugin* pc_resolve_locked(uint32_t h)
{
    if (!h) return NULL;
    uint32_t slot = (h & 0xFFu);
    if (slot == 0) return NULL;
    slot--;
    if (slot >= PC_MAX_PLUGINS) return NULL;
    PcPlugin* p = &g_slots[slot];
    if (p->state != 1) return NULL;
    if (((h >> 8) & 0x00FFFFFFu) != p->gen) return NULL;
    return p;
}

/* =====================================================================
 *  模块三：MCP 哈希路由
 * ===================================================================== */
static void pc_chain_repoint(McpRouter* r, uint32_t h,
                             uint32_t old_idx, uint32_t new_idx)
{
    uint32_t slot = (h & 0xFFu);
    if (slot == 0 || slot > PC_MAX_PLUGINS) return;
    slot--;
    uint16_t want = (uint16_t)(old_idx + 1u);
    uint16_t to   = (uint16_t)(new_idx + 1u);

    if (r->heads[slot] == want) { r->heads[slot] = to; return; }
    uint16_t cur = r->heads[slot];
    uint32_t guard = 0;
    while (cur && guard++ < PC_ROUTER_SLOTS) {
        McpRoute* e = &r->entries[cur - 1u];
        if (e->next == want) { e->next = to; return; }
        cur = e->next;
    }
}

static void pc_chain_unlink(McpRouter* r, uint32_t h, uint32_t idx)
{
    uint32_t slot = (h & 0xFFu);
    if (slot == 0 || slot > PC_MAX_PLUGINS) return;
    slot--;
    uint16_t want = (uint16_t)(idx + 1u);

    if (r->heads[slot] == want) {
        r->heads[slot] = r->entries[idx].next;
        return;
    }
    uint16_t cur = r->heads[slot];
    uint32_t guard = 0;
    while (cur && guard++ < PC_ROUTER_SLOTS) {
        McpRoute* e = &r->entries[cur - 1u];
        if (e->next == want) {
            e->next = r->entries[idx].next;
            return;
        }
        cur = e->next;
    }
}

/* Knuth 回移删除：保证“空槽 = 探测链终点”不变式，且不留墓碑。 */
static void pc_router_delete_locked(McpRouter* r, uint32_t idx)
{
    McpRoute* e = &r->entries[idx];
    uint32_t h = e->handle;

    pc_chain_unlink(r, h, idx);
    e->state = 0;
    e->next = 0;
    if (r->used > 0) r->used--;

    /* 回移的正确条件：只有“探测路径跨过空洞”的条目才能搬进空洞。
     * 不能一遇到 home==j 就停——那会留下一个把后续同簇条目挡住的洞。
     * 正确做法是跳过不可搬的条目，继续向后扫描，直到遇到空槽。 */
    uint32_t hole = idx;
    uint32_t j = (hole + 1u) & PC_ROUTER_MASK;
    uint32_t guard = 0;
    while (guard++ < PC_ROUTER_SLOTS) {
        McpRoute* m = &r->entries[j];
        if (m->state != 1) break;

        uint32_t home = m->hash & PC_ROUTER_MASK;
        int can_move;
        if (hole <= j)
            can_move = (home <= hole || home > j);
        else
            can_move = (home <= hole && home > j);

        if (can_move) {
            r->entries[hole] = *m;
            r->entries[j].state = 0;
            r->entries[j].next  = 0;
            pc_chain_repoint(r, r->entries[hole].handle, j, hole);
            hole = j;
        }
        j = (j + 1u) & PC_ROUTER_MASK;
    }
}

void router_init(McpRouter* r)
{
    if (!r) return;
    safe_memset(r, 0, (uint32_t)sizeof(*r));
    spin_init(&r->lock);
}

int router_register(McpRouter* r, uint32_t h, const char* tool,
                    pc_tool_fn fn, uint32_t caps, uint32_t flags)
{
    if (!r || !tool || !fn) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    if (caps & ~PC_CAP_KNOWN) return pc_fail(PC_ERR_CAP, ERR_PERMDENIED);
    if (flags & ~(PC_TOOL_PATH_SANDBOX | PC_TOOL_USER_PTR))
        return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    uint32_t tlen = safe_strlen(tool, PC_MAX_TOOL);
    if (tlen == 0 || tlen >= PC_MAX_TOOL)
        return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    uint32_t slot = (h & 0xFFu);
    if (slot == 0 || slot > PC_MAX_PLUGINS)
        return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    uint32_t hval = fnv1a32_str(tool);
    uint32_t start = hval & PC_ROUTER_MASK;

    uint32_t fl = spin_lock_irqsave(&r->lock);
    if ((r->used + 1u) * PC_ROUTER_MAX_LOAD_DEN >
        PC_ROUTER_SLOTS * PC_ROUTER_MAX_LOAD_NUM) {
        spin_unlock_irqrestore(&r->lock, fl);
        return pc_fail(PC_ERR_NOSPACE, ERR_NOSPACE);
    }

    for (uint32_t i = 0; i < PC_ROUTER_SLOTS; i++) {
        uint32_t idx = (start + i) & PC_ROUTER_MASK;
        McpRoute* e = &r->entries[idx];
        if (e->state != 1) {
            e->hash   = hval;
            e->handle = h;
            e->caps   = caps;
            e->flags  = flags;
            e->fn     = fn;
            e->state  = 1;
            e->pad    = 0;
            safe_strcpy(e->tool, PC_MAX_TOOL, tool);

            slot--;                       /* 路由链下标 */
            e->next = r->heads[slot];
            r->heads[slot] = (uint16_t)(idx + 1u);
            r->used++;
            spin_unlock_irqrestore(&r->lock, fl);
            return pc_ok();
        }
        if (e->hash == hval && safe_streq(e->tool, tool)) {
            spin_unlock_irqrestore(&r->lock, fl);
            return pc_fail(PC_ERR_DENIED, ERR_NAMEEXISTS);
        }
    }

    spin_unlock_irqrestore(&r->lock, fl);
    return pc_fail(PC_ERR_NOSPACE, ERR_NOSPACE);
}

/* 查找 idx；-1 = 未找到。调用者持锁。 */
static int pc_router_find_locked(McpRouter* r, const char* tool)
{
    if (!r || !tool) return -1;
    uint32_t hval = fnv1a32_str(tool);
    uint32_t start = hval & PC_ROUTER_MASK;
    for (uint32_t i = 0; i < PC_ROUTER_SLOTS; i++) {
        uint32_t idx = (start + i) & PC_ROUTER_MASK;
        McpRoute* e = &r->entries[idx];
        if (e->state != 1) return -1;
        if (e->hash == hval && safe_streq(e->tool, tool)) return (int)idx;
    }
    return -1;
}

int router_unregister_tool(McpRouter* r, const char* tool)
{
    if (!r || !tool) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    uint32_t fl = spin_lock_irqsave(&r->lock);
    int idx = pc_router_find_locked(r, tool);
    if (idx < 0) {
        spin_unlock_irqrestore(&r->lock, fl);
        return pc_fail(PC_ERR_NOENT, ERR_NOTFOUND);
    }
    pc_router_delete_locked(r, (uint32_t)idx);
    spin_unlock_irqrestore(&r->lock, fl);
    return pc_ok();
}

int router_unregister_handle(McpRouter* r, uint32_t h)
{
    if (!r) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    uint32_t slot = (h & 0xFFu);
    if (slot == 0 || slot > PC_MAX_PLUGINS)
        return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    slot--;

    uint32_t fl = spin_lock_irqsave(&r->lock);
    uint32_t guard = 0;
    while (r->heads[slot] && guard++ < PC_ROUTER_SLOTS) {
        uint32_t idx = (uint32_t)r->heads[slot] - 1u;
        if (idx >= PC_ROUTER_SLOTS || r->entries[idx].state != 1) {
            r->heads[slot] = 0;
            break;
        }
        pc_router_delete_locked(r, idx);
    }
    spin_unlock_irqrestore(&r->lock, fl);
    return pc_ok();
}

int router_lookup(McpRouter* r, const char* tool, McpRoute* out)
{
    if (!r || !tool) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    uint32_t hval = fnv1a32_str(tool);
    uint32_t start = hval & PC_ROUTER_MASK;

    uint32_t fl = spin_lock_irqsave(&r->lock);
    r->lookups++;
    int found = 0;
    uint32_t used_probes = 0;
    for (uint32_t i = 0; i < PC_ROUTER_SLOTS; i++) {
        uint32_t idx = (start + i) & PC_ROUTER_MASK;
        McpRoute* e = &r->entries[idx];
        if (e->state != 1) { used_probes = i + 1u; break; }
        if (e->hash == hval && safe_streq(e->tool, tool)) {
            if (out) *out = *e;
            found = 1;
            used_probes = i + 1u;
            break;
        }
    }
    r->probes += used_probes;
    if (used_probes > r->probe_max) r->probe_max = used_probes;
    if (found) r->hits++;
    spin_unlock_irqrestore(&r->lock, fl);

    if (!found) return pc_fail(PC_ERR_NOENT, ERR_NOTFOUND);
    return pc_ok();
}
/* =====================================================================
 *  模块四：生命周期
 * ===================================================================== */
int pc_core_init(void)
{
    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    for (uint32_t i = 0; i < PC_MAX_PLUGINS; i++) {
        safe_memset(&g_slots[i], 0, (uint32_t)sizeof(g_slots[i]));
    }
    safe_memset(g_slot_bits, 0, (uint32_t)sizeof(g_slot_bits));
    g_live = 0;
    g_oom_tripped = 0;
    g_initialized = 1;
    router_init(&g_router);
    spin_unlock_irqrestore(&g_core_lock, fl);
    return pc_ok();
}

int load_plugin(const PcManifest* m, int is_signed, uint32_t* out_h)
{
    if (out_h) *out_h = 0;
    if (!m || !out_h) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    if (!pc_id_ok(m->id)) return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    if (m->capabilities & ~PC_CAP_KNOWN)
        return pc_fail(PC_ERR_CAP, ERR_PERMDENIED);
    if (m->capabilities & PC_CAP_IPC)
        return pc_fail(PC_ERR_CAP, ERR_PERMDENIED);   /* 单进程零 IPC */

    uint32_t abi = m->abi_version ? m->abi_version : 1u;
    if (abi > 1u) return pc_fail(PC_ERR_STATE, ERR_BROKEN);

    uint32_t quota = m->quota ? m->quota : PC_MAX_INFLIGHT;
    if (quota > PC_HARD_INFLIGHT) return pc_fail(PC_ERR_QUOTA, ERR_QUOTA);

    if (m->sandbox_base == 0 || m->sandbox_size < PC_MIN_SANDBOX ||
        m->sandbox_size > PC_MAX_SANDBOX)
        return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    if (m->sandbox_base > 0xFFFFFFFFFFFFFFFFULL - (uint64_t)m->sandbox_size)
        return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    if (is_signed) {
        if (m->extra_page_count > PC_MAX_PAGES - 1u)
            return pc_fail(PC_ERR_BADARG, ERR_BADARG);
        if (m->extra_page_count > 0 && !m->extra_pages)
            return pc_fail(PC_ERR_BADARG, ERR_BADARG);
        for (uint32_t i = 0; i < m->extra_page_count; i++) {
            const PagePermission* pg = &m->extra_pages[i];
            if (pg->size == 0) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
            if (pg->prot == 0 ||
                (pg->prot & ~(PC_PROT_READ | PC_PROT_WRITE | PC_PROT_EXEC)) != 0)
                return pc_fail(PC_ERR_BADARG, ERR_BADARG);
            if (pg->base > 0xFFFFFFFFFFFFFFFFULL - pg->size)
                return pc_fail(PC_ERR_BADARG, ERR_BADARG);
        }
    }

    /* 根目录：未签名永远 /sandbox/<id>；签名也只有 /sandbox 之下才认。 */
    char root[PC_MAX_ROOT];
    int root_ok = 0;
    if (is_signed && m->sandbox_root) {
        char norm[PC_MAX_PATH];
        if (pc_norm_path(m->sandbox_root, norm, sizeof(norm)) == 0 &&
            safe_strlen(norm, PC_MAX_PATH) < PC_MAX_ROOT &&
            safe_strncmp(norm, "/sandbox/", 9) == 0 &&
            pc_path_in_sandbox("/sandbox", norm)) {
            safe_strcpy(root, sizeof(root), norm);
            root_ok = 1;
        }
    }
    if (!root_ok) pc_build_root(m->id, root, sizeof(root));

    uint32_t caps = m->capabilities;
    if (!is_signed) caps &= ~PC_CAP_FS_WRITE;   /* 未签名绝不允许写 */

    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    if (g_oom_tripped) {
        spin_unlock_irqrestore(&g_core_lock, fl);
        return pc_fail(PC_ERR_NOSPACE, ERR_NOSPACE);
    }

    int slot = pc_slot_alloc_locked();
    if (slot < 0) {
        g_oom_tripped = 1;                       /* OOM 熔断 */
        spin_unlock_irqrestore(&g_core_lock, fl);
        return pc_fail(PC_ERR_NOSPACE, ERR_NOSPACE);
    }

    PcPlugin* p = &g_slots[(uint32_t)slot];
    uint32_t old_gen = p->gen;
    safe_memset(p, 0, (uint32_t)sizeof(*p));

    p->gen = pc_gen_next(old_gen);
    p->state = 1;
    p->slot  = (uint32_t)slot;
    p->h     = (p->gen << 8) | ((uint32_t)slot + 1u);
    p->caps  = caps;
    p->quota = quota;
    p->is_signed = (uint8_t)(is_signed ? 1 : 0);
    p->sandbox_base = m->sandbox_base;
    p->sandbox_size = (uint64_t)m->sandbox_size;
    safe_strcpy(p->id, PC_MAX_ID, m->id);
    safe_strcpy(p->sandbox_root, PC_MAX_ROOT, root);

    /* 影子页表：未签名只有一段 sandbox 读写页，EXEC 和额外页全丢。 */
    p->pages[0].base  = m->sandbox_base;
    p->pages[0].size  = (uint64_t)m->sandbox_size;
    p->pages[0].prot  = PC_PROT_RW;
    p->pages[0].owner = p->h;
    p->page_count = 1;

    if (is_signed) {
        for (uint32_t i = 0; i < m->extra_page_count && p->page_count < PC_MAX_PAGES; i++) {
            p->pages[p->page_count].base  = m->extra_pages[i].base;
            p->pages[p->page_count].size  = m->extra_pages[i].size;
            p->pages[p->page_count].prot  = m->extra_pages[i].prot;
            p->pages[p->page_count].owner = p->h;
            p->page_count++;
        }
    }

    g_live++;
    uint32_t h = p->h;
    spin_unlock_irqrestore(&g_core_lock, fl);

    *out_h = h;
    return pc_ok();
}

int unload_plugin(uint32_t h)
{
    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    PcPlugin* p = pc_resolve_locked(h);
    if (!p) {
        spin_unlock_irqrestore(&g_core_lock, fl);
        return pc_fail(PC_ERR_STALE, ERR_STALE);
    }
    if (p->inflight > 0) {
        spin_unlock_irqrestore(&g_core_lock, fl);
        return pc_fail(PC_ERR_BUSY, ERR_BUSY);
    }

    /* 先注销路由，再清空上下文：顺序反过来会让并发 dispatch
     * 在“插件已死、路由还在”的窗口里拿到悬空函数指针。 */
    router_unregister_handle(&g_router, h);

    uint32_t slot = p->slot;
    uint32_t newgen = pc_gen_next(p->gen);
    safe_memset(p, 0, (uint32_t)sizeof(*p));
    p->gen = newgen;               /* 旧句柄的 gen 永远对不上 */
    p->state = 0;

    pc_slot_free_locked(slot);
    if (g_live > 0) g_live--;
    spin_unlock_irqrestore(&g_core_lock, fl);
    return pc_ok();
}

int plugin_count(void)
{
    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    uint32_t n = g_live;
    spin_unlock_irqrestore(&g_core_lock, fl);
    return (int)n;
}

PcPlugin* plugin_lookup(uint32_t h)
{
    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    PcPlugin* p = pc_resolve_locked(h);
    spin_unlock_irqrestore(&g_core_lock, fl);
    if (!p) { pc_fail(PC_ERR_STALE, ERR_STALE); return NULL; }
    pc_ok();
    return p;
}

int plugin_snapshot(uint32_t h, PcPlugin* out)
{
    if (!out) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    PcPlugin* p = pc_resolve_locked(h);
    if (!p) {
        spin_unlock_irqrestore(&g_core_lock, fl);
        return pc_fail(PC_ERR_STALE, ERR_STALE);
    }
    *out = *p;
    spin_unlock_irqrestore(&g_core_lock, fl);
    return pc_ok();
}

int is_valid_user_ptr_h(uint32_t h, const void* ptr,
                        uint32_t len, uint32_t prot)
{
    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    PcPlugin* p = pc_resolve_locked(h);
    int ok = 0;
    if (!p) {
        spin_unlock_irqrestore(&g_core_lock, fl);
        return pc_fail(PC_ERR_STALE, ERR_STALE);
    }
    ok = is_valid_user_ptr(p, ptr, len, prot);
    spin_unlock_irqrestore(&g_core_lock, fl);
    return ok;
}

int register_tool(uint32_t h, const char* tool, pc_tool_fn fn,
                  uint32_t need_caps, uint32_t flags)
{
    if (!tool || !fn) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    if (need_caps & ~PC_CAP_KNOWN) return pc_fail(PC_ERR_CAP, ERR_PERMDENIED);
    if (flags & ~(PC_TOOL_PATH_SANDBOX | PC_TOOL_USER_PTR))
        return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    PcPlugin* p = pc_resolve_locked(h);
    if (!p) {
        spin_unlock_irqrestore(&g_core_lock, fl);
        return pc_fail(PC_ERR_STALE, ERR_STALE);
    }
    if ((need_caps & ~p->caps) != 0) {
        spin_unlock_irqrestore(&g_core_lock, fl);
        return pc_fail(PC_ERR_DENIED, ERR_PERMDENIED);
    }
    int rc = router_register(&g_router, h, tool, fn, need_caps, flags);
    spin_unlock_irqrestore(&g_core_lock, fl);
    return rc;
}

int unregister_tool(uint32_t h, const char* tool)
{
    if (!tool) return pc_fail(PC_ERR_BADARG, ERR_BADARG);
    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    PcPlugin* p = pc_resolve_locked(h);
    if (!p) {
        spin_unlock_irqrestore(&g_core_lock, fl);
        return pc_fail(PC_ERR_STALE, ERR_STALE);
    }
    McpRoute route;
    int rc = router_lookup(&g_router, tool, &route);
    if (rc == PC_OK && route.handle != h) {
        spin_unlock_irqrestore(&g_core_lock, fl);
        return pc_fail(PC_ERR_NOENT, ERR_NOTFOUND);
    }
    if (rc == PC_OK) rc = router_unregister_tool(&g_router, tool);
    spin_unlock_irqrestore(&g_core_lock, fl);
    return rc;
}

int pc_oom_tripped(void)
{
    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    uint32_t v = g_oom_tripped;
    spin_unlock_irqrestore(&g_core_lock, fl);
    return (int)v;
}

int pc_clear_oom_fuse(void)
{
    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    g_oom_tripped = 0;
    spin_unlock_irqrestore(&g_core_lock, fl);
    return pc_ok();
}

/* =====================================================================
 *  模块四：MCP 派发
 * ===================================================================== */
static void pc_leave_call(PcPlugin* p)
{
    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    if (p->inflight > 0) p->inflight--;
    spin_unlock_irqrestore(&g_core_lock, fl);
}

int dispatch_mcp(uint32_t h, const char* tool, const char* args_json,
                 char* out, uint32_t outcap)
{
    if (out && outcap) out[0] = 0;
    if (!out || outcap == 0 || !tool)
        return pc_fail(PC_ERR_BADARG, ERR_BADARG);

    uint32_t fl = spin_lock_irqsave(&g_core_lock);
    PcPlugin* p = pc_resolve_locked(h);

    if (!p) {
        spin_unlock_irqrestore(&g_core_lock, fl);
        pc_write_error(out, outcap, PC_ERR_STALE, tool);
        return pc_fail(PC_ERR_STALE, ERR_STALE);
    }
    if (p->inflight >= p->quota) {
        spin_unlock_irqrestore(&g_core_lock, fl);
        pc_write_error(out, outcap, PC_ERR_BUSY, tool);
        return pc_fail(PC_ERR_BUSY, ERR_BUSY);
    }

    p->inflight++;
    p->call_count++;
    char root[PC_MAX_ROOT];
    safe_strcpy(root, sizeof(root), p->sandbox_root);
    uint32_t caps = p->caps;
    spin_unlock_irqrestore(&g_core_lock, fl);

    /* 从这里开始 p 不会被 unload：inflight > 0 时 unload 返回 BUSY。 */

    McpRoute route;
    int rc = router_lookup(&g_router, tool, &route);
    if (rc != PC_OK) {
        pc_leave_call(p);
        pc_write_error(out, outcap, PC_ERR_NOENT, tool);
        return pc_fail(PC_ERR_NOENT, ERR_NOTFOUND);
    }
    if (route.handle != h) {
        pc_leave_call(p);
        pc_write_error(out, outcap, PC_ERR_DENIED, tool);
        return pc_fail(PC_ERR_DENIED, ERR_PERMDENIED);
    }
    if ((route.caps & ~caps) != 0) {
        pc_leave_call(p);
        pc_write_error(out, outcap, PC_ERR_DENIED, tool);
        return pc_fail(PC_ERR_DENIED, ERR_PERMDENIED);
    }

    /* 路径沙盒：MCP 传 { "path": "/etc/passwd" } 在这里被挡住。 */
    if (route.flags & PC_TOOL_PATH_SANDBOX) {
        char path[PC_MAX_PATH];
        rc = json_get_string(args_json ? args_json : "{}", "path",
                             path, sizeof(path));
        if (rc != PC_OK) {
            pc_leave_call(p);
            pc_write_error(out, outcap, PC_ERR_BADARG, tool);
            return pc_fail(PC_ERR_BADARG, ERR_BADARG);
        }
        if (!pc_path_in_sandbox(root, path)) {
            pc_leave_call(p);
            pc_write_error(out, outcap, PC_ERR_SANDBOX, tool);
            return pc_fail(PC_ERR_SANDBOX, ERR_PERMDENIED);
        }
    }

    /* 用户指针：插件不能把伪造地址塞进内核。 */
    if (route.flags & PC_TOOL_USER_PTR) {
        int64_t vptr = 0;
        int64_t vlen = 0;
        if (json_get_int64(args_json ? args_json : "{}", "ptr", &vptr) != PC_OK ||
            json_get_int64(args_json ? args_json : "{}", "len", &vlen) != PC_OK ||
            vptr <= 0 || vlen <= 0 || vlen > 0xFFFFFFFFLL) {
            pc_leave_call(p);
            pc_write_error(out, outcap, PC_ERR_BADARG, tool);
            return pc_fail(PC_ERR_BADARG, ERR_BADARG);
        }
        const void* uptr = (const void*)(uintptr_t)(uint64_t)vptr;
        if (is_valid_user_ptr(p, uptr, (uint32_t)vlen, PC_PROT_READ) != 1) {
            pc_leave_call(p);
            pc_write_error(out, outcap, PC_ERR_DENIED, tool);
            return pc_fail(PC_ERR_DENIED, ERR_PERMDENIED);
        }
    }

    /* 锁外调用；插件可以安全地再次进入 dispatch/lookup。 */
    int frc = route.fn(h, args_json ? args_json : "{}", out, outcap);
    pc_leave_call(p);

    if (out && outcap) out[outcap - 1u] = 0;
    if (frc < 0) {
        pc_write_error(out, outcap, frc, tool);
        return pc_fail(frc, pc_os_from(frc));
    }
    return pc_ok();
}
/* =====================================================================
 *  模块五：内核防线自测
 * ===================================================================== */
#if defined(NEX_PC_HOST_TEST) && !defined(_WIN32)
#  include <pthread.h>
#endif

static int g_cs_pass;
static int g_cs_fail;

static void cs_emit_int(int64_t v)
{
    char t[24];
    int  k = 0;
    int  neg = (v < 0);
    uint64_t mag = neg ? ((uint64_t)(-(v + 1)) + 1ULL) : (uint64_t)v;
    if (mag == 0) { pc_emit("0"); return; }
    while (mag > 0 && k < (int)sizeof(t)) {
        t[k++] = (char)('0' + (uint32_t)(mag % 10ULL));
        mag /= 10ULL;
    }
    if (neg) pc_emit("-");
    while (k > 0) {
        char c[2];
        c[0] = t[--k];
        c[1] = 0;
        pc_emit(c);
    }
}

static void cs_emit_hex(uint64_t v)
{
    static const char* hexd = "0123456789abcdef";
    char t[18];
    int  k = 0;
    if (v == 0) { pc_emit("0x0"); return; }
    while (v && k < 16) {
        t[k++] = hexd[(uint32_t)(v & 0x0FULL)];
        v >>= 4;
    }
    pc_emit("0x");
    while (k > 0) {
        char c[2];
        c[0] = t[--k];
        c[1] = 0;
        pc_emit(c);
    }
}

static void cs_check(int cond, const char* name)
{
    if (cond) { g_cs_pass++; pc_emit("[SELFTEST] PASS  "); }
    else      { g_cs_fail++; pc_emit("[SELFTEST] FAIL  "); }
    pc_emit(name);
    pc_emit("\n");
}

#define CS_MAX_MAN 160

static char       cs_ids[CS_MAX_MAN][PC_MAX_ID];
static PcManifest cs_man[CS_MAX_MAN];
static uint8_t    cs_sandbox[PC_MIN_SANDBOX];
static uint8_t    cs_extra_page[PC_MIN_SANDBOX];

static void cs_make_id(char* dst, uint32_t cap, const char* prefix, int n)
{
    uint32_t o = 0;
    while (prefix && *prefix && o + 1u < cap) dst[o++] = *prefix++;

    char t[12];
    int  k = 0;
    if (n == 0) t[k++] = '0';
    while (n > 0 && k < (int)sizeof(t)) {
        t[k++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (k > 0 && o + 1u < cap) dst[o++] = t[--k];
    dst[o] = 0;
}

static void cs_u64_dec(uint64_t v, char* out, uint32_t cap)
{
    char t[24];
    uint32_t k = 0;
    if (v == 0) t[k++] = '0';
    while (v > 0 && k < sizeof(t)) {
        t[k++] = (char)('0' + (uint32_t)(v % 10ULL));
        v /= 10ULL;
    }
    uint32_t o = 0;
    while (k > 0 && o + 1u < cap) out[o++] = t[--k];
    out[o] = 0;
}

static void cs_manifest(PcManifest* m, const char* id,
                        uint32_t caps, uint32_t quota)
{
    safe_memset(m, 0, (uint32_t)sizeof(*m));
    m->id             = id;
    m->sandbox_root   = "/";              /* 恶意申请，必须被忽略 */
    m->sandbox_base   = (uint64_t)(uintptr_t)cs_sandbox;
    m->sandbox_size   = PC_MIN_SANDBOX;
    m->capabilities   = caps;
    m->quota          = quota;
    m->abi_version    = 1;
}

static int cs_echo_fn(uint32_t h, const char* args, char* out, uint32_t outcap)
{
    (void)h; (void)args;
    int rc = safe_strcpy(out, outcap, "{\"ok\":true}");
    return rc == PC_OK ? PC_OK : PC_ERR_OVERFLOW;
}

/* 重入测试：外层调用已经把 inflight 拉到配额，内层必须被 BUSY 拒绝。 */
static int cs_reenter_fn(uint32_t h, const char* args, char* out, uint32_t outcap)
{
    (void)args; (void)out; (void)outcap;
    char tmp[128];
    return dispatch_mcp(h, "cs.reenter", "{}", tmp, sizeof(tmp));
}

#if defined(NEX_PC_HOST_TEST) && !defined(_WIN32)
struct CsThreadArg {
    int      first;
    int      count;
    int      fail;
    uint32_t handles[16];
};

static void* cs_thread_main(void* vp)
{
    CsThreadArg* a = (CsThreadArg*)vp;
    for (int i = 0; i < a->count; i++) {
        uint32_t h = 0;
        int rc = load_plugin(&cs_man[a->first + i], 0, &h);
        if (rc != PC_OK || h == 0) {
            a->fail++;
        } else {
            a->handles[i] = h;
        }
    }
    return NULL;
}
#endif

int run_core_selftest(void)
{
    g_cs_pass = 0;
    g_cs_fail = 0;

    pc_core_init();

    pc_emit("\n===== NexOS plugin_core selftest =====\n");

    /* ---------------------------------------------------------------
     * 场景 A：伪造指针
     * ------------------------------------------------------------- */
    pc_emit("\n--- 场景 A：恶意插件传入伪造指针 ---\n");

    cs_manifest(&cs_man[0], "audit", PC_CAP_FS_READ | PC_CAP_FS_WRITE |
                               PC_CAP_SYSINFO, 10);
    uint32_t h1 = 0;
    int rc = load_plugin(&cs_man[0], 0 /* unsigned */, &h1);
    cs_check(rc == PC_OK && h1 != 0, "unsigned plugin loads");

    PcPlugin snap;
    safe_memset(&snap, 0, (uint32_t)sizeof(snap));
    rc = plugin_snapshot(h1, &snap);
    cs_check(rc == PC_OK, "plugin_snapshot resolves handle O(1)");

    cs_check((snap.caps & PC_CAP_FS_WRITE) == 0,
             "unsigned plugin: CAP_FS_WRITE stripped");
    cs_check(safe_streq(snap.sandbox_root, "/sandbox/audit"),
             "unsigned plugin: root forced to /sandbox/audit");
    cs_check(snap.page_count == 1 &&
             snap.pages[0].prot == PC_PROT_RW &&
             snap.pages[0].base == (uint64_t)(uintptr_t)cs_sandbox,
             "unsigned plugin: exactly one RW sandbox page");
    cs_check((snap.pages[0].prot & PC_PROT_EXEC) == 0,
             "unsigned plugin: no EXEC page granted");

    cs_check(is_valid_user_ptr(&snap, cs_sandbox + 64, 128, PC_PROT_READ) == 1,
             "valid sandbox pointer accepted");
    cs_check(is_valid_user_ptr(&snap, cs_sandbox + 64, 128, PC_PROT_WRITE) == 1,
             "valid sandbox WRITE pointer accepted");

    const void* forged = (const void*)(uintptr_t)0xDEADBEEF00000000ULL;
    rc = is_valid_user_ptr(&snap, forged, 64, PC_PROT_READ);
    int forged_errno = osapi_get_last_error();
    cs_check(rc == PC_ERR_DENIED && forged_errno == ERR_PERMDENIED,
             "forged 0xDEADBEEF00000000 rejected");

    rc = is_valid_user_ptr(&snap, cs_sandbox + PC_MIN_SANDBOX - 8u, 64,
                           PC_PROT_READ);
    cs_check(rc == PC_ERR_DENIED, "pointer crossing sandbox end rejected");

    rc = is_valid_user_ptr(&snap, NULL, 64, PC_PROT_READ);
    cs_check(rc == PC_ERR_DENIED, "NULL user pointer rejected");

    const void* wrapped = (const void*)(uintptr_t)(PC_USER_PTR_MAX - 8ULL);
    rc = is_valid_user_ptr(&snap, wrapped, 64, PC_PROT_READ);
    cs_check(rc == PC_ERR_DENIED, "wrapping pointer (end < start) rejected");

    rc = is_valid_user_ptr(&snap, (const void*)&snap, 16, PC_PROT_READ);
    cs_check(rc == PC_ERR_DENIED, "pointer outside page table rejected");

    rc = is_valid_user_ptr(&snap, cs_sandbox + 64, 16, PC_PROT_EXEC);
    cs_check(rc == PC_ERR_DENIED, "EXEC on RW-only page rejected");

    char copy[32];
    rc = safe_copy_from_user(&snap, copy, sizeof(copy), forged, 16);
    cs_check(rc == PC_ERR_DENIED, "safe_copy_from_user(forged) denied");

    const char* probe = (const char*)(cs_sandbox + 128);
    rc = safe_copy_from_user(&snap, copy, sizeof(copy), probe, 8);
    cs_check(rc == PC_OK && safe_strncmp(copy, probe, 8) == 0,
             "safe_copy_from_user(valid) copies");

    pc_emit("[SELFTEST] 场景 A: 伪造指针被 is_valid_user_ptr 拦截: ");
    cs_emit_hex((uint64_t)(uintptr_t)forged);
    pc_emit(" -> errno=");
    cs_emit_int((int64_t)forged_errno);
    pc_emit("\n");

    /* 模块一补充：截断必须 NUL 结尾且不越界。 */
    char small[8];
    rc = safe_strcpy(small, sizeof(small),
                     "0123456789abcdef");
    cs_check(rc == PC_ERR_OVERFLOW &&
             safe_strlen(small, sizeof(small)) == sizeof(small) - 1u,
             "safe_strcpy truncates and NUL-terminates");
    rc = safe_strcpy(NULL, 8, "x");
    cs_check(rc == PC_ERR_BADARG, "safe_strcpy(NULL) rejected");

    /* ---------------------------------------------------------------
     * 场景 B：MCP 传入 /etc/passwd
     * ------------------------------------------------------------- */
    pc_emit("\n--- 场景 B：MCP 传入 {\"path\":\"/etc/passwd\"} ---\n");

    rc = register_tool(h1, "fs.read", cs_echo_fn, PC_CAP_FS_READ,
                       PC_TOOL_PATH_SANDBOX);
    cs_check(rc == PC_OK, "register fs.read (PATH_SANDBOX)");

    rc = register_tool(h1, "fs.write", cs_echo_fn, PC_CAP_FS_WRITE,
                       PC_TOOL_PATH_SANDBOX);
    cs_check(rc == PC_ERR_DENIED,
             "register fs.write denied (unsigned lost CAP_FS_WRITE)");

    char out[512];
    rc = dispatch_mcp(h1, "fs.read", "{\"path\":\"/etc/passwd\"}",
                      out, sizeof(out));
    cs_check(rc == PC_ERR_SANDBOX && out[0] == '{',
             "dispatch /etc/passwd -> PC_ERR_SANDBOX + JSON error");
    pc_emit("[SELFTEST] 场景 B: /etc/passwd 被路径校验拦截: ");
    pc_emit(out);
    pc_emit("\n");

    rc = dispatch_mcp(h1, "fs.read",
                      "{\"path\":\"/sandbox/audit/../../etc/shadow\"}",
                      out, sizeof(out));
    cs_check(rc == PC_ERR_SANDBOX, "dispatch traversal ../../etc/shadow denied");

    rc = dispatch_mcp(h1, "fs.read", "{\"path\":\"/sandbox/audit2/x\"}",
                      out, sizeof(out));
    cs_check(rc == PC_ERR_SANDBOX, "dispatch sibling prefix /sandbox/audit2 denied");

    /* JSON 里要写成 \\，解码后才是单个反斜杠；路径规范化必须拒绝它。 */
    rc = dispatch_mcp(h1, "fs.read", "{\"path\":\"/sandbox/audit\\\\..\\\\x\"}",
                      out, sizeof(out));
    cs_check(rc == PC_ERR_SANDBOX, "dispatch backslash path denied");

    rc = dispatch_mcp(h1, "fs.read", "{}", out, sizeof(out));
    cs_check(rc == PC_ERR_BADARG, "dispatch missing path -> BADARG");

    rc = dispatch_mcp(h1, "fs.read",
                      "{\"path\":\"/sandbox/audit/notes.txt\"}",
                      out, sizeof(out));
    cs_check(rc == PC_OK && out[0] == '{', "dispatch inside sandbox allowed");

    /* 模块四：MCP 参数里的用户指针也要过影子页表。 */
    rc = register_tool(h1, "mem.read", cs_echo_fn, 0, PC_TOOL_USER_PTR);
    cs_check(rc == PC_OK, "register mem.read (USER_PTR)");

    char pargs[160];
    char num[24];
    uint64_t good_ptr = (uint64_t)(uintptr_t)(cs_sandbox + 256);
    cs_u64_dec(good_ptr, num, sizeof(num));
    safe_strcpy(pargs, sizeof(pargs), "{\"ptr\":");
    { uint32_t o = safe_strlen(pargs, sizeof(pargs));
      safe_strncpy(pargs + o, sizeof(pargs) - o, num, sizeof(pargs) - o - 1u); }
    { uint32_t o = safe_strlen(pargs, sizeof(pargs));
      safe_strncpy(pargs + o, sizeof(pargs) - o, ",\"len\":16}", sizeof(pargs) - o - 1u); }

    rc = dispatch_mcp(h1, "mem.read", pargs, out, sizeof(out));
    cs_check(rc == PC_OK, "dispatch valid user pointer allowed");

    safe_strcpy(pargs, sizeof(pargs), "{\"ptr\":123145459761152,\"len\":16}");
    rc = dispatch_mcp(h1, "mem.read", pargs, out, sizeof(out));
    cs_check(rc == PC_ERR_DENIED,
             "dispatch forged user pointer -> PC_ERR_DENIED");

    safe_strcpy(pargs, sizeof(pargs), "{\"ptr\":0,\"len\":16}");
    rc = dispatch_mcp(h1, "mem.read", pargs, out, sizeof(out));
    cs_check(rc == PC_ERR_BADARG, "dispatch NULL user pointer -> BADARG");

    /* 模块二附加：签名插件可以拿到额外页（含 EXEC）。 */
    PagePermission extra;
    extra.base  = (uint64_t)(uintptr_t)cs_extra_page;
    extra.size  = PC_MIN_SANDBOX;
    extra.prot  = PC_PROT_READ | PC_PROT_EXEC;
    extra.owner = 0;

    PcManifest msigned;
    cs_manifest(&msigned, "signed.app", PC_CAP_FS_READ | PC_CAP_SYSINFO, 10);
    msigned.sandbox_root    = "/sandbox/signed.app";
    msigned.extra_pages     = &extra;
    msigned.extra_page_count = 1;

    uint32_t hs = 0;
    rc = load_plugin(&msigned, 1 /* signed */, &hs);
    cs_check(rc == PC_OK && hs != 0, "signed plugin loads");
    PcPlugin ss;
    safe_memset(&ss, 0, sizeof(ss));
    rc = plugin_snapshot(hs, &ss);
    cs_check(rc == PC_OK && ss.page_count == 2,
             "signed plugin gets sandbox + extra page");
    cs_check((ss.pages[1].prot & PC_PROT_EXEC) != 0,
             "signed plugin keeps EXEC on extra page");
    cs_check(is_valid_user_ptr(&ss, cs_extra_page + 32, 16, PC_PROT_EXEC) == 1,
             "signed plugin EXEC pointer accepted");
    unload_plugin(hs);

    /* 模块三附加：200 条路由，删一半后其余仍 O(1) 命中（验证回移删除）。 */
    pc_emit("[SELFTEST] router churn: 200 routes, delete 100 ...\n");
    char tname[200][PC_MAX_TOOL];
    for (int i = 0; i < 200; i++) {
        cs_make_id(tname[i], PC_MAX_TOOL, "churn.t", i);
        rc = register_tool(h1, tname[i], cs_echo_fn, 0, 0);
        if (rc != PC_OK) break;
    }
    cs_check(rc == PC_OK, "200 routes registered");
    for (int i = 0; i < 200; i += 2) {
        rc = unregister_tool(h1, tname[i]);
        if (rc != PC_OK) break;
    }
    cs_check(rc == PC_OK, "100 routes unregistered");

    McpRoute rr;
    int churn_ok = 1;
    for (int i = 0; i < 200; i++) {
        int lrc = router_lookup(&g_router, tname[i], &rr);
        if (i % 2 == 0) { if (lrc != PC_ERR_NOENT) churn_ok = 0; }
        else            { if (lrc != PC_OK || rr.handle != h1) churn_ok = 0; }
    }
    cs_check(churn_ok, "deleted routes miss, surviving routes still hit");

    /* JSON 解析器本身的对抗用例。 */
    char jbuf[64];
    rc = json_get_string("{\"path\":\"/sandbox/audit/a\\u002etxt\"}",
                         "path", jbuf, sizeof(jbuf));
    cs_check(rc == PC_OK && safe_streq(jbuf, "/sandbox/audit/a.txt"),
             "JSON \\u escape decoded");
    rc = json_get_string("{\"path\":\"a\\u0000b\"}", "path",
                         jbuf, sizeof(jbuf));
    cs_check(rc == PC_ERR_BADARG, "JSON \\u0000 rejected (no truncation)");
    rc = json_get_string("{\"name\":\"\\u0070ath\",\"path\":\"x\"}",
                         "path", jbuf, sizeof(jbuf));
    cs_check(rc == PC_OK && safe_streq(jbuf, "x"),
             "escaped key cannot spoof plain key");
    int64_t iv = 0;
    rc = json_get_int64("{\"n\":-9223372036854775808}", "n", &iv);
    cs_check(rc == PC_OK && iv < 0, "JSON int64 min parsed");
    rc = json_get_int64("{\"n\":9223372036854775808}", "n", &iv);
    cs_check(rc == PC_ERR_OVERFLOW, "JSON int64 overflow rejected");

    /* ---------------------------------------------------------------
     * 场景 C：并发 load_plugin
     * ------------------------------------------------------------- */
    pc_emit("\n--- 场景 C：多线程并发 load_plugin ---\n");

    rc = unload_plugin(h1);
    cs_check(rc == PC_OK, "audit plugin unloaded before concurrency test");
    McpRoute gone;
    cs_check(router_lookup(&g_router, "churn.t1", &gone) == PC_ERR_NOENT,
             "unload removed the plugin's whole route chain");

    const int nthreads = 8;
    const int per_thread = 8;
    const int ntotal = nthreads * per_thread;

    for (int i = 0; i < ntotal; i++) {
        cs_make_id(cs_ids[i], PC_MAX_ID, "thr", i);
        cs_manifest(&cs_man[i], cs_ids[i], PC_CAP_SYSINFO, 10);
    }

    uint32_t all[64];
    safe_memset(all, 0, sizeof(all));

#if defined(NEX_PC_HOST_TEST) && !defined(_WIN32)
    pthread_t   tids[8];
    CsThreadArg args[8];
    safe_memset(args, 0, sizeof(args));
    for (int t = 0; t < nthreads; t++) {
        args[t].first = t * per_thread;
        args[t].count = per_thread;
        pthread_create(&tids[t], NULL, cs_thread_main, &args[t]);
    }
    for (int t = 0; t < nthreads; t++) pthread_join(tids[t], NULL);

    int loads_ok = 0;
    int load_fail = 0;
    for (int t = 0; t < nthreads; t++) {
        load_fail += args[t].fail;
        for (int i = 0; i < args[t].count; i++) {
            all[t * per_thread + i] = args[t].handles[i];
            if (args[t].handles[i]) loads_ok++;
        }
    }
#else
    int loads_ok = 0;
    int load_fail = 0;
    for (int i = 0; i < ntotal; i++) {
        uint32_t h = 0;
        int lrc = load_plugin(&cs_man[i], 0, &h);
        if (lrc == PC_OK && h) { all[i] = h; loads_ok++; }
        else load_fail++;
    }
#endif

    int dup = 0;
    for (int i = 0; i < ntotal; i++) {
        if (!all[i]) continue;
        for (int j = i + 1; j < ntotal; j++) {
            if (all[i] == all[j]) dup++;
        }
    }

    cs_check(load_fail == 0, "concurrent loads all succeeded");
    cs_check(loads_ok == ntotal, "all 64 handles allocated");
    cs_check(dup == 0, "no duplicate handle / slot");
    cs_check(plugin_count() == ntotal, "plugin_count == 64");

    for (int i = 0; i < ntotal; i++) {
        if (!all[i]) continue;
        PcPlugin tmp;
        if (plugin_snapshot(all[i], &tmp) != PC_OK) dup++;
    }
    cs_check(dup == 0, "every allocated handle resolves");

    pc_emit("[SELFTEST] 场景 C: ");
    cs_emit_int(nthreads);
    pc_emit(" 线程 / ");
    cs_emit_int(loads_ok);
    pc_emit(" 插件，重复槽位=");
    cs_emit_int(dup);
    pc_emit("\n");

    /* ---------------------------------------------------------------
     * 场景 D：卸载后旧句柄立刻失效
     * ------------------------------------------------------------- */
    pc_emit("\n--- 场景 D：卸载后旧句柄失效 ---\n");

    uint32_t oldh = all[0];
    rc = register_tool(oldh, "cs.echo", cs_echo_fn, 0, 0);
    cs_check(rc == PC_OK, "register cs.echo before unload");

    rc = dispatch_mcp(oldh, "cs.echo", "{}", out, sizeof(out));
    cs_check(rc == PC_OK, "dispatch before unload succeeds");

    rc = unload_plugin(oldh);
    cs_check(rc == PC_OK, "unload_plugin returns OK");

    rc = dispatch_mcp(oldh, "cs.echo", "{}", out, sizeof(out));
    cs_check(rc == PC_ERR_STALE, "old handle dispatch -> PC_ERR_STALE");

    cs_check(plugin_lookup(oldh) == NULL, "old handle lookup -> NULL");

    McpRoute dead_route;
    rc = router_lookup(&g_router, "cs.echo", &dead_route);
    cs_check(rc == PC_ERR_NOENT, "route removed by unload");

    /* 槽位被复用，但代际必须变，旧句柄仍然失效。 */
    cs_make_id(cs_ids[64], PC_MAX_ID, "after", 1);
    cs_manifest(&cs_man[64], cs_ids[64], PC_CAP_SYSINFO, 10);
    uint32_t newh = 0;
    rc = load_plugin(&cs_man[64], 0, &newh);
    cs_check(rc == PC_OK && newh != 0 && newh != oldh,
             "reused slot gets a different generation");
    rc = dispatch_mcp(oldh, "cs.echo", "{}", out, sizeof(out));
    cs_check(rc == PC_ERR_STALE, "old handle still stale after slot reuse");
    unload_plugin(newh);

    pc_emit("[SELFTEST] 场景 D: 旧句柄 ");
    cs_emit_hex(oldh);
    pc_emit(" -> ");
    cs_emit_int(PC_ERR_STALE);
    pc_emit(" (stale), 新句柄 ");
    cs_emit_hex(newh);
    pc_emit("\n");

    for (int i = 1; i < ntotal; i++) {
        if (all[i]) unload_plugin(all[i]);
    }
    cs_check(plugin_count() == 0, "all concurrency-test plugins unloaded");

    /* ---------------------------------------------------------------
     * 附加：并发配额
     * ------------------------------------------------------------- */
    cs_make_id(cs_ids[65], PC_MAX_ID, "quota", 1);
    cs_manifest(&cs_man[65], cs_ids[65], PC_CAP_SYSINFO, 1 /* quota */);
    uint32_t hq = 0;
    rc = load_plugin(&cs_man[65], 0, &hq);
    cs_check(rc == PC_OK, "quota plugin loads");

    rc = register_tool(hq, "cs.reenter", cs_reenter_fn, 0, 0);
    cs_check(rc == PC_OK, "register reentrant tool");

    rc = dispatch_mcp(hq, "cs.reenter", "{}", out, sizeof(out));
    cs_check(rc == PC_ERR_BUSY, "quota=1 reentrant call -> PC_ERR_BUSY");

    rc = unload_plugin(hq);
    cs_check(rc == PC_OK, "quota plugin unloads after inflight drops");

    /* ---------------------------------------------------------------
     * 附加：OOM 熔断
     * ------------------------------------------------------------- */
    pc_emit("\n--- 附加：OOM 熔断 ---\n");

    for (int i = 0; i < PC_MAX_PLUGINS; i++) {
        cs_make_id(cs_ids[i], PC_MAX_ID, "bulk", i);
        cs_manifest(&cs_man[i], cs_ids[i], PC_CAP_SYSINFO, 10);
    }
    uint32_t bulk[PC_MAX_PLUGINS];
    safe_memset(bulk, 0, sizeof(bulk));
    int bulk_ok = 0;
    for (int i = 0; i < PC_MAX_PLUGINS; i++) {
        if (load_plugin(&cs_man[i], 0, &bulk[i]) == PC_OK) bulk_ok++;
    }
    cs_check(bulk_ok == PC_MAX_PLUGINS && plugin_count() == PC_MAX_PLUGINS,
             "128 slots filled");

    cs_make_id(cs_ids[PC_MAX_PLUGINS], PC_MAX_ID, "bulk", PC_MAX_PLUGINS);
    cs_manifest(&cs_man[PC_MAX_PLUGINS], cs_ids[PC_MAX_PLUGINS],
                PC_CAP_SYSINFO, 10);
    uint32_t over = 0;
    rc = load_plugin(&cs_man[PC_MAX_PLUGINS], 0, &over);
    cs_check(rc == PC_ERR_NOSPACE && pc_oom_tripped() == 1,
             "129th load -> PC_ERR_NOSPACE + OOM fuse");

    unload_plugin(bulk[0]);
    rc = load_plugin(&cs_man[PC_MAX_PLUGINS], 0, &over);
    cs_check(rc == PC_ERR_NOSPACE,
             "fuse latched: load still denied while slot is free");

    pc_clear_oom_fuse();
    rc = load_plugin(&cs_man[PC_MAX_PLUGINS], 0, &over);
    cs_check(rc == PC_OK, "after pc_clear_oom_fuse() load succeeds");

    for (int i = 1; i < PC_MAX_PLUGINS; i++) {
        if (bulk[i]) unload_plugin(bulk[i]);
    }
    if (over) unload_plugin(over);
    cs_check(plugin_count() == 0, "all bulk plugins unloaded");
    cs_check(pc_oom_tripped() == 0, "fuse cleared");

    pc_emit("\n===== plugin_core selftest: ");
    cs_emit_int(g_cs_pass);
    pc_emit(" passed, ");
    cs_emit_int(g_cs_fail);
    pc_emit(" failed =====\n");

    return g_cs_fail;
}
