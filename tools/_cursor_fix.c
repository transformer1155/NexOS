/* Verify the fix: restore_bg must run unless the damage FULLY CONTAINS the
 * cursor box.  Simulates the same 3-frame sequence as _cursor_ghost.c with the
 * corrected condition and checks that no stale cursor ink survives. */
#include <stdio.h>
#include <stdint.h>

#define SW 1024
#define SH 768
#define CURSOR_SIZE 32

static uint32_t bb[SW * SH];
static uint32_t saved[CURSOR_SIZE * CURSOR_SIZE];
static int saved_valid = 0;

#define BG   0x00102030u
#define CURS 0x00FFFFFFu

static int cursor_x = 200, cursor_y = 200;
static int dmg_x, dmg_y, dmg_w, dmg_h, dmg_valid = 0;

static void damage_add(int x, int y, int w, int h) {
    dmg_x = x; dmg_y = y; dmg_w = w; dmg_h = h; dmg_valid = 1;
}
static void save_bg(void) {
    if (!saved_valid) {
        for (int dy = 0; dy < CURSOR_SIZE; dy++)
            for (int dx = 0; dx < CURSOR_SIZE; dx++)
                saved[dy * CURSOR_SIZE + dx] = bb[(cursor_y + dy) * SW + (cursor_x + dx)];
        saved_valid = 1;
    }
}
static void restore_bg(void) {
    if (saved_valid) {
        for (int dy = 0; dy < CURSOR_SIZE; dy++)
            for (int dx = 0; dx < CURSOR_SIZE; dx++)
                bb[(cursor_y + dy) * SW + (cursor_x + dx)] = saved[dy * CURSOR_SIZE + dx];
        saved_valid = 0;
    }
}
static void draw(void) {
    for (int dy = 0; dy < 18; dy++)
        for (int dx = 0; dx < 18; dx++)
            if ((dx + dy) % 3 != 0)
                bb[(cursor_y + dy) * SW + (cursor_x + dx)] = CURS;
}
/* FIXED render_all() cursor block. */
static void render_all_cursor(void) {
    int cw = CURSOR_SIZE, ch = CURSOR_SIZE;
    int cx0 = cursor_x, cy0 = cursor_y;
    int under_covered = 0;
    if (dmg_valid) {
        for (int i = 0; i < 1; i++) {   /* single damage rect in this model */
            if (dmg_x <= cx0 && dmg_y <= cy0 &&
                dmg_x + dmg_w >= cx0 + cw && dmg_y + dmg_h >= cy0 + ch)
                under_covered = 1;
        }
    }
    if (saved_valid && !under_covered) restore_bg();
    saved_valid = 0;
    save_bg();
    draw();
    dmg_valid = 0;
}
static int count_cursor_ink(void) {
    int n = 0;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++)
            if (bb[y * SW + x] == CURS) n++;
    return n;
}
static void draw_control(int x, int y, int w, int h) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            bb[yy * SW + xx] = 0x0080A0C0u;
}

int main(void) {
    for (int i = 0; i < SW * SH; i++) bb[i] = BG;

    cursor_x = 200; cursor_y = 200; dmg_valid = 0;
    render_all_cursor();
    printf("frame 1 (plain desktop)     : cursor ink = %d\n", count_cursor_ink());

    cursor_x = 210; cursor_y = 205;
    draw_control(210, 205, 60, 24);
    damage_add(210, 205, 60, 24);
    render_all_cursor();
    int f2 = count_cursor_ink();
    printf("frame 2 (over a control)    : cursor ink = %d\n", f2);

    cursor_x = 300; cursor_y = 300;
    draw_control(210, 205, 60, 24);
    damage_add(210, 205, 60, 24);
    render_all_cursor();
    int f3 = count_cursor_ink();
    printf("frame 3 (moved off control) : cursor ink = %d\n", f3);

    int here = 0;
    for (int dy = 0; dy < 18; dy++)
        for (int dx = 0; dx < 18; dx++)
            if (bb[(300 + dy) * SW + (300 + dx)] == CURS) here++;
    int ghosts = f3 - here;
    printf("\ncursor ink at current pos  : %d\n", here);
    printf("stale ink elsewhere (ghost): %d\n", ghosts);

    /* Also verify the "fully covered" case still skips the restore, so a window
     * sliding under the pointer keeps its fresh pixels. */
    for (int i = 0; i < SW * SH; i++) bb[i] = BG;
    cursor_x = 300; cursor_y = 300; dmg_valid = 0;
    render_all_cursor();
    /* Now a big window repaints over the whole cursor box. */
    draw_control(290, 290, 200, 200);
    damage_add(290, 290, 200, 200);
    render_all_cursor();
    int ctrl = 0;
    for (int y = 290; y < 490; y++)
        for (int x = 290; x < 490; x++)
            if (bb[y * SW + x] == 0x0080A0C0u) ctrl++;
    int covered = 0;
    for (int dy = 0; dy < 18; dy++)
        for (int dx = 0; dx < 18; dx++)
            if (bb[(300 + dy) * SW + (300 + dx)] == 0x0080A0C0u) covered++;
    printf("\n[big-window case] control pixels in window area: %d\n", ctrl);
    printf("[big-window case] cursor-box pixels that are control: %d/324\n", covered);
    printf("                 (must be 324 -> restore was correctly SKIPPED)\n");

    printf("\n%s\n", (ghosts == 0 && covered == 324)
        ? "RESULT: PASS - no ghost, and the fully-covered case still skips restore"
        : "RESULT: FAIL");
    return 0;
}
