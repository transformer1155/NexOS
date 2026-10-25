/* Reproduce the cursor-ghost sequence from gui.cpp's render_all():
 *
 *   if (saved_valid && !under_covered) { restore_bg(); dirty_add(...); }
 *   saved_valid = false;
 *   save_bg();
 *   draw();
 *   dirty_add(...);
 *
 * `under_covered` is set when the cursor box intersects ANY damage rect this
 * frame.  That is meant to mean "the UI repainted under the pointer, so the
 * saved background is stale".  But the repaint usually covers only the control
 * rect, NOT the whole 32x32 cursor box -- so the cursor pixels outside the
 * control are never restored, and they persist as a ghost.
 *
 * This test simulates that geometry and reports the leftover stale pixels.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define SW 1024
#define SH 768
#define CURSOR_SIZE 32

static uint32_t bb[SW * SH];       /* backbuffer */
static uint32_t saved[CURSOR_SIZE * CURSOR_SIZE];
static int saved_valid = 0;

#define BG   0x00102030u     /* desktop background */
#define CURS 0x00FFFFFFu     /* cursor ink */

static int cursor_x = 200, cursor_y = 200;

/* --- damage node model (one rect, matching the simplified case) --- */
static int dmg_x, dmg_y, dmg_w, dmg_h;
static int dmg_valid = 0;

static void damage_add(int x, int y, int w, int h) {
    dmg_x = x; dmg_y = y; dmg_w = w; dmg_h = h; dmg_valid = 1;
}

/* rect a (cursor box) vs rect b (damage) */
static int intersects(int ax, int ay, int aw, int ah,
                      int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

/* ---- cursor primitives (verbatim from gui.cpp) ---- */
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
/* Draw an 18x18 cursor glyph at (x,y) -- the real bitmap is ~18x18 in a 32x32 cell. */
static void draw(void) {
    for (int dy = 0; dy < 18; dy++)
        for (int dx = 0; dx < 18; dx++)
            if ((dx + dy) % 3 != 0)                       /* arrow-ish shape */
                bb[(cursor_y + dy) * SW + (cursor_x + dx)] = CURS;
}

/* ---- the render_all() cursor block ---- */
static void render_all_cursor(void) {
    int cw = CURSOR_SIZE, ch = CURSOR_SIZE;
    int cx0 = cursor_x, cy0 = cursor_y;
    int under_covered = 0;
    if (dmg_valid && intersects(cx0, cy0, cw, ch, dmg_x, dmg_y, dmg_w, dmg_h))
        under_covered = 1;
    if (saved_valid && !under_covered) restore_bg();
    saved_valid = 0;
    save_bg();
    draw();
    dmg_valid = 0;   /* damage consumed by present() */
}

/* Count cursor-ink pixels still on screen (ghost detection). */
static int count_cursor_ink(void) {
    int n = 0;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++)
            if (bb[y * SW + x] == CURS) n++;
    return n;
}

/* Draw a control (button) rect, as the UI hover repaint would. */
static void draw_control(int x, int y, int w, int h) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            bb[yy * SW + xx] = 0x0080A0C0u;   /* hover highlight colour */
}

int main(void) {
    /* clean desktop */
    for (int i = 0; i < SW * SH; i++) bb[i] = BG;

    /* FRAME 1: cursor over plain desktop.  Nothing else damaged. */
    cursor_x = 200; cursor_y = 200;
    dmg_valid = 0;
    render_all_cursor();
    int after_f1 = count_cursor_ink();
    printf("frame 1 (plain desktop)     : cursor ink pixels = %d\n", after_f1);

    /* FRAME 2: mouse moved onto a button.  The button repaints with a hover
     * highlight -- damage covers ONLY the button (60x24), not the 32x32 cursor. */
    cursor_x = 210; cursor_y = 205;                 /* move 10,5 */
    draw_control(210, 205, 60, 24);                  /* the UI control repaint */
    damage_add(210, 205, 60, 24);
    render_all_cursor();
    int after_f2 = count_cursor_ink();
    printf("frame 2 (over a control)    : cursor ink pixels = %d\n", after_f2);

    /* FRAME 3: mouse moved OFF the button back to plain desktop.
     * The button repaints back to normal -> damage again only covers the button. */
    cursor_x = 300; cursor_y = 300;                 /* move away */
    draw_control(210, 205, 60, 24);                 /* restore normal look */
    damage_add(210, 205, 60, 24);
    render_all_cursor();
    int after_f3 = count_cursor_ink();
    printf("frame 3 (moved off control) : cursor ink pixels = %d\n", after_f3);

    /* Expected: exactly the pixels of ONE cursor (18x18-ish) are on screen.
     * A ghost shows up as MORE than that -- leftover ink at the old position. */
    printf("\nOne full cursor at the current position is expected.\n");
    int n = 0;
    for (int dy = 0; dy < 18; dy++)
        for (int dx = 0; dx < 18; dx++)
            if (bb[(300 + dy) * SW + (300 + dx)] == CURS) n++;
    printf("cursor ink at current pos (300,300): %d\n", n);
    int ghosts = after_f3 - n;
    printf("STALE cursor ink elsewhere (ghost): %d\n", ghosts);

    printf("\n%s\n", ghosts > 0
        ? "RESULT: BUG REPRODUCED - stale cursor pixels persist (ghost/trail)"
        : "RESULT: no ghost");
    return 0;
}
