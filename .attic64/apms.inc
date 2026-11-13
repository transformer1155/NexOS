// =====================================================================
//  apms.inc -- APMS implementation, #include'd by .attic64/kernel64.cpp
//             immediately before run_command().
// ---------------------------------------------------------------------
//  Kept in a separate file so the 408 KB kernel source stays navigable;
//  it is NOT a separate translation unit (mkfs, sfs, pmm_* and the APMS
//  globals are file-scope statics in kernel64.cpp).
// =====================================================================

// ---- tiny freestanding string helpers --------------------------------
static int apms_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static void apms_cpy(char* dst, int cap, const char* src) {
    int i = 0;
    if (cap <= 0) return;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static bool apms_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

static int apms_atoi(const char* s) {
    int v = 0, neg = 0;
    if (!s) return 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

static uint32_t apms_hexval(const char* s) {
    uint32_t v = 0;
    for (int i = 0; s && s[i] && i < 8; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else break;
    }
    return v;
}

// Append into a bounded buffer, always NUL-terminating.  Returns the new
// write cursor; hitting the end is safe (subsequent appends are no-ops).
static char* apms_puts(char* p, char* end, const char* s) {
    while (s && *s && p < end - 1) *p++ = *s++;
    if (p < end) *p = 0;
    return p;
}

static char* apms_putn(char* p, char* end, uint32_t v) {
    char b[12]; int i = 0;
    if (v == 0) b[i++] = '0';
    while (v > 0 && i < 11) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0 && p < end - 1) *p++ = b[--i];
    if (p < end) *p = 0;
    return p;
}

// ---- structured event ring -------------------------------------------
extern "C" void apms_event(int code, const char* tag, const char* msg) {
    ApmsEvent* e = &g_apms_events[g_apms_ev_head];
    g_apms_ev_head++;
    if (g_apms_ev_head >= APMS_EVENT_MAX) g_apms_ev_head = 0;
    if (g_apms_ev_n < APMS_EVENT_MAX) g_apms_ev_n++;
    e->seq = ++g_apms_seq;
    e->stamp = apms_stamp();
    e->code = code;
    apms_cpy(e->tag, (int)sizeof(e->tag), tag ? tag : "?");
    apms_cpy(e->msg, (int)sizeof(e->msg), msg ? msg : "");
}

// MiniCLR's fault() lives in clr.cpp (a different TU) and writes straight
// to the UART, bypassing serial_puts().  It calls this hook explicitly.
extern "C" void apms_on_clr_fault(const char* msg) {
    g_apms_clr_faults++;
    apms_event(20, "clr", msg ? msg : "CLR fault");
}

static const ApmsEvent* apms_ev_at(int i) {
    if (i < 0 || i >= g_apms_ev_n) return 0;
    int start = (g_apms_ev_n < APMS_EVENT_MAX) ? 0 : g_apms_ev_head;
    return &g_apms_events[(start + i) % APMS_EVENT_MAX];
}

extern "C" int apms_event_text(int i, char* out, int cap) {
    const ApmsEvent* e = apms_ev_at(i);
    if (!e || !out || cap < 8) return 0;
    char* p = out; char* end = out + cap;
    p = apms_puts(p, end, "#"); p = apms_putn(p, end, e->seq);
    p = apms_puts(p, end, " t"); p = apms_putn(p, end, e->stamp);
    p = apms_puts(p, end, " c"); p = apms_putn(p, end, (uint32_t)e->code);
    p = apms_puts(p, end, " ["); p = apms_puts(p, end, e->tag);
    p = apms_puts(p, end, "] "); p = apms_puts(p, end, e->msg);
    return (int)(p - out);
}

// ---- serial-log ring --------------------------------------------------
extern "C" int apms_log_tail(char* out, int cap) {
    if (!out || cap < 2) return 0;
    int want = cap - 1;
    if (want > g_apms_log_len) want = g_apms_log_len;
    if (want > 1024) want = 1024;
    int start = (g_apms_log_pos - want + APMS_LOG_SZ) % APMS_LOG_SZ;
    int n = 0;
    for (int i = 0; i < want; i++) {
        char c = g_apms_log[(start + i) % APMS_LOG_SZ];
        // Keep the report readable as plain text.
        if (c == 0 || (uint8_t)c < 32 && c != '\n' && c != '\t') c = '.';
        out[n++] = c;
    }
    out[n] = 0;
    return n;
}

extern "C" void apms_clear(void) {
    g_apms_log_pos = 0; g_apms_log_len = 0;
    g_apms_ev_n = 0; g_apms_ev_head = 0;
    g_apms_clr_faults = 0; g_apms_faults = 0;
    g_apms_captures = 0; g_apms_reports = 0;
}

// ---- manifest-driven application registry ----------------------------
// sfs_files/apps.man is the SINGLE SOURCE OF TRUTH.  There is no longer a
// compiled-in app list anywhere in the kernel or the managed shell.
static void apms_parse_line(char* line) {
    char* f[6]; int nf = 0;
    char* q = line;
    f[nf++] = q;
    while (*q && nf < 6) {
        if (*q == '|') { *q = 0; f[nf++] = q + 1; }
        q++;
    }
    if (nf < 6 || g_apms_app_n >= APMS_APP_MAX) return;
    // Trim a trailing CR from CRLF manifests.
    int dl = apms_len(f[5]);
    if (dl > 0 && f[5][dl - 1] == '\r') f[5][dl - 1] = 0;
    int kind = apms_atoi(f[0]);
    if (kind < 0 || kind >= 128) return;
    ApmsApp* a = &g_apms_apps[g_apms_app_n++];
    a->kind = kind;
    a->color = apms_hexval(f[3]);
    a->letter = f[4][0] ? (int)(uint8_t)f[4][0] : 'A';
    apms_cpy(a->name, APMS_NAME_SZ, f[1]);
    apms_cpy(a->mex, APMS_MEX_SZ, f[2]);
    apms_cpy(a->desc, APMS_DESC_SZ, f[5]);
}

static void apms_apps_ensure(void) {
    if (g_apms_apps_tried) return;
    // The manifest lives on the SFS volume.  If SFS is not mounted yet the
    // read cannot succeed, and marking the table as "tried" here would
    // leave the system with NO applications for the rest of the boot --
    // the desktop would be empty and every app launch would fail.  Retry
    // until the volume is available.
    if (!sfs.mounted) return;
    static uint8_t buf[8192];              // 8 KiB .bss, parsed once at boot
    int n = sfs.read("apps.man", buf, (int)sizeof(buf) - 1);
    if (n <= 0) {
        // The file may genuinely be missing (a hand-made image).  Only
        // then do we stop trying.
        g_apms_apps_tried = 1;
        serial_puts("[APMS] apps.man missing - no standalone apps registered\n");
        return;
    }
    g_apms_apps_tried = 1;                // parsed once
    buf[n] = 0;
    char* p = (char*)buf;
    while (*p) {
        char* e = p;
        while (*e && *e != '\n') e++;
        char saved = *e;
        *e = 0;
        if (p[0] && p[0] != '#' && p[0] != '\r') apms_parse_line(p);
        if (!saved) break;
        p = e + 1;
    }
    serial_puts("[APMS] manifest: ");
    serial_dec((uint32_t)g_apms_app_n);
    serial_puts(" standalone apps registered\n");
    apms_event(2, "manifest", "standalone app registry loaded");
}

extern "C" int apms_app_count(void) { apms_apps_ensure(); return g_apms_app_n; }
extern "C" int apms_event_count(void) { return g_apms_ev_n; }
extern "C" int apms_app_kind(int i) {
    apms_apps_ensure();
    return (i >= 0 && i < g_apms_app_n) ? g_apms_apps[i].kind : -1;
}
extern "C" const char* apms_app_name(int i) {
    apms_apps_ensure();
    return (i >= 0 && i < g_apms_app_n) ? g_apms_apps[i].name : "";
}
extern "C" const char* apms_app_mex(int i) {
    apms_apps_ensure();
    return (i >= 0 && i < g_apms_app_n) ? g_apms_apps[i].mex : "";
}
extern "C" uint32_t apms_app_color(int i) {
    apms_apps_ensure();
    return (i >= 0 && i < g_apms_app_n) ? g_apms_apps[i].color : 0x4A6FA5u;
}
extern "C" int apms_app_letter(int i) {
    apms_apps_ensure();
    return (i >= 0 && i < g_apms_app_n) ? g_apms_apps[i].letter : 'A';
}
extern "C" const char* apms_app_desc(int i) {
    apms_apps_ensure();
    return (i >= 0 && i < g_apms_app_n) ? g_apms_apps[i].desc : "";
}

static int apms_app_index_by_kind(int kind) {
    apms_apps_ensure();
    for (int i = 0; i < g_apms_app_n; i++)
        if (g_apms_apps[i].kind == kind) return i;
    return -1;
}

extern "C" const char* apms_mex_for_kind(int kind) {
    int i = apms_app_index_by_kind(kind);
    return (i >= 0) ? g_apms_apps[i].mex : 0;
}

extern "C" int apms_app_kind_by_mex(const char* mex) {
    apms_apps_ensure();
    if (!mex) return -1;
    for (int i = 0; i < g_apms_app_n; i++)
        if (apms_eq(g_apms_apps[i].mex, mex)) return g_apms_apps[i].kind;
    return -1;
}

// ---- framebuffer screenshot ------------------------------------------
// Reads the VBE/GOP info block at 0x5000 (see gui_legacy.cpp VbeInfo),
// downsamples into a RAM buffer FIRST, then writes a P6 PPM to the MKFS
// data disk.  Doing the framebuffer reads before any ATA PIO write avoids
// the >4 GiB LFB heisenbug documented in diag_step().
static inline void apms_get_rgb(volatile uint8_t* fb, int stride4, int pitch,
                                int x, int y, uint8_t fmt,
                                uint8_t* r, uint8_t* g, uint8_t* b) {
    uint32_t c;
    switch (fmt) {
    case 0: {   // BGRX32
        c = ((volatile uint32_t*)fb)[(uint64_t)y * stride4 + x];
        *r = (uint8_t)(c >> 16); *g = (uint8_t)(c >> 8); *b = (uint8_t)c;
        break;
    }
    case 1: {   // RGBX32
        c = ((volatile uint32_t*)fb)[(uint64_t)y * stride4 + x];
        *r = (uint8_t)c; *g = (uint8_t)(c >> 8); *b = (uint8_t)(c >> 16);
        break;
    }
    case 2: {   // RGB24, packed
        volatile uint8_t* row = fb + (uint64_t)y * pitch;
        *r = row[x * 3]; *g = row[x * 3 + 1]; *b = row[x * 3 + 2];
        break;
    }
    case 3: {   // RGB565
        uint16_t v = ((volatile uint16_t*)fb)[(uint64_t)y * (pitch / 2) + x];
        *r = (uint8_t)(((v >> 11) & 31) * 255 / 31);
        *g = (uint8_t)(((v >> 5) & 63) * 255 / 63);
        *b = (uint8_t)((v & 31) * 255 / 31);
        break;
    }
    default:
        *r = 0; *g = 0; *b = 0;
        break;
    }
}

extern "C" int apms_capture(int scale) {
    volatile uint8_t* vb = (volatile uint8_t*)0x5000;
    uint16_t w     = *(volatile uint16_t*)(vb + 0x04);
    uint16_t h     = *(volatile uint16_t*)(vb + 0x06);
    uint16_t pitch = *(volatile uint16_t*)(vb + 0x09);
    uint8_t  fmt   = *(volatile uint8_t*)(vb + 0x0F);
    uint64_t fb64  = *(volatile uint64_t*)(vb + 0x10);
    uintptr_t fbva = (uintptr_t)((fb64 > 0xFFFFFFFFULL)
                     ? fb64 : (uint64_t)(*(volatile uint32_t*)vb));
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || fbva == 0) return -1;
    if (fmt > 3) return -5;                       // BltOnly / unknown
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    int ow = (int)w / scale, oh = (int)h / scale;
    if (ow < 1 || oh < 1) return -2;
    if ((uint64_t)ow * oh * 3 > APMS_SHOT_PIX_CAP) return -3;
    int pstride = pitch ? (int)pitch / 4 : (int)w;
    if (fmt == 2 && pstride < (int)w) pstride = (int)w;  // unused for 24-bit
    uint32_t total = 64u + (uint32_t)((uint64_t)ow * oh * 3);

    uint8_t* buf = (uint8_t*)big_alloc(total);
    if (!buf) return -4;

    char* p = (char*)buf;
    char* end = (char*)buf + total;
    p = apms_puts(p, end, "P6\n");
    p = apms_putn(p, end, (uint32_t)ow);
    p = apms_puts(p, end, " ");
    p = apms_putn(p, end, (uint32_t)oh);
    p = apms_puts(p, end, "\n255\n");
    uint8_t* px = (uint8_t*)p;
    volatile uint8_t* fb = (volatile uint8_t*)fbva;

    for (int y = 0; y < oh; y++) {
        int sy = y * scale;
        for (int x = 0; x < ow; x++) {
            int sx = x * scale;
            uint8_t r, g, b;
            apms_get_rgb(fb, pstride, (int)pitch, sx, sy, fmt, &r, &g, &b);
            *px++ = r; *px++ = g; *px++ = b;
        }
    }
    uint32_t len = (uint32_t)(px - buf);

    // The user data volume may have been "cd"-ed into a subfolder; reports
    // always live in the root so tools/apms_extract.py can find them.
    g_cwd = 0xFFFF;
    int rc = mkfs.create("apms_shot.ppm", buf, (int)len);
    big_free(buf, total);
    if (rc < 0) {
        apms_event(31, "capture", "MKFS write failed for screenshot");
        return -6;
    }
    g_apms_captures++;
    char note[64];
    char* np = note; char* ne = note + sizeof(note);
    np = apms_puts(np, ne, "shot "); np = apms_putn(np, ne, (uint32_t)ow);
    np = apms_puts(np, ne, "x"); np = apms_putn(np, ne, (uint32_t)oh);
    apms_event(30, "capture", note);
    serial_puts("[APMS] screenshot apms_shot.ppm ");
    serial_dec(len);
    serial_puts(" bytes\n");
    return (int)len;
}

// ---- Markdown bug report ---------------------------------------------
extern "C" int apms_report(const char* user_text) {
    uint8_t* buf = (uint8_t*)big_alloc(APMS_REPORT_MAX);
    if (!buf) return -1;
    char* p = (char*)buf;
    char* end = (char*)buf + APMS_REPORT_MAX;

    p = apms_puts(p, end, "# NexOS APMS Bug 报告\n\n");
    p = apms_puts(p, end, "由「Bug 上报」独立应用生成；把本文件与 apms_shot.png 一起交给助手即可。\n\n");

    p = apms_puts(p, end, "## 运行环境\n\n");
    p = apms_puts(p, end, "- 内核: NexOS 64-bit (kernel64.cpp), 无 IRQ / 协作式帧循环\n");
    p = apms_puts(p, end, "- TSC 近似毫秒: "); p = apms_putn(p, end, apms_stamp());
    p = apms_puts(p, end, "\n- 最后诊断步骤: "); p = apms_putn(p, end, g_last_step);
    p = apms_puts(p, end, " ("); p = apms_puts(p, end, g_last_step_name);
    p = apms_puts(p, end, ")\n");
    p = apms_puts(p, end, "- 已注册独立应用: "); p = apms_putn(p, end, (uint32_t)apms_app_count());
    p = apms_puts(p, end, "\n\n## 运行指标\n\n");
    p = apms_puts(p, end, "- 物理内存: "); p = apms_putn(p, end, pmm_mem_kb);
    p = apms_puts(p, end, " KiB, 总页 "); p = apms_putn(p, end, pmm_total_pages);
    p = apms_puts(p, end, ", 空闲页 "); p = apms_putn(p, end, pmm_free_pages);
    p = apms_puts(p, end, ", 已用页 "); p = apms_putn(p, end, pmm_used_pages);
    p = apms_puts(p, end, "\n- 内核堆: 分配次数 "); p = apms_putn(p, end, heap_alloc_count);
    p = apms_puts(p, end, ", 释放次数 "); p = apms_putn(p, end, heap_free_count);
    p = apms_puts(p, end, ", 已分配字节 "); p = apms_putn(p, end, heap_bytes_alloc);
    p = apms_puts(p, end, "\n- 故障: CPU "); p = apms_putn(p, end, g_apms_faults);
    p = apms_puts(p, end, ", CLR "); p = apms_putn(p, end, g_apms_clr_faults);
    p = apms_puts(p, end, ", 截图 "); p = apms_putn(p, end, g_apms_captures);
    p = apms_puts(p, end, ", 报告 "); p = apms_putn(p, end, g_apms_reports);
    p = apms_puts(p, end, "\n\n## 应用清单 (apps.man)\n\n");
    apms_apps_ensure();
    for (int i = 0; i < g_apms_app_n; i++) {
        p = apms_puts(p, end, "- kind="); p = apms_putn(p, end, (uint32_t)g_apms_apps[i].kind);
        p = apms_puts(p, end, " "); p = apms_puts(p, end, g_apms_apps[i].name);
        p = apms_puts(p, end, " -> "); p = apms_puts(p, end, g_apms_apps[i].mex);
        p = apms_puts(p, end, "\n");
    }

    p = apms_puts(p, end, "\n## APMS 事件 (最近 ");
    p = apms_putn(p, end, (uint32_t)g_apms_ev_n);
    p = apms_puts(p, end, " 条)\n\n");
    static char evline[160];
    for (int i = 0; i < g_apms_ev_n; i++) {
        apms_event_text(i, evline, (int)sizeof(evline));
        p = apms_puts(p, end, "- "); p = apms_puts(p, end, evline);
        p = apms_puts(p, end, "\n");
    }

    p = apms_puts(p, end, "\n## 串口日志尾部\n\n```\n");
    static char tail[1025];
    apms_log_tail(tail, (int)sizeof(tail));
    p = apms_puts(p, end, tail);
    p = apms_puts(p, end, "```\n");

    p = apms_puts(p, end, "\n## 截图\n\n");
    p = apms_puts(p, end, "同卷 `apms_shot.ppm`（P6 / RGB24 / 按比例缩小）。\n");
    p = apms_puts(p, end, "宿主机执行 `python tools/apms_extract.py` 会得到 `apms_reports/apms_shot.png`。\n");

    p = apms_puts(p, end, "\n## 出现情况 / 复现步骤（用户填写）\n\n");
    p = apms_puts(p, end, (user_text && user_text[0]) ? user_text : "(未填写)");
    p = apms_puts(p, end, "\n");

    int len = (int)(p - (char*)buf);
    g_cwd = 0xFFFF;
    int rc = mkfs.create("apms_report.md", buf, len);
    big_free(buf, APMS_REPORT_MAX);
    if (rc < 0) {
        apms_event(41, "report", "MKFS write failed for bug report");
        return -6;
    }
    g_apms_reports++;
    apms_event(40, "report", "apms_report.md written to data disk");
    serial_puts("[APMS] bug report apms_report.md ");
    serial_dec((uint32_t)len);
    serial_puts(" bytes\n");
    return len;
}

// ---- shell command ----------------------------------------------------
// `apms status | apps | events | log | shot [1..8] | report [text] | clear`
static void cmd_apms(const char* args) {
    char sub[24]; int i = 0;
    while (args && *args == ' ') args++;
    while (args && *args && *args != ' ' && i < 23) sub[i++] = *args++;
    sub[i] = 0;
    const char* rest = args;
    while (rest && *rest == ' ') rest++;

    if (sub[0] == 0 || apms_eq(sub, "status")) {
        term.write("APMS: events ");
        term.write_dec((int)g_apms_ev_n);
        term.write(" / log bytes ");
        term.write_dec(g_apms_log_len);
        term.write(" / apps ");
        term.write_dec(apms_app_count());
        term.write(" / cpu faults ");
        term.write_dec((int)g_apms_faults);
        term.write(" / clr faults ");
        term.write_dec((int)g_apms_clr_faults);
        term.write("\n");
        return;
    }
    if (apms_eq(sub, "apps")) {
        apms_apps_ensure();
        for (int j = 0; j < g_apms_app_n; j++) {
            term.write_dec(g_apms_apps[j].kind);
            term.write("  ");
            term.write(g_apms_apps[j].name);
            term.write("  ->  ");
            term.write(g_apms_apps[j].mex);
            term.write("\n");
        }
        return;
    }
    if (apms_eq(sub, "events")) {
        static char l[160];
        for (int j = 0; j < g_apms_ev_n; j++) {
            apms_event_text(j, l, (int)sizeof(l));
            term.write(l); term.write("\n");
        }
        return;
    }
    if (apms_eq(sub, "log")) {
        static char lt[1025];
        apms_log_tail(lt, (int)sizeof(lt));
        term.write(lt); term.write("\n");
        return;
    }
    if (apms_eq(sub, "shot")) {
        int rc = apms_capture(rest && rest[0] ? apms_atoi(rest) : 2);
        term.write(rc > 0 ? "APMS: screenshot written to apms_shot.ppm\n"
                          : "APMS: screenshot failed (see serial log)\n");
        return;
    }
    if (apms_eq(sub, "report")) {
        int rc = apms_report(rest);
        term.write(rc > 0 ? "APMS: report written to apms_report.md\n"
                          : "APMS: report failed (see serial log)\n");
        return;
    }
    if (apms_eq(sub, "clear")) {
        apms_clear();
        term.write("APMS: event and log rings cleared\n");
        return;
    }
    term.write("Usage: apms [status|apps|events|log|shot [1..8]|report [text]|clear]\n");
}
