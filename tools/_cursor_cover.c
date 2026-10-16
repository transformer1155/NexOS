/* Decide whether "skip restore_bg" is safe.

   restore_bg() writes the SNAPSHOT taken before the cursor was drawn at this
   position.  Two cases:

     A) the UI repainted the whole cursor box this frame -> the backbuffer
        already has fresh pixels, and our snapshot is stale.  Restoring would
        overwrite them with the pre-repaint background.  SKIP is correct.

     B) the repaint covered only PART of the cursor box -> the pixels outside
        the repaint still hold the old cursor.  Restoring the snapshot is the
        only way to erase them.  RESTORE is required.

   So the condition must be "damage fully contains the cursor box", not
   "damage merely intersects it".

   This test enumerates the geometric cases and prints the decision.
 */
#include <stdio.h>

#define CURSOR_SIZE 32

static int contains(int dx, int dy, int dw, int dh, int cx, int cy) {
    return dx <= cx && dy <= cy &&
           dx + dw >= cx + CURSOR_SIZE && dy + dh >= cy + CURSOR_SIZE;
}
static int intersects(int ax, int ay, int aw, int ah,
                      int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

int main(void) {
    int cx = 210, cy = 205;
    struct { int x, y, w, h; const char* label; } cases[] = {
        { 210, 205,  60,  24, "control fully INSIDE cursor box (small button)" },
        { 205, 200,  64,  40, "control slightly LARGER than cursor box"        },
        { 210, 205, 200, 200, "large window covering the cursor box"           },
        { 210, 205,  60, 200, "tall strip left of cursor box"                  },
        { 200, 205, 100,  24, "strip right of cursor box"                      },
        { 210, 300,  60,  24, "control far below (no overlap at all)"          },
    };
    printf("cursor box = (%d,%d) %dx%d\n\n", cx, cy, CURSOR_SIZE, CURSOR_SIZE);
    printf("%-42s | int | fully_contains | OLD skip? | NEW skip?\n", "case");
    printf("----------------------------------------------------------------------------\n");
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        int inter = intersects(cx, cy, CURSOR_SIZE, CURSOR_SIZE,
                              cases[i].x, cases[i].y, cases[i].w, cases[i].h);
        int full  = contains(cases[i].x, cases[i].y, cases[i].w, cases[i].h, cx, cy);
        printf("%-42s |  %d  |       %d        |    %d     |    %d\n",
               cases[i].label, inter, full, inter, full);
    }
    printf("\nOLD = skip on any intersection (current code) -> case 1 leaves a ghost\n");
    printf("NEW = skip only when damage fully contains the cursor box\n");
    return 0;
}
