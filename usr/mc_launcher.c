/* =====================================================================
 *  usr/mc_launcher.c  -  NexOS remote-desktop demo.
 * ---------------------------------------------------------------------
 *  A Minecraft-style launcher GUI rendered to the framebuffer by a guest
 *  Linux ELF32, driven over HTTP: the browser fetches /screen for the
 *  pixels and POSTs /input to move the pointer / click, which the kernel
 *  forwards to this program via the nexos_input_wait() syscall.
 *
 *  Built exactly like usr/python.c (freestanding ELF32, int 0x80 ABI).
 * ===================================================================== */
#include "libc.h"
#include <stdint.h>

/* ---- remote-desktop ABI (kept in sync with kernel's remote_desktop.h) ---- */
#define NEXOS_SYS_FB    410   /* renumbered from 400 to avoid clash with guest TCP socket bridge (usr/libc.h 400/401/402/403) */
#define NEXOS_SYS_INPUT 411
#define FB_BGRX32 0
#define FB_RGBX32 1

struct NexosFBInfo {
    uint32_t phys;
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint8_t  bpp;
    uint8_t  format;
    uint8_t  _pad[2];
};

struct NexosInput {
    uint32_t seq;
    int32_t  mouse_x;
    int32_t  mouse_y;
    uint8_t  buttons;
    uint8_t  key;
    uint8_t  key_down;
    uint8_t  _pad;
};

static inline long sys(int num, long a, long b, long c, long d)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d)
        : "memory", "cc");
    return ret;
}

/* ---- framebuffer state ---- */
static struct NexosFBInfo g_fb;
static volatile uint32_t* g_vram;
static unsigned int       g_ppr;   /* pixels per row = pitch/4 */

/* Pack a color into the framebuffer's reported pixel format. */
static inline uint32_t pack(int r, int g, int b)
{
    if (g_fb.format == FB_RGBX32)
        return (0xFFu<<24) | ((uint32_t)b<<16) | ((uint32_t)g<<8) | (uint32_t)r;
    return (0xFFu<<24) | ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)b; /* BGRX32 */
}

static inline void setpx(int x, int y, int r, int g, int b)
{
    if (x < 0 || y < 0 || (uint32_t)x >= g_fb.width || (uint32_t)y >= g_fb.height) return;
    g_vram[(uint32_t)y * g_ppr + (uint32_t)x] = pack(r, g, b);
}

static void fill_rect(int x0, int y0, int x1, int y1, int r, int g, int b)
{
    int x, y;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            setpx(x, y, r, g, b);
}

/* ---- layout constants ---- */
#define PLAY_X0 412
#define PLAY_X1 612
#define PLAY_Y0 620
#define PLAY_Y1 700

static int g_launching = 0;
static int g_progress  = 0;   /* 0..100 */

static int in_play(int mx, int my)
{
    return mx >= PLAY_X0 && mx <= PLAY_X1 && my >= PLAY_Y0 && my <= PLAY_Y1;
}

static void draw(const struct NexosInput* in)
{
    int w = g_fb.width, h = g_fb.height;

    /* background */
    fill_rect(0, 0, w - 1, h - 1, 28, 28, 32);

    /* left sidebar */
    fill_rect(0, 0, 220, h - 1, 40, 40, 46);

    /* creeper logo (classic face) */
    fill_rect(60, 40, 160, 140, 76, 175, 80);
    fill_rect(75, 60, 97, 82, 0, 0, 0);       /* left eye   */
    fill_rect(123, 60, 145, 82, 0, 0, 0);     /* right eye  */
    fill_rect(108, 85, 132, 160, 0, 0, 0);    /* mouth vert */
    fill_rect(85, 108, 147, 130, 0, 0, 0);    /* mouth horiz */

    /* sidebar menu items (hover highlight) */
    int item_y[3] = {180, 232, 284};
    int i;
    for (i = 0; i < 3; i++) {
        int hover = (in->mouse_x >= 20 && in->mouse_x <= 200 &&
                     in->mouse_y >= item_y[i] && in->mouse_y <= item_y[i] + 44);
        fill_rect(20, item_y[i], 200, item_y[i] + 44,
                  hover ? 60 : 48, hover ? 60 : 48, hover ? 70 : 56);
        if (hover) fill_rect(20, item_y[i], 26, item_y[i] + 44, 120, 170, 255);
    }

    /* account box (top-right) */
    fill_rect(820, 20, 1004, 70, 52, 52, 60);
    fill_rect(836, 36, 872, 56, 120, 170, 255);

    /* PLAY button */
    int pr, pg, pb;
    int over = in_play(in->mouse_x, in->mouse_y);
    if (g_launching) {
        pr = 230; pg = 126; pb = 34;            /* orange while launching */
    } else if (g_progress >= 100) {
        pr = 66; pg = 139; pb = 202;            /* blue when ready */
    } else if (over) {
        pr = 102; pg = 200; pb = 106;           /* brighter green on hover */
    } else {
        pr = 76; pg = 175; pb = 80;             /* normal green */
    }
    fill_rect(PLAY_X0, PLAY_Y0, PLAY_X1, PLAY_Y1, pr, pg, pb);
    fill_rect(PLAY_X0 + 70, PLAY_Y0 + 30, PLAY_X1 - 70, PLAY_Y0 + 50,
              pr / 2 + 20, pg / 2 + 20, pb / 2 + 20);

    /* launch progress bar */
    if (g_launching || g_progress > 0) {
        fill_rect(362, 560, 662, 590, 30, 30, 34);
        int pw = 300 * g_progress / 100;
        fill_rect(362, 560, 362 + pw, 590, 230, 126, 34);
    }
}

int main(int argc, char** argv, char** envp)
{
    (void)argc; (void)argv;

    sys(NEXOS_SYS_FB, (long)&g_fb, 0, 0, 0);
    printf("MC_LAUNCHER: fb phys=%x w=%d h=%d pitch=%d fmt=%d\n",
           (unsigned)g_fb.phys, g_fb.width, g_fb.height, g_fb.pitch, g_fb.format);
    if (g_fb.phys == 0 || g_fb.width == 0) {
        printf("MC_LAUNCHER: NO FRAMEBUFFER\n");
        return 2;
    }
    g_ppr   = g_fb.pitch / 4;
    g_vram  = (volatile uint32_t*)(uintptr_t)g_fb.phys;

    struct NexosInput in;
    in.seq = 0;
    in.mouse_x = g_fb.width / 2;
    in.mouse_y = g_fb.height / 2;
    in.buttons = 0; in.key = 0; in.key_down = 0;

    printf("MC_LAUNCHER: ready (waiting for input over HTTP)\n");
    draw(&in);   /* draw once so /screen shows content before first input */

    for (;;) {
        sys(NEXOS_SYS_INPUT, (long)&in, 0, 0, 0);

        if (in.buttons & 1) {
            if (in_play(in.mouse_x, in.mouse_y) && !g_launching && g_progress < 100)
                g_launching = 1;
        }
        if (g_launching) {
            g_progress += 2;
            if (g_progress >= 100) { g_progress = 100; g_launching = 0; }
        }
        draw(&in);
    }
    return 0;
}
