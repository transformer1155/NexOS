#!/usr/bin/env python3
# gen_skill_files.py - emit .skill source (C++ subset) for every plugin into
# sfs_files/ so they bake into the MKFS/SFS image (flat 20-char names:
# src_<name>.skill).  Phase 3 makes plugin source readable/editable inside NexOS.
# Bodies use only the skillc-supported subset (int/uint32_t/bool/char, if/while/
# for, functions) so skillc can compile them in Phase 4.
import os

ROOT = os.path.dirname(os.path.abspath(__file__))
SFS  = os.path.join(os.path.dirname(ROOT), "sfs_files")

# name -> body (C++ subset; representative of the real plugin logic)
BODIES = {
"gfx_core": """
// gfx_core.skill - graphics primitives (C++ subset)
int g_width = 1024;
int g_height = 768;
int g_backbuffer[1024 * 768];
void put_pixel(int x, int y, int c) {
    if (x < 0 || y < 0 || x >= g_width || y >= g_height) return;
    g_backbuffer[y * g_width + x] = c;
}
void fill_rect(int x, int y, int w, int h, int c) {
    int j = 0;
    while (j < h) {
        int i = 0;
        while (i < w) { put_pixel(x + i, y + j, c); i = i + 1; }
        j = j + 1;
    }
}
int main() { fill_rect(0, 0, g_width, g_height, 0x1F1F1F); return 0; }
""",
"gfx_glass": """
// gfx_glass.skill - frosted glass
void glass_rounded_rect(int x, int y, int w, int h, int r, int c, int a) {
    if (a <= 0) return;
    fill_rect(x, y, w, h, c);
}
void glass_rect(int x, int y, int w, int h, int c, int a) {
    glass_rounded_rect(x, y, w, h, 0, c, a);
}
void glow_ellipse(int cx, int cy, int rx, int ry, int c) {
    fill_rect(cx - rx, cy - ry, rx * 2, ry * 2, c);
}
""",
"font_bitmap": """
// font_bitmap.skill - bitmap glyph blit
int font8x16_base = 0x3000;
void draw_char_bmp(int x, int y, int ch, int fg) {
    int row = 0;
    while (row < 16) {
        int bit = 0;
        while (bit < 8) {
            if ((font8x16_base + row) & (1 << bit)) put_pixel(x + bit, y + row, fg);
            bit = bit + 1;
        }
        row = row + 1;
    }
}
""",
"font_vector": """
// font_vector.skill - vector glyph stub
int vec_ready() { return 1; }
int vec_glyph(int cp, int px, int w, int h) { w = px; h = px; return 0; }
int vec_advance(int cp, int px) { return px; }
""",
"font_cjk": """
// font_cjk.skill - CJK drawing
int draw_cjk(int x, int y, int cp, int fg, int bg) {
    if (vec_ready()) { draw_char_bmp(x, y, cp, fg); return 16; }
    return 16;
}
""",
"wm_core": """
// wm_core.skill - window management
int g_win_count = 0;
void create_window(int x, int y, int w, int h) { g_win_count = g_win_count + 1; }
void draw_window(int id, int x, int y, int w, int h) { fill_rect(x, y, w, h, 0x2B2B2B); }
""",
"wm_anim": """
// wm_anim.skill - easing
int ease_out_cubic(int p) {
    int c = 256 - p;
    return 256 - (c * c * c) / (256 * 256);
}
int ease_in_out_cubic(int p) {
    if (p < 128) return (p * p * 4) / 256;
    int c = 256 - p; return 256 - (c * c * 4) / 256;
}
int anim_state = 0;
""",
"input_keyboard": """
// input_keyboard.skill - key handler
void handle_key(int ch) { if (ch == 27) anim_state = 0; }
""",
"input_mouse": """
// input_mouse.skill - mouse handlers
void handle_mouse_down(int x, int y) { create_window(x, y, 400, 300); }
void handle_mouse_move(int x, int y) { }
void handle_mouse_wheel(int d) { }
""",
"app_control_panel": """
// app_control_panel.skill
void app_control_panel() { create_window(40, 40, 500, 400); }
""",
"app_file_explorer": """
// app_file_explorer.skill
void app_file_explorer() { create_window(40, 40, 600, 400); }
""",
"app_task_manager": """
// app_task_manager.skill
void app_task_manager() { create_window(40, 40, 500, 300); }
""",
"app_calculator": """
// app_calculator.skill - Phase 4 demo target
int calc(int a, int b, int op) {
    if (op == 0) return a + b;
    if (op == 1) return a - b;
    if (op == 2) return a * b;
    if (op == 3) return a / b;
    return 0;
}
int main() {
    int r = calc(2, 3, 0);
    return r;
}
""",
"app_terminal": """
// app_terminal.skill
void app_terminal() { create_window(40, 40, 640, 400); }
""",
"app_browser": """
// app_browser.skill
void app_browser() { create_window(40, 40, 800, 500); }
""",
"theme_default": """
// theme_default.skill - colour constants (Phase 5 hot-swap target)
int C_BG        = 0x1F1F1F;
int C_PANEL     = 0x2B2B2B;
int C_ACCENT    = 0x0078D4;
int C_TEXT      = 0xF3F3F3;
int C_BORDER    = 0x323232;
int C_STARTMENU = 0x2D2D2D;
""",
}

for name, body in BODIES.items():
    fn = os.path.join(SFS, "src_%s.skill" % name)
    with open(fn, "w") as f:
        f.write("// " + "=" * 60 + "\n")
        f.write("// src/%s.skill  (NexOS plugin source, C++ subset)\n" % name)
        f.write("// " + "=" * 60 + "\n")
        f.write(body)
    print("wrote", fn)

print("done", len(BODIES))
