/* Reproduce the click-time "re-print" ghost.

   Sequence for a click on a UI control:

     1. handle_mouse_down()  -> render_all()
          - the control repaints in its PRESSED/hover state (damage = control)
          - the cursor block runs: damage does NOT fully contain the 32x32
            cursor box (the control is smaller), so restore_bg() RUNS
          - save_bg() now snapshots the backbuffer INCLUDING the pressed control
     2. handle_mouse_up()    -> no repaint at all (only repaints after a drag)
     3. the pointer moves off -> handle_mouse_move() -> restore_bg()
          - restores the snapshot, which CONTAINS the pressed control pixels
          - so the control stays drawn in its pressed state where the cursor was

   The user calls this "重印" (re-print): the pressed/hover highlight is printed
   back onto the screen by the cursor's own background restore.

   This test models that exact sequence and measures the leftover highlight.
 */
#include <stdio.h>
#include <stdint.h>

#define SW 1024
#define SH 768
#define CURSOR_SIZE 32

static uint32_t bb[SW * SH];
static uint32_t saved[CURSOR_SIZE * CURSOR_SIZE];
static int saved_x, saved_y, saved_valid = 0;

#define BG       0x00102030u   /* desktop                      */
#define CTRL_OFF 0x0060A0C0u   /* control, normal state        */
#define CTRL_ON  0x00FF8000u   /* control, pressed/hover state */
#define CURS     0x00FFFFFFu

static int cursor_x, cursor_y;
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
/* FIXED render_all() cursor block (full-containment test). */
static void render_all_cursor(void) {
    int cw = CURSOR_SIZE, ch = CURSOR_SIZE;
    int cx0 = cursor_x, cy0 = cursor_y;
    int under_covered = 0;
    if (dmg_valid &&
        dmg_x <= cx0 && dmg_y <= cy0 &&
        dmg_x + dmg_w >= cx0 + cw && dmg_y + dmg_h >= cy0 + ch)
        under_covered = 1;
    if (saved_valid && !under_covered) restore_bg();
    saved_valid = 0;
    save_bg();
    draw();
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

int main(void) {
    for (int i = 0; i < SW * SH; i++) bb[i] = BG;

    /* The control is drawn in its NORMAL state. */
    const int CX = 300, CY = 400, CW = 120, CH = 34;
    paint_control(CX, CY, CW, CH, CTRL_OFF);

    /* Pointer moves onto the control. */
    cursor_x = 330; cursor_y = 412; saved_valid = 0;
    /* hover highlight repaint: damage = the control only */
    paint_control(CX, CY, CW, CH, CTRL_ON);
    damage_add(CX, CY, CW, CH);
    render_all_cursor();
    printf("after hover-on      : highlight px = %d, cursor px = %d\n",
           count(CTRL_ON), count(CURS));

    /* CLICK: handle_mouse_down() repaints the control pressed and redraws the
     * cursor.  render_all_cursor() saves the background INCLUDING the pressed
     * highlight, because the control damage does not cover the whole cursor
     * box, so restore_bg() ran and then save_bg() re-snapshotted. */
    paint_control(CX, CY, CW, CH, CTRL_ON);
    damage_add(CX, CY, CW, CH);
    render_all_cursor();
    printf("after click (down)  : highlight px = %d\n", count(CTRL_ON));

    /* handle_mouse_up(): NO repaint happens for a plain click. */

    /* Pointer moves OFF the control.  handle_mouse_move() restores the saved
     * background -- which still contains the pressed highlight. */
    cursor_x = 600; cursor_y = 200;
    render_all_cursor();   /* models the restore at the old position */
    printf("after move-off      : highlight px = %d\n", count(CTRL_ON));

    int on_ctrl = 0;
    for (int y = CY; y < CY + CH; y++)
        for (int x = CX; x < CX + CW; x++)
            if (bb[y * SW + x] == CTRL_ON) on_ctrl++;
    printf("\nhighlight pixels inside the control rect: %d / %d\n", on_ctrl, CW * CH);
    printf("(the control should have returned to CTRL_OFF; %d > 0 means it is\n"
           " still showing the pressed highlight -> the reported re-print)\n", on_ctrl);

    printf("\n%s\n", on_ctrl == 0
        ? "RESULT: no re-print"
        : "RESULT: BUG REPRODUCED - the pressed highlight is re-printed by the cursor restore");
    return 0;
}
