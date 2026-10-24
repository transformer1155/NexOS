// =====================================================================
//  silent_telemetry.cpp  -  Silent Telemetry System (no UI)
// ---------------------------------------------------------------------
//  Freestanding: -ffreestanding -fno-exceptions -fno-rtti -nostdlib.
//  All buffers are BSS-pre-allocated; zero runtime allocations.
// =====================================================================
#include "silent_telemetry.h"

// ---------------------------------------------------------------------
//  Freestanding helpers
// ---------------------------------------------------------------------
static void* tmemset_(void* d, int v, int n) {
    unsigned char* p = (unsigned char*)d;
    while (n--) *p++ = (unsigned char)v;
    return d;
}
static void tstrcpy_(char* d, const char* s) { while ((*d++ = *s++)); }
static int  tstrlen_(const char* s) { int n = 0; while (s[n]) n++; return n; }
static int  tmin_(int a, int b) { return a < b ? a : b; }
static uint32_t tclamp_(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo; if (v > hi) return hi; return v;
}
static void tutoa_(uint32_t v, char buf[12]) {
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    int k = 0;
    while (n > 0) buf[k++] = buf[--n];
    buf[k] = 0;
}

static inline uint32_t t_rdtsc32(void) {
    uint32_t lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo) : : "edx");
    return lo;
}

// ---------------------------------------------------------------------
//  Globals (BSS)
// ---------------------------------------------------------------------
static TeAppContext  te_apps[TELEM_MAX_APPS];
static uint32_t  te_fps_buf[TELEM_FPS_HISTORY];
static uint32_t  te_fps_head;
static uint32_t  te_fps_count;
static TeStutter te_stutters[TELEM_MAX_STUTTERS];
static uint32_t  te_stut_head;
static uint32_t  te_stut_count;
static TeClickLatency te_clicks[TELEM_MAX_CLICK_LOG];
static uint32_t  te_clk_head;
static uint32_t  te_clk_count;
static int       te_clk_pending;

static float     te_fps_current;
static uint32_t  te_fps_min;
static uint32_t  te_fps_max;

static float     te_launch_fps_before;
static uint32_t  te_launch_lowest_fps;
static int       te_launch_active;
static uint32_t  te_launch_start_ms;

static uint32_t  te_uptime_ms;
static uint32_t  te_prev_frame_tc;
static int       te_first_frame;
static uint64_t  te_ticks_per_ms;

// ---------------------------------------------------------------------
//  Initialisation
// ---------------------------------------------------------------------
extern "C" void telem_init_early(void) {
    tmemset_(te_apps, 0, sizeof(te_apps));
    tmemset_(te_fps_buf, 0, sizeof(te_fps_buf));
    tmemset_(te_stutters, 0, sizeof(te_stutters));
    tmemset_(te_clicks, 0, sizeof(te_clicks));
    te_first_frame = 1;
    te_fps_min = 0xFFFFFFFFu;
    te_prev_frame_tc = t_rdtsc32();
}

extern "C" void telem_init_full(void) {
    // Placeholder calibration -- gui.cpp will write a real value into
    // te_ticks_per_ms once it has PIT/Timer calibration data.
    if (te_ticks_per_ms == 0) te_ticks_per_ms = 2500000ULL;
}

// ---------------------------------------------------------------------
//  Frame lifecycle
// ---------------------------------------------------------------------
extern "C" void telem_frame_begin(void) {
    uint32_t now = t_rdtsc32();
    uint32_t dt;
    if (te_first_frame) {
        dt = 16;
        te_first_frame = 0;
    } else {
        dt = (now > te_prev_frame_tc)
            ? (now - te_prev_frame_tc)
            : (0xFFFFFFFFu - te_prev_frame_tc) + now + 1;
    }
    te_prev_frame_tc = now;

    uint32_t dt_ms = te_ticks_per_ms ? (uint32_t)((uint64_t)dt / te_ticks_per_ms) : 16;
    if (dt_ms == 0) dt_ms = 1;
    if (dt_ms > 1000) dt_ms = 1000;

    te_fps_buf[te_fps_head] = dt_ms;
    te_fps_head = (te_fps_head + 1) & (TELEM_FPS_HISTORY - 1);
    if (te_fps_count < TELEM_FPS_HISTORY) te_fps_count++;

    uint32_t accum = 0;
    for (uint32_t i = 0; i < te_fps_count; i++) accum += te_fps_buf[i];
    te_fps_current = accum > 0
        ? (float)(te_fps_count * 1000u) / (float)accum
        : 0.0f;

    uint32_t inst_fps = dt_ms ? (1000u / dt_ms) : 60;
    inst_fps = tclamp_(inst_fps, 0, 240);
    if (te_fps_min == 0xFFFFFFFFu) te_fps_min = inst_fps;
    if (inst_fps < te_fps_min) te_fps_min = inst_fps;
    if (inst_fps > te_fps_max) te_fps_max = inst_fps;

    if (te_launch_active) {
        if (te_fps_current < (float)te_launch_lowest_fps)
            te_launch_lowest_fps = (uint32_t)te_fps_current;
    }

    te_uptime_ms += dt_ms;

    // Auto-close launch-capture window after 500ms
    if (te_launch_active && (te_uptime_ms - te_launch_start_ms) > 500)
        te_launch_active = 0;

    if (te_clk_pending) {
        TeClickLatency* c = &te_clicks[te_clk_head];
        uint32_t end_tc = t_rdtsc32();
        uint64_t delta = (end_tc >= (uint32_t)c->click_tick)
            ? (uint64_t)(end_tc - (uint32_t)c->click_tick)
            : (uint64_t)(0xFFFFFFFFu - (uint32_t)c->click_tick) + end_tc + 1;
        c->latency_ms = te_ticks_per_ms ? (uint32_t)(delta / te_ticks_per_ms) : 0;
        c->render_tick = end_tc;
        c->resolved = 1;
        te_clk_head = (te_clk_head + 1) & (TELEM_MAX_CLICK_LOG - 1);
        if (te_clk_count < TELEM_MAX_CLICK_LOG) te_clk_count++;
        te_clk_pending = 0;
    }
}

extern "C" void telem_frame_end(uint32_t dirty_pixels) {
    (void)dirty_pixels;
}

// ---------------------------------------------------------------------
//  Mouse & click hooks
// ---------------------------------------------------------------------
extern "C" void telem_mouse_move(int dx, int dy) {
    static uint32_t prev_tc = 0;
    static int      have_prev = 0;
    uint32_t now = t_rdtsc32();
    uint32_t dt;
    if (!have_prev) { dt = 0; have_prev = 1; }
    else if (now > prev_tc) dt = now - prev_tc;
    else                     dt = (0xFFFFFFFFu - prev_tc) + now + 1;
    prev_tc = now;

    uint32_t dt_ms = te_ticks_per_ms ? (uint32_t)((uint64_t)dt / te_ticks_per_ms) : 16;

    if ((dx != 0 || dy != 0) && dt_ms > 33) {
        TeStutter* s = &te_stutters[te_stut_head];
        s->frame_interval_ms = dt_ms;
        s->mouse_dx = (int16_t)tmin_(tmin_(dx, 32767), -32768);
        s->mouse_dy = (int16_t)tmin_(tmin_(dy, 32767), -32768);
        s->uptime_s = te_uptime_ms / 1000u;
        te_stut_head = (te_stut_head + 1) & (TELEM_MAX_STUTTERS - 1);
        if (te_stut_count < TELEM_MAX_STUTTERS) te_stut_count++;
    }
}

extern "C" void telem_click_begin(void) {
    uint32_t now = t_rdtsc32();
    TeClickLatency* c = &te_clicks[te_clk_head];
    c->click_tick = now;
    c->render_tick = 0;
    c->latency_ms = 0;
    c->resolved = 0;
    te_clk_pending = 1;
}

// ---------------------------------------------------------------------
//  App lifecycle
// ---------------------------------------------------------------------
static TeAppContext* find_app(TeAppKind kind) {
    for (int i = 0; i < TELEM_MAX_APPS; i++) {
        if (te_apps[i].active && te_apps[i].kind == kind) return &te_apps[i];
    }
    return 0;
}

extern "C" void telem_app_register(const char* name, TeAppKind kind) {
    TeAppContext* a = find_app(kind);
    if (!a) {
        for (int i = 0; i < TELEM_MAX_APPS; i++) {
            if (!te_apps[i].active) { a = &te_apps[i]; break; }
        }
    }
    if (!a) return;
    tmemset_(a, 0, sizeof(TeAppContext));
    int n = 0;
    while (name && name[n] && n < TELEM_MAX_APP_NAME - 1) { a->name[n] = name[n]; n++; }
    a->name[n] = 0;
    a->kind = kind;
    a->active = 1;
    a->launches = 1;
}

extern "C" void telem_app_unregister(TeAppKind kind) {
    TeAppContext* a = find_app(kind);
    if (a) a->active = 0;
}

extern "C" void telem_app_set_heap(TeAppKind kind, uint32_t bytes) {
    TeAppContext* a = find_app(kind);
    if (!a) return;
    a->heap_bytes_cur = bytes;
    if (bytes > a->heap_bytes_peak) a->heap_bytes_peak = bytes;
}

extern "C" void telem_app_accum_cpu_ticks(TeAppKind kind, uint64_t delta) {
    TeAppContext* a = find_app(kind);
    if (!a) return;
    a->cpu_ticks_delta += delta;
}

extern "C" void telem_app_launch_start(TeAppKind kind) {
    te_launch_fps_before = te_fps_current;
    te_launch_lowest_fps = (uint32_t)te_fps_current;
    te_launch_active = 1;
    te_launch_start_ms = te_uptime_ms;
    TeAppContext* a = find_app(kind);
    if (a) a->launches++;
}

extern "C" void telem_app_launch_end(TeAppKind kind) {
    te_launch_active = 0;
    (void)kind;
}

// Expose the tick ratio so callers (gui.cpp after PIT calibrate) can set it
extern "C" void telem_set_ticks_per_ms(uint64_t v) {
    if (v > 0 && v < 50000000ULL) te_ticks_per_ms = v;
}
extern "C" uint64_t telem_get_ticks_per_ms(void) { return te_ticks_per_ms; }

// ---------------------------------------------------------------------
//  Report builder
// ---------------------------------------------------------------------
extern "C" void telem_report(telem_sink_t out_fn) {
    char buf[256];
    int pos = 0;

    // Uptime
    char ub[12]; tutoa_(te_uptime_ms / 1000u, ub);
    const char* h1 = "[SYS_PERF] Uptime: ";
    int n1 = tstrlen_(h1);
    for (int i = 0; i < n1; i++) buf[pos++] = h1[i];
    int nu = tstrlen_(ub);
    for (int i = 0; i < nu; i++) buf[pos++] = ub[i];
    buf[pos++] = 's'; buf[pos++] = '\n';
    out_fn(buf, pos); pos = 0;

    // Global FPS
    uint32_t whole = (uint32_t)te_fps_current;
    uint32_t frac10 = (uint32_t)((te_fps_current - (float)whole) * 10.0f + 0.5f);
    if (frac10 >= 10) { frac10 = 0; whole++; }
    char wb[12]; tutoa_(whole, wb);
    char fb[4];  tutoa_(frac10, fb);
    if (fb[0] == 0) { fb[0] = '0'; fb[1] = 0; }

    char minb[12]; tutoa_(te_fps_min == 0xFFFFFFFFu ? 0u : te_fps_min, minb);
    char maxb[12]; tutoa_(te_fps_max, maxb);

    const char* h2 = "[SYS_PERF] Global FPS: ";
    int n2 = tstrlen_(h2);
    for (int i = 0; i < n2; i++) buf[pos++] = h2[i];
    int nw = tstrlen_(wb); for (int i = 0; i < nw; i++) buf[pos++] = wb[i];
    buf[pos++] = '.';
    int nf = tstrlen_(fb); for (int i = 0; i < nf; i++) buf[pos++] = fb[i];
    const char* m1 = " (Min: ";
    int nm1 = tstrlen_(m1); for (int i = 0; i < nm1; i++) buf[pos++] = m1[i];
    int nmin = tstrlen_(minb); for (int i = 0; i < nmin; i++) buf[pos++] = minb[i];
    const char* m2 = ", Max: ";
    int nm2 = tstrlen_(m2); for (int i = 0; i < nm2; i++) buf[pos++] = m2[i];
    int nmax = tstrlen_(maxb); for (int i = 0; i < nmax; i++) buf[pos++] = maxb[i];
    buf[pos++] = ')'; buf[pos++] = '\n';
    out_fn(buf, pos); pos = 0;

    // Mouse stutters + max delay
    uint32_t max_sd = 0;
    for (uint32_t i = 0; i < te_stut_count; i++) {
        if (te_stutters[i].frame_interval_ms > max_sd)
            max_sd = te_stutters[i].frame_interval_ms;
    }
    char sc[12]; tutoa_(te_stut_count, sc);
    char mdb[12]; tutoa_(max_sd, mdb);
    const char* h3 = "[SYS_PERF] Mouse Stutters: ";
    int n3 = tstrlen_(h3); for (int i = 0; i < n3; i++) buf[pos++] = h3[i];
    int ns = tstrlen_(sc); for (int i = 0; i < ns; i++) buf[pos++] = sc[i];
    const char* m3 = " (Max Delay: ";
    int nm3 = tstrlen_(m3); for (int i = 0; i < nm3; i++) buf[pos++] = m3[i];
    if (max_sd == 0) { buf[pos++] = '0'; }
    else { int nmd = tstrlen_(mdb); for (int i = 0; i < nmd; i++) buf[pos++] = mdb[i]; }
    buf[pos++] = 'm'; buf[pos++] = 's'; buf[pos++] = ')'; buf[pos++] = '\n';
    out_fn(buf, pos); pos = 0;

    // Click latency (last sample)
    uint32_t last_ms = 0;
    if (te_clk_count > 0) {
        int idx = (int)(te_clk_head == 0 ? TELEM_MAX_CLICK_LOG - 1 : te_clk_head - 1);
        if (te_clicks[idx].latency_ms > 0) last_ms = te_clicks[idx].latency_ms;
    }
    char lcb[12]; tutoa_(last_ms, lcb);
    const char* h4 = "[SYS_PERF] Click Latency (last): ";
    int n4 = tstrlen_(h4); for (int i = 0; i < n4; i++) buf[pos++] = h4[i];
    int nl = tstrlen_(lcb); for (int i = 0; i < nl; i++) buf[pos++] = lcb[i];
    buf[pos++] = 'm'; buf[pos++] = 's'; buf[pos++] = '\n';
    out_fn(buf, pos); pos = 0;

    // Per-App lines
    for (int i = 0; i < TELEM_MAX_APPS; i++) {
        TeAppContext* a = &te_apps[i];
        if (!a->active) continue;
        const char* h5 = "[SYS_PERF] App: ";
        int n5 = tstrlen_(h5); for (int j = 0; j < n5; j++) buf[pos++] = h5[j];
        int nan = tstrlen_(a->name); for (int j = 0; j < nan; j++) buf[pos++] = a->name[j];
        const char* m4 = " | Heap: ";
        int nm4 = tstrlen_(m4); for (int j = 0; j < nm4; j++) buf[pos++] = m4[j];

        uint32_t bytes = a->heap_bytes_cur;
        uint32_t mb = bytes / (1024u * 1024u);
        uint32_t kb_part = (bytes % (1024u * 1024u)) / 1024u;
        uint32_t frac_mb = (kb_part * 10u) / 1024u;
        char mbs[12]; tutoa_(mb, mbs);
        char fms[4];  tutoa_(frac_mb, fms);
        if (fms[0] == 0) { fms[0] = '0'; fms[1] = 0; }

        int nmb = tstrlen_(mbs); for (int j = 0; j < nmb; j++) buf[pos++] = mbs[j];
        buf[pos++] = '.';
        int nfm = tstrlen_(fms); for (int j = 0; j < nfm; j++) buf[pos++] = fms[j];
        buf[pos++] = 'M'; buf[pos++] = 'B';

        const char* m5 = " | CPU: ";
        int nm5 = tstrlen_(m5); for (int j = 0; j < nm5; j++) buf[pos++] = m5[j];

        uint32_t cp10 = a->cpu_pct_latest;
        uint32_t cp_whole = cp10 / 10;
        uint32_t cp_frac  = cp10 % 10;
        char cps[12]; tutoa_(cp_whole, cps);
        char cfs[4];  tutoa_(cp_frac, cfs);
        if (cfs[0] == 0) { cfs[0] = '0'; cfs[1] = 0; }

        int ncp = tstrlen_(cps); for (int j = 0; j < ncp; j++) buf[pos++] = cps[j];
        buf[pos++] = '.';
        int ncf = tstrlen_(cfs); for (int j = 0; j < ncf; j++) buf[pos++] = cfs[j];
        buf[pos++] = '%'; buf[pos++] = '\n';
        out_fn(buf, pos); pos = 0;
    }
}

extern "C" void telem_perf_dump(void) {
    // Leave as a stub; real output goes through telem_report()
}
