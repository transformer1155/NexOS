/* Verify the BUG-CURSOR1 fix end to end:
 *
 *   1. the snapshot records its own origin (saved_x/saved_y)
 *   2. restore_bg() writes back at that origin
 *   3. render_all() skips the restore only when the damage FULLY contains the
 *      cursor box
 *
 * The scenario that produced the ghost: the pointer advances, then a UI control
 * (a hover highlight) repaints, damaging only its own rect.
 */
#include <stdio.h>
#include <stdint.h>

#define SW 1024
#define SH 768
#define CURSOR_SIZE 32

static uint32_t bb[SW * SH];
static uint32_t saved[CURSOR_SIZE * CURSOR_SIZE];
static int saved_x, saved_y, saved_valid = 0;

#define BG   0x00102030u
#define CURS 0x00FFFFFFu
#define CTRL 0x0080A0C0u

static int cursor_x = 200, cursor_y = 200;
static int dmg_x, dmg_y, dmg_w, dmg_h, dmg_valid = 0;

static void damage_add(int x, int y, int w, int h) {
    dmg_x = x; dmg_y = y; dmg_w = w; dmg_h = h; dmg_valid = 1;
}
static void save_bg(void) {
    if (!saved_valid) {
        saved_x = cursor_x; saved_y = cursor_y;
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
                bb[(saved_y + dy) * SW + (saved_x + dx)] = saved[dy * CURSOR_SIZE + dx];
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
    if (dmg_valid &&
        dmg_x <= cx0 && dmg_y <= cy0 &&
        dmg_x + dmg_w >= cx0 + cw && dmg_y + dmg_h >= cy0 + ch)
        under_covered = 1;
    if (saved_valid && !under_covered) {
        int ox = saved_x, oy = saved_y;
        restore_bg();
        (void)ox; (void)oy;   /* damage would be dirty_add(ox,oy,cw,ch) */
    }
    saved_valid = 0;
    save_bg();
    draw();
    dmg_valid = 0;
}
static void draw_control(int x, int y, int w, int h) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            bb[yy * SW + xx] = CTRL;
}
static int count(uint32_t c) {
    int n = 0;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++)
            if (bb[y * SW + x] == c) n++;
    return n;
}
static int ink_in(int x, int y, int w, int h) {
    int n = 0;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (bb[yy * SW + xx] == CURS) n++;
    return n;
}
static int ink_in2(int x, int y, int w, int h, uint32_t c) {
    int n = 0;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (bb[yy * SW + xx] == c) n++;
    return n;
}

int main(void) {
    for (int i = 0; i < SW * SH; i++) bb[i] = BG;

    /* --- Scenario 1: plain move over controls, the reported bug --- */
    cursor_x = 200; cursor_y = 200; dmg_valid = 0;
    render_all_cursor();
    printf("S1 f1 desktop           cursor ink = %d\n", count(CURS));

    cursor_x = 210; cursor_y = 205;
    draw_control(210, 205, 60, 24); damage_add(210, 205, 60, 24);
    render_all_cursor();
    printf("S1 f2 over a control    cursor ink = %d\n", count(CURS));

    cursor_x = 300; cursor_y = 300;
    draw_control(210, 205, 60, 24); damage_add(210, 205, 60, 24);
    render_all_cursor();
    int s1 = count(CURS);
    int s1_here = ink_in(300, 300, 18, 18);
    printf("S1 f3 moved off         cursor ink = %d  (at new pos: %d, ghost: %d)\n",
           s1, s1_here, s1 - s1_here);

    /* --- Scenario 2: a big window slides fully under the cursor.
     *     The restore MUST be skipped so the fresh window pixels survive. --- */
    for (int i = 0; i < SW * SH; i++) bb[i] = BG;
    cursor_x = 300; cursor_y = 300; dmg_valid = 0;
    render_all_cursor();
    draw_control(290, 290, 200, 200);
    damage_add(290, 290, 200, 200);
    render_all_cursor();
    int s2_ctrl = ink_in2(300, 300, 18, 18, CTRL);
    int s2_ink  = ink_in(300, 300, 18, 18);
    printf("\nS2 big window over cursor: control=%d cursor=%d in the 18x18 box"
           "  (no BG pixels => restore correctly SKIPPED)\n", s2_ctrl, s2_ink);
    int s2_bg = ink_in2(300, 300, 18, 18, BG);
    printf("S2 background pixels left in the cursor box: %d  (0 => PASS)\n", s2_bg);

    int ghost = s1 - s1_here;
    printf("\n%s\n", (ghost == 0 && s2_bg == 0)
        ? "RESULT: PASS - ghost eliminated and full-coverage still skips restore"
        : "RESULT: FAIL");
    return 0;
}
