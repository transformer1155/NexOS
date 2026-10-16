/* Validate the BUG-CURSOR2 fix: with transient damage under the pointer, the
 * snapshot must be discarded so the next frame captures settled pixels, but the
 * cursor must still be drawable and no cursor ink may be baked into a
 * background that is later restored.
 *
 * Frame model (what render_all_cursor does now):
 *
 *   intersects = damage overlaps the cursor box
 *   contained  = damage fully contains the cursor box
 *
 *   if (!contained)            restore old snapshot   (erase old cursor)
 *   if (intersects && !contained)  draw cursor, DISCARD snapshot
 *   else                          draw cursor, KEEP snapshot
 *
 * The invariant to check: whenever a snapshot is kept, it must contain NO cursor
 * ink.  Otherwise a later restore paints a ghost.
 */
#include <stdio.h>
#include <stdint.h>

#define SW 1024
#define SH 768
#define CURSOR_SIZE 32

static uint32_t bb[SW * SH];
static uint32_t saved[CURSOR_SIZE * CURSOR_SIZE];
static int saved_x, saved_y, saved_valid = 0;

#define BG       0x00102030u
#define CTRL_OFF 0x0060A0C0u
#define CTRL_ON  0x00FF8000u
#define CURS     0x00FFFFFFu

static int cursor_x, cursor_y;
static int dmg_x, dmg_y, dmg_w, dmg_h, dmg_valid = 0;

static void damage_add(int x, int y, int w, int h) {
    dmg_x = x; dmg_y = y; dmg_w = w; dmg_h = h; dmg_valid = 1;
}
static void save_bg(void) {
    saved_x = cursor_x; saved_y = cursor_y;
    for (int dy = 0; dy < CURSOR_SIZE; dy++)
        for (int dx = 0; dx < CURSOR_SIZE; dx++)
            saved[dy * CURSOR_SIZE + dx] = bb[(cursor_y + dy) * SW + (cursor_x + dx)];
    saved_valid = 1;
}
static void restore_bg(void) {
    if (!saved_valid) return;
    for (int dy = 0; dy < CURSOR_SIZE; dy++)
        for (int dx = 0; dx < CURSOR_SIZE; dx++)
            bb[(saved_y + dy) * SW + (saved_x + dx)] = saved[dy * CURSOR_SIZE + dx];
    saved_valid = 0;
}
static void draw(void) {
    for (int dy = 0; dy < 18; dy++)
        for (int dx = 0; dx < 18; dx++)
            if ((dx + dy) % 3 != 0)
                bb[(cursor_y + dy) * SW + (cursor_x + dx)] = CURS;
}

static void render_all_cursor(void) {
    int cw = CURSOR_SIZE, ch = CURSOR_SIZE;
    int cx0 = cursor_x, cy0 = cursor_y;
    int intersects = 0, contained = 0;
    if (dmg_valid) {
        if (cx0 < dmg_x + dmg_w && cx0 + cw > dmg_x &&
            cy0 < dmg_y + dmg_h && cy0 + ch > dmg_y) intersects = 1;
        if (dmg_x <= cx0 && dmg_y <= cy0 &&
            dmg_x + dmg_w >= cx0 + cw && dmg_y + dmg_h >= cy0 + ch) contained = 1;
    }
    if (saved_valid && !contained) restore_bg();
    if (intersects && !contained) {
        /* transient: erase only, draw nothing, keep the snapshot invalid */
        saved_valid = 0;
    } else {
        saved_valid = 0;
        save_bg();
        draw();
    }
    dmg_valid = 0;
}

static void paint_control(int x, int y, int w, int h, uint32_t col) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            bb[yy * SW + xx] = col;
}
static int count(uint32_t c) {
    int n = 0;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++)
            if (bb[y * SW + x] == c) n++;
    return n;
}
static int count_in(int x0, int y0, int w, int h, uint32_t c) {
    int n = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if (bb[y * SW + x] == c) n++;
    return n;
}

int main(void) {
    const int CX = 300, CY = 400, CW = 120, CH = 34;
    int violations = 0;

    for (int i = 0; i < SW * SH; i++) bb[i] = BG;
    paint_control(CX, CY, CW, CH, CTRL_OFF);

    /* --- click sequence --- */
    cursor_x = 330; cursor_y = 412; saved_valid = 0;

    /* hover on: transient highlight, damage = control only */
    paint_control(CX, CY, CW, CH, CTRL_ON);
    damage_add(CX, CY, CW, CH);
    render_all_cursor();
    printf("hover-on   : highlight=%d cursor=%d snapshot=%d\n",
           count(CTRL_ON), count(CURS), saved_valid);
    if (saved_valid) { printf("  VIOLATION: transient snapshot kept\n"); violations++; }

    /* click down: pressed state, same damage */
    paint_control(CX, CY, CW, CH, CTRL_ON);
    damage_add(CX, CY, CW, CH);
    render_all_cursor();
    printf("click-down : highlight=%d cursor=%d snapshot=%d\n",
           count(CTRL_ON), count(CURS), saved_valid);

    /* release: control returns to NORMAL, damage = control only */
    paint_control(CX, CY, CW, CH, CTRL_OFF);
    damage_add(CX, CY, CW, CH);
    render_all_cursor();
    printf("release    : highlight=%d\n", count(CTRL_ON));

    /* move off: no damage at all under the new position */
    cursor_x = 600; cursor_y = 200;
    dmg_valid = 0;
    render_all_cursor();
    printf("move-off   : highlight=%d cursor=%d snapshot=%d\n",
           count(CTRL_ON), count(CURS), saved_valid);

    int on = count_in(CX, CY, CW, CH, CTRL_ON);
    printf("\nhighlight left inside the control: %d  (0 = PASS)\n", on);
    if (on > 0) violations++;

    /* --- steady-state sequence: cursor over plain desktop, no damage --- */
    for (int i = 0; i < SW * SH; i++) bb[i] = BG;
    cursor_x = 300; cursor_y = 300; saved_valid = 0; dmg_valid = 0;
    render_all_cursor();
    printf("\nplain frame 1: cursor=%d snapshot=%d\n", count(CURS), saved_valid);
    if (!saved_valid) { printf("  VIOLATION: steady snapshot should be kept\n"); violations++; }

    /* A window slides fully under the cursor: fresh content must survive. */
    paint_control(290, 290, 200, 200, CTRL_OFF);
    damage_add(290, 290, 200, 200);
    render_all_cursor();
    int bg_leak = count_in(290, 290, 200, 200, BG);
    printf("window-over-cursor: background pixels inside window = %d (0 = PASS)\n", bg_leak);
    if (bg_leak > 0) violations++;

    printf("\n%s\n", violations == 0
        ? "RESULT: PASS - no re-print, steady snapshot kept, window content preserved"
        : "RESULT: FAIL");
    return 0;
}
