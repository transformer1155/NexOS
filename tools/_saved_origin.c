/* Focused proof of the saved-origin fix (BUG-CURSOR1).

   The scenario that broke: render_all()'s cursor block runs AFTER the pointer
   has already advanced, so cursor.x/y is the NEW position while `saved[]` still
   holds the OLD background.

     OLD code: restore_bg() wrote saved[] at (x,y)      -> new position
               => old cursor pixels never erased (ghost)
               => new location's fresh pixels destroyed
     NEW code: restore_bg() writes saved[] at (saved_x,saved_y) -> old position
               => ghost erased, new pixels intact

   This test asserts both halves: the ghost is gone AND the fresh content under
   the new position survives.
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
#define FRESH 0x00FF0000u   /* freshly painted content (must survive) */

static int cursor_x, cursor_y;

static void save_bg(void) {
    if (!saved_valid) {
        saved_x = cursor_x; saved_y = cursor_y;           /* NEW: remember origin */
        for (int dy = 0; dy < CURSOR_SIZE; dy++)
            for (int dx = 0; dx < CURSOR_SIZE; dx++)
                saved[dy * CURSOR_SIZE + dx] = bb[(cursor_y + dy) * SW + (cursor_x + dx)];
        saved_valid = 1;
    }
}
static void restore_bg(int use_saved_origin) {
    if (!saved_valid) return;
    int ox = use_saved_origin ? saved_x : cursor_x;        /* OLD vs NEW */
    int oy = use_saved_origin ? saved_y : cursor_y;
    for (int dy = 0; dy < CURSOR_SIZE; dy++)
        for (int dx = 0; dx < CURSOR_SIZE; dx++)
            bb[(oy + dy) * SW + (ox + dx)] = saved[dy * CURSOR_SIZE + dx];
    saved_valid = 0;
}
static void draw(void) {
    for (int dy = 0; dy < 18; dy++)
        for (int dx = 0; dx < 18; dx++)
            if ((dx + dy) % 3 != 0)
                bb[(cursor_y + dy) * SW + (cursor_x + dx)] = CURS;
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

/* Run one trial: 0 = old behaviour, 1 = fixed behaviour. */
static int trial(int use_saved_origin) {
    for (int i = 0; i < SW * SH; i++) bb[i] = BG;

    /* Frame 1: cursor at P1 over plain desktop. */
    cursor_x = 200; cursor_y = 200; saved_valid = 0;
    save_bg(); draw();                                    /* snapshot of BG at P1 */

    /* The pointer advances to P2.  The UI paints FRESH content at P2. */
    cursor_x = 260; cursor_y = 260;
    for (int y = 260; y < 300; y++)
        for (int x = 260; x < 300; x++)
            bb[y * SW + x] = FRESH;

    /* render_all() cursor block runs now: restore the old cursor, then
     * re-snapshot and redraw at the new position. */
    restore_bg(use_saved_origin);
    saved_valid = 0;
    save_bg();                                            /* snapshot at P2 */
    draw();                                               /* cursor at P2 */

    /* Measurements
     * - stale cursor ink: ink outside the current cursor box  => the ghost
     * - fresh content surviving: FRESH pixels that are NOT under the drawn
     *   cursor (the cursor legitimately covers those). */
    int ghosts = count(CURS) - count_in(260, 260, 18, 18, CURS);
    int fresh_kept = 0, fresh_total = 0;
    for (int y = 260; y < 300; y++)
        for (int x = 260; x < 300; x++) {
            uint32_t v = bb[y * SW + x];
            if (v == CURS) continue;               /* cursor covers it: fine */
            fresh_total++;
            if (v == FRESH) fresh_kept++;
        }
    printf("  %s : stale cursor pixels = %3d | fresh kept = %3d / %3d\n",
           use_saved_origin ? "FIXED (saved origin)" : "OLD   (live x/y)   ",
           ghosts, fresh_kept, fresh_total);
    return ghosts == 0 && fresh_kept == fresh_total;
}

int main(void) {
    printf("cursor advanced 200,200 -> 260,260, then render_all ran:\n");
    int a = trial(0);
    int b = trial(1);
    printf("\nOLD behaviour passes : %s\n", a ? "yes" : "NO");
    printf("NEW behaviour passes : %s\n", b ? "yes" : "NO");
    printf("\n%s\n", (!a && b)
        ? "RESULT: PASS - the fix is what makes both invariants hold"
        : "RESULT: inconclusive");
    return 0;
}
