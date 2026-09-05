// =====================================================================
//  winloader.cpp - Windows 可执行文件加载器 (MiniOS 内核版)
// ---------------------------------------------------------------------
//  Freestanding 适配: 无 libc, 通过回调读文件/输出。
//  支持:
//    - .bat / .cmd  批处理脚本引擎 (echo/set/goto/if exist/call/%VAR%)
//    - .ps1 / .psm1  简化 PowerShell 引擎 (Write-Host/变量/基础 if)
//    - .exe/.dll/.sys PE 检测 (识别 MZ+PE 头并报告, 内核不执行)
//    - .com          DOS COM 检测
// =====================================================================

#include <stdint.h>

// ---- 回调注入 (kernel.cpp 提供) ----
typedef int  (*WL_FileReader)(const char* name, uint8_t* buf, int bufsize);
typedef void (*WL_Writer)(const char* text);
static WL_FileReader g_reader = 0;
static WL_Writer     g_writer = 0;

extern "C" {
void winloader_init(WL_FileReader reader, WL_Writer writer) {
    g_reader = reader;
    g_writer = writer;
}
}

// ---- 基础字符串 (freestanding, 无 libc) ----
static int wl_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static int wl_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static int wl_strnicmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 1;
        if (!ca) return 0;
    }
    return 0;
}
static const char* wl_strstr(const char* h, const char* n) {
    if (!h || !n || !*n) return 0;
    for (; *h; h++) {
        const char* p = h, *q = n;
        while (*p && *q && *p == *q) { p++; q++; }
        if (!*q) return h;
    }
    return 0;
}
static void wl_strcpy(char* d, const char* s) { while ((*d++ = *s++)); }
static void wl_strncpy(char* d, const char* s, int n) {
    int i = 0;
    while (i < n - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static const char* wl_strchr(const char* s, char c) {
    while (*s) { if (*s == c) return s; s++; }
    return 0;
}
static void wl_trim(char* s) {
    char* e = s + wl_strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char* st = s;
    while (*st == ' ' || *st == '\t') st++;
    if (st != s) { int i = 0; while (st[i]) { s[i] = st[i]; i++; } s[i] = 0; }
}

// ---- 输出 ----
static void wl_print(const char* s) { if (g_writer) g_writer(s); }
static void wl_println(const char* s) { wl_print(s); wl_print("\n"); }
static void wl_print_num(long v) {
    char buf[16];
    int i = 15;
    buf[i] = 0;
    bool neg = v < 0;
    if (neg) v = -v;
    if (v == 0) buf[--i] = '0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (int)(v % 10); v /= 10; }
    if (neg) buf[--i] = '-';
    wl_print(buf + i);
}

// =====================================================================
//  文件类型检测
// =====================================================================
enum WFileType {
    WFT_UNKNOWN, WFT_PE, WFT_BAT, WFT_CMD, WFT_PS1, WFT_COM
};

static WFileType wl_detect(const uint8_t* data, int size, const char* filename) {
    const char* ext = 0;
    for (const char* p = filename; *p; p++)
        if (*p == '.') ext = p;
    if (ext) {
        if (wl_strnicmp(ext, ".exe", 4) == 0 || wl_strnicmp(ext, ".dll", 4) == 0 ||
            wl_strnicmp(ext, ".sys", 4) == 0 || wl_strnicmp(ext, ".drv", 4) == 0)
            return WFT_PE;
        if (wl_strnicmp(ext, ".bat", 4) == 0) return WFT_BAT;
        if (wl_strnicmp(ext, ".cmd", 4) == 0) return WFT_CMD;
        if (wl_strnicmp(ext, ".ps1", 4) == 0 || wl_strnicmp(ext, ".psm1", 4) == 0 ||
            wl_strnicmp(ext, ".psd1", 4) == 0) return WFT_PS1;
        if (wl_strnicmp(ext, ".com", 4) == 0) return WFT_COM;
    }
    // 魔数
    if (data && size >= 2) {
        if (data[0] == 'M' && data[1] == 'Z') return WFT_PE;
    }
    // 文本检测
    if (data) {
        bool text = true;
        for (int i = 0; i < size && i < 512; i++) {
            if (data[i] < 0x20 && data[i] != '\n' && data[i] != '\r' &&
                data[i] != '\t' && data[i] != '\b') { text = false; break; }
        }
        if (text) {
            const char* d = (const char*)data;
            if (wl_strstr(d, "@echo") || wl_strstr(d, "echo ") || wl_strstr(d, "goto "))
                return WFT_BAT;
            if (wl_strstr(d, "Write-Host") || wl_strstr(d, "param(") || wl_strstr(d, "$args"))
                return WFT_PS1;
        }
    }
    return WFT_UNKNOWN;
}

// =====================================================================
//  批处理引擎 (BAT/CMD)
// =====================================================================
struct WLEnvVar { char name[32]; char value[256]; };
static WLEnvVar g_env[32];
static int g_env_count = 0;

static void wl_env_set(const char* name, const char* value) {
    for (int i = 0; i < g_env_count; i++)
        if (wl_strcmp(g_env[i].name, name) == 0) { wl_strncpy(g_env[i].value, value, 256); return; }
    if (g_env_count < 32) {
        wl_strncpy(g_env[g_env_count].name, name, 32);
        wl_strncpy(g_env[g_env_count].value, value, 256);
        g_env_count++;
    }
}
static const char* wl_env_get(const char* name) {
    for (int i = 0; i < g_env_count; i++)
        if (wl_strcmp(g_env[i].name, name) == 0) return g_env[i].value;
    return 0;
}
static void wl_env_expand(char* out, int outsize, const char* in) {
    int o = 0;
    for (int i = 0; in[i] && o < outsize - 1; i++) {
        if (in[i] == '%') {
            char var[32];
            int v = 0;
            int j = i + 1;
            while (in[j] && in[j] != '%' && v < 31) var[v++] = in[j++];
            var[v] = 0;
            if (in[j] == '%' && v > 0) {
                const char* val = wl_env_get(var);
                if (val) { int k = 0; while (val[k] && o < outsize - 2) out[o++] = val[k++]; }
                i = j;
            } else { out[o++] = in[i]; }
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = 0;
}

// BAT 执行器 (递归支持 goto)
static int wl_exec_bat_lines(const uint8_t* data, int size, int start_line, int* last_line);

static void wl_bat_exec_command(const char* cmd, const uint8_t* data, int size, int* cur) {
    char expanded[512];
    wl_env_expand(expanded, sizeof(expanded), cmd);
    const char* c = expanded;
    while (*c == ' ') c++;
    wl_print(c);
    wl_print("\n");
}

static int wl_exec_bat_lines(const uint8_t* data, int size, int start_line, int* last_line) {
    int line = 0;
    int pos = 0;
    while (pos < size && line < start_line) {
        while (pos < size && data[pos] != '\n') pos++;
        if (pos < size) pos++;
        line++;
    }
    bool echo_on = true;
    wl_env_set("0", "batch");
    wl_env_set("1", "");
    wl_env_set("2", "");
    wl_env_set("3", "");
    wl_env_set("PATH", "/bin:/usr/bin");
    wl_env_set("COMSPEC", "minios-shell");
    wl_env_set("OS", "MiniOS");

    while (pos < size) {
        char l[512];
        int n = 0;
        while (pos < size && data[pos] != '\n' && n < 511) l[n++] = (char)data[pos++];
        if (pos < size) pos++;
        l[n] = 0;
        for (int i = 0; l[i]; i++) if (l[i] == '\r') l[i] = 0;
        wl_trim(l);
        if (l[0] == 0) continue;

        if (wl_strnicmp(l, "@echo", 5) == 0) {
            const char* r = l + 5;
            while (*r == ' ') r++;
            if (wl_strnicmp(r, "off", 3) == 0) echo_on = false;
            else if (wl_strnicmp(r, "on", 2) == 0) echo_on = true;
            continue;
        }
        if (wl_strnicmp(l, "rem ", 4) == 0 || wl_strnicmp(l, "REM", 3) == 0 ||
            (l[0] == ':' && l[1] == ':')) continue;
        if (l[0] == ':') continue;
        if (wl_strnicmp(l, "echo ", 5) == 0) {
            const char* r = l + 5;
            if (*r == '.') r++;
            char out[512];
            wl_env_expand(out, sizeof(out), r);
            if (echo_on) wl_println(out);
            continue;
        }
        if (wl_strnicmp(l, "set ", 4) == 0) {
            const char* r = l + 4;
            const char* eq = wl_strchr(r, '=');
            if (eq) {
                char name[32], val[256];
                int vn = 0, vv = 0;
                while (r < eq && vn < 31) name[vn++] = *r++;
                name[vn] = 0;
                wl_trim(name);
                r = eq + 1;
                while (*r && vv < 255) val[vv++] = *r++;
                val[vv] = 0;
                wl_trim(val);
                wl_env_set(name, val);
            }
            continue;
        }
        if (wl_strnicmp(l, "cd ", 3) == 0) {
            const char* r = l + 3;
            while (*r == ' ') r++;
            wl_env_set("CD", r);
            wl_println(r);
            continue;
        }
        if (wl_strnicmp(l, "goto ", 5) == 0) {
            const char* r = l + 5;
            while (*r == ' ') r++;
            if (*r == ':') r++;
            int lnum = 0, p2 = 0;
            bool found = false;
            while (p2 < size && !found) {
                char ll[512];
                int nn = 0;
                while (p2 < size && data[p2] != '\n' && nn < 511) ll[nn++] = (char)data[p2++];
                if (p2 < size) p2++;
                ll[nn] = 0;
                for (int i = 0; ll[i]; i++) if (ll[i] == '\r') ll[i] = 0;
                wl_trim(ll);
                if (ll[0] == ':' && wl_strnicmp(ll + 1, r, wl_strlen(r)) == 0) {
                    if (wl_strlen(ll) - 1 == wl_strlen(r)) { found = true; break; }
                }
                lnum++;
            }
            if (found) {
                int dummy = 0;
                return wl_exec_bat_lines(data, size, lnum + 1, &dummy);
            }
            wl_println("Label not found");
            return 1;
        }
        if (wl_strnicmp(l, "if ", 3) == 0) {
            const char* r = l + 3;
            while (*r == ' ') r++;
            if (wl_strnicmp(r, "exist ", 6) == 0) {
                const char* f = r + 6;
                while (*f == ' ') f++;
                const char* sp = wl_strchr(f, ' ');
                char fname[128];
                if (sp) { wl_strncpy(fname, f, (int)(sp - f) + 1); }
                else wl_strcpy(fname, f);
                wl_trim(fname);
                bool exists = false;
                if (g_reader) {
                    uint8_t tmp[4];
                    if (g_reader(fname, tmp, 4) >= 0) exists = true;
                }
                if (sp && exists) {
                    const char* cmd2 = sp + 1;
                    while (*cmd2 == ' ') cmd2++;
                    wl_bat_exec_command(cmd2, data, size, &line);
                } else if (!sp) {
                    wl_println(exists ? "EXISTS" : "NOT FOUND");
                }
            }
            continue;
        }
        if (echo_on) {
            wl_print(l);
            wl_println(" - unknown command (MiniOS bat engine)");
        }
    }
    if (last_line) *last_line = line;
    return 0;
}

static int wl_exec_bat(const uint8_t* data, int size) {
    wl_println("== MiniOS Batch Engine ==");
    int dummy = 0;
    return wl_exec_bat_lines(data, size, 0, &dummy);
}

// =====================================================================
//  简化 PowerShell 引擎 (PS1)
// =====================================================================
struct WLPsVar { char name[32]; char value[256]; };
static WLPsVar g_psvars[32];
static int g_psvar_count = 0;

static void wl_ps_set(const char* name, const char* value) {
    for (int i = 0; i < g_psvar_count; i++)
        if (wl_strcmp(g_psvars[i].name, name) == 0) { wl_strncpy(g_psvars[i].value, value, 256); return; }
    if (g_psvar_count < 32) {
        wl_strncpy(g_psvars[g_psvar_count].name, name, 32);
        wl_strncpy(g_psvars[g_psvar_count].value, value, 256);
        g_psvar_count++;
    }
}
static const char* wl_ps_get(const char* name) {
    for (int i = 0; i < g_psvar_count; i++)
        if (wl_strcmp(g_psvars[i].name, name) == 0) return g_psvars[i].value;
    return 0;
}

static void wl_ps_expand(char* out, int outsize, const char* in) {
    int o = 0;
    for (int i = 0; in[i] && o < outsize - 1; i++) {
        if (in[i] == '$') {
            char vn[32];
            int v = 0, j = i + 1;
            while (in[j] && in[j] != ' ' && in[j] != ')' && in[j] != ';' &&
                   in[j] != '"' && in[j] != '\n' && in[j] != '\r' && v < 31)
                vn[v++] = in[j++];
            vn[v] = 0;
            if (v > 0) {
                const char* val = wl_ps_get(vn);
                if (val) { int k = 0; while (val[k] && o < outsize - 2) out[o++] = val[k++]; }
                i = j - 1;
            } else { out[o++] = in[i]; }
        } else { out[o++] = in[i]; }
    }
    out[o] = 0;
}

static int wl_exec_ps1(const uint8_t* data, int size) {
    wl_println("== MiniOS PowerShell Engine ==");
    wl_ps_set("PSVersion", "5.1.19041");
    wl_ps_set("PWD", "/");
    wl_ps_set("args", "");
    int pos = 0;
    while (pos < size) {
        char l[512];
        int n = 0;
        while (pos < size && data[pos] != '\n' && n < 511) l[n++] = (char)data[pos++];
        if (pos < size) pos++;
        l[n] = 0;
        for (int i = 0; l[i]; i++) if (l[i] == '\r') l[i] = 0;
        wl_trim(l);
        if (l[0] == 0 || l[0] == '#') continue;
        if (wl_strnicmp(l, "Write-Host", 10) == 0) {
            const char* r = l + 10;
            while (*r == ' ' || *r == '-') r++;
            char out[512];
            wl_ps_expand(out, sizeof(out), r);
            wl_println(out);
            continue;
        }
        if (wl_strnicmp(l, "Write-Output", 12) == 0) {
            const char* r = l + 12;
            while (*r == ' ') r++;
            char out[512];
            wl_ps_expand(out, sizeof(out), r);
            wl_println(out);
            continue;
        }
        if (l[0] == '$') {
            const char* eq = wl_strchr(l, '=');
            if (eq) {
                char vn[32], vv[256];
                int v = 1, vn2 = 0;
                while (l[v] && l[v] != '=' && vn2 < 31) vn[vn2++] = l[v++];
                vn[vn2] = 0;
                wl_trim(vn);
                const char* val = eq + 1;
                while (*val == ' ') val++;
                int vv2 = 0;
                if (*val == '"' || *val == '\'') val++;
                while (*val && *val != '"' && *val != '\'' && vv2 < 255) vv[vv2++] = *val++;
                vv[vv2] = 0;
                wl_ps_set(vn, vv);
                continue;
            }
        }
        if (wl_strnicmp(l, "if ", 3) == 0) {
            const char* eq = wl_strstr(l, " -eq ");
            if (eq) {
                char left[128], right[128];
                wl_strncpy(left, l + 3, (int)(eq - (l + 3)) + 1);
                wl_trim(left);
                const char* rp = eq + 5;
                char r2[128];
                int ri = 0;
                while (*rp && *rp != ')' && *rp != '{' && ri < 127) r2[ri++] = *rp++;
                r2[ri] = 0;
                wl_trim(r2);
                char le[256], re[256];
                wl_ps_expand(le, sizeof(le), left);
                wl_ps_expand(re, sizeof(re), r2);
                if (wl_strcmp(le, re) == 0) {
                    const char* brace = wl_strchr(l, '{');
                    if (brace) {
                        const char* cmd = brace + 1;
                        while (*cmd == ' ') cmd++;
                        char out[512];
                        wl_ps_expand(out, sizeof(out), cmd);
                        wl_println(out);
                    }
                }
            }
            continue;
        }
        wl_print("PS> ");
        wl_println(l);
    }
    return 0;
}

// =====================================================================
//  PE / COM 检测
// =====================================================================
static int wl_pe_info(const uint8_t* data, int size) {
    wl_println("== PE/COFF Executable Detected ==");
    uint16_t e_magic = data[0] | (data[1] << 8);
    if (e_magic != 0x5A4D) { wl_println("  Bad MZ magic"); return 1; }
    uint32_t e_lfanew = (uint32_t)data[0x3C] | ((uint32_t)data[0x3D] << 8) |
                        ((uint32_t)data[0x3E] << 16) | ((uint32_t)data[0x3F] << 24);
    if (e_lfanew + 4 > (uint32_t)size) { wl_println("  PE header out of range"); return 1; }
    if (data[e_lfanew] != 'P' || data[e_lfanew + 1] != 'E' ||
        data[e_lfanew + 2] != 0 || data[e_lfanew + 3] != 0) {
        wl_println("  Invalid PE signature (DOS/MZ only?)");
        return 1;
    }
    uint16_t machine = data[e_lfanew + 4] | (data[e_lfanew + 5] << 8);
    uint16_t nsects  = data[e_lfanew + 6] | (data[e_lfanew + 7] << 8);
    wl_print("  Machine: ");
    wl_print(machine == 0x14C ? "i386 (32-bit)" : (machine == 0x8664 ? "x86-64" : "unknown"));
    wl_println("");
    wl_print("  Sections: ");
    wl_print_num(nsects);
    wl_println("");
    wl_println("  Runtime: MiniOS does NOT support Windows PE execution yet.");
    return 0;
}

static int wl_com_info(int size) {
    wl_println("== DOS COM Executable Detected ==");
    wl_print("  Size: ");
    wl_print_num(size);
    wl_println(" bytes (64KB max)");
    wl_println("  Runtime: MiniOS does NOT support 8086 real-mode COM execution.");
    return 0;
}

// =====================================================================
//  统一加载器入口
// =====================================================================

// ---- 捕获输出模式 (供 GUI 窗口显示) ----
static char* g_capture_buf = 0;
static int   g_capture_pos = 0;
static int   g_capture_size = 0;

extern "C" int winloader_run(const char* filename, const char* args);  // forward decl

static void wl_capture_writer(const char* s) {
    if (g_capture_buf && s) {
        while (*s && g_capture_pos < g_capture_size - 1) {
            g_capture_buf[g_capture_pos++] = *s++;
        }
        g_capture_buf[g_capture_pos] = 0;
    }
}

// 执行文件并把 winloader 输出捕获到 out 缓冲 (GUI 窗口用)
extern "C" int winloader_capture_run(const char* filename, const char* args,
                                     char* out, int outsize) {
    if (out && outsize > 0) out[0] = 0;
    g_capture_buf = out;
    g_capture_pos = 0;
    g_capture_size = outsize;
    WL_Writer old = g_writer;
    g_writer = wl_capture_writer;
    int rc = winloader_run(filename, args);
    g_writer = old;
    g_capture_buf = 0;
    return rc;
}

extern "C" {
int winloader_run(const char* filename, const char* args) {
    if (!g_reader) { wl_println("winloader: no file reader registered"); return -1; }
    wl_print("Loading: ");
    wl_println(filename);
    if (args && args[0]) { wl_print("Args: "); wl_println(args); }

    static uint8_t wl_buf[32768];
    int rd = g_reader(filename, wl_buf, sizeof(wl_buf));
    if (rd < 0) {
        wl_println("Failed to open file (not found in SFS/MKFS)");
        return -1;
    }
    WFileType type = wl_detect(wl_buf, rd, filename);
    switch (type) {
        case WFT_BAT:
        case WFT_CMD: return wl_exec_bat(wl_buf, rd);
        case WFT_PS1: return wl_exec_ps1(wl_buf, rd);
        case WFT_PE:  return wl_pe_info(wl_buf, rd);
        case WFT_COM: return wl_com_info(rd);
        default:
            wl_println("Unsupported or unknown file type");
            return -1;
    }
}
}
