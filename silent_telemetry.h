// =====================================================================
//  silent_telemetry.h  -  Silent Telemetry System (no UI, pure data collection)
// =====================================================================
#ifndef NEXOS_SILENT_TELEMETRY_H
#define NEXOS_SILENT_TELEMETRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TELEM_MAX_APPS          32
#define TELEM_MAX_APP_NAME      32
#define TELEM_FPS_HISTORY       512
#define TELEM_MAX_STUTTERS     128
#define TELEM_MAX_CLICK_LOG    256

typedef int TeAppKind;

typedef struct {
    char      name[TELEM_MAX_APP_NAME];
    TeAppKind kind;
    uint32_t  heap_bytes_cur;
    uint32_t  heap_bytes_peak;
    uint64_t  cpu_ticks_delta;
    uint64_t  cpu_ticks_last;
    uint32_t  cpu_pct_latest;
    uint32_t  frames_painted;
    uint32_t  launches;
    int       active;
} TeAppContext;

typedef struct {
    uint32_t frame_interval_ms;
    int16_t  mouse_dx;
    int16_t  mouse_dy;
    uint32_t uptime_s;
} TeStutter;

typedef struct {
    uint64_t click_tick;
    uint64_t render_tick;
    uint32_t latency_ms;
    int      resolved;
} TeClickLatency;

void telem_init_early(void);
void telem_init_full(void);

void telem_frame_begin(void);
void telem_frame_end(uint32_t dirty_pixels);

void telem_mouse_move(int dx, int dy);
void telem_click_begin(void);

void telem_app_register(const char* name, TeAppKind kind);
void telem_app_unregister(TeAppKind kind);
void telem_app_set_heap(TeAppKind kind, uint32_t bytes);
void telem_app_accum_cpu_ticks(TeAppKind kind, uint64_t delta);
void telem_app_launch_start(TeAppKind kind);
void telem_app_launch_end(TeAppKind kind);

typedef void (*telem_sink_t)(const char* text, int len);
void telem_report(telem_sink_t out_fn);
void telem_perf_dump(void);

// Let callers (gui.cpp after PIT calibrate) write the TSC->ms ratio
void telem_set_ticks_per_ms(uint64_t v);
uint64_t telem_get_ticks_per_ms(void);

#ifdef __cplusplus
}
#endif

#endif
