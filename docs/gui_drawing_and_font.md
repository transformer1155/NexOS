# NexOS GUI 绘图与字体渲染 — 学习笔记

> 源码位置：`bootloader/gui.cpp`（~435KB，所有绘图原语与字体逻辑）、`bootloader/font_vec.c`（TrueType 矢量光栅化）、`bootloader/zfont_data.h`（内嵌 GB2312 16×16 点阵）。
> 本文所有行号均指向 `gui.cpp`，用于对照精读。

---

## 1. 整体架构：双缓冲 + 格式转换管线

NexOS 的 GUI 不直接写显存，而是：

```
[ 软件绘制 ]  ->  backbuffer (0x00RRGGBB, 行优先, stride=width)
                         |
                  present() / present_rect()
                         |  (按 LFB 的 pitch 与 pixel_format 转换)
                         v
              LFB (硬件帧缓冲, 可能是 BGRX32/RGBX32/RGB24/RGB565, 且 pitch != width*bpp)
```

关键事实：
- `backbuffer` 是 `uint32_t[width*height]`，每个像素 `0x00RRGGBB`（R 在 bit16-23）。所有绘制函数都只认这一种内部格式，简单统一。
- 真正的硬件帧缓冲（LFB）格式五花八门，`present()` 负责把内部格式翻译成 LFB 格式（`gui.cpp:1503`）。

### 1.1 像素格式与 `pitch` 铁律

VBE/UEFI 在 `0x5000` 处放 `VbeInfo` 结构（`gui.cpp:127`）：

```c
uint32_t framebuffer_phys;   // 0x5000
uint16_t width;              // 0x5004
uint16_t height;             // 0x5006
uint8_t  bpp;                // 0x5008
uint16_t pitch;              // 0x5009  <-- 每行字节数，常 ≠ width*bpp
uint8_t  pixel_format;       // 0x500F  0=BGRX32 1=RGBX32 2=RGB24 3=RGB565
```

> **巨坑**：真实硬件（如 Intel Iris Xe）的 `pitch` 往往比 `width*bpp` 大（行按 16/32/256 字节对齐）。`present()` 与 `present_rect()` 必须按 `pitch` 逐行跨步，否则整屏会被压进第一行变成一条乱码条纹。**这是新手移植 GUI 最容易翻车的地方。**

`present()` 的格式分支（`gui.cpp:1503`）：
- `PXF_RGBX32`：交换 R/B 字节后写入（内存里是 R,G,B,X）。
- `PXF_RGB24`：每像素 3 字节打包。
- `PXF_RGB565`：把 `0x00RRGGBB` 拆成 5/6/5 位。
- 默认 `BGRX32`：x86 小端下 `uint32_t` 直接拷贝即正确。

`present_rect()`（`gui.cpp:1566`）是**局部翻屏**——只把脏矩形从 backbuffer 拷到 LFB，用于光标移动、时钟滴答，避免整屏撕裂和 CPU 浪费。

---

## 2. 像素与颜色模型

```c
typedef uint32_t Color;   // 0x00RRGGBB
```

最基础的原语（`gui.cpp:1667`）：

```c
inline void put_pixel(int x, int y, Color c) {
    if ((uint32_t)x >= width || (uint32_t)y >= height) return;
    backbuffer[y * width + x] = c;          // 硬替换，忽略 alpha
}
inline Color get_pixel(int x, int y) {
    if ((uint32_t)x >= width || (uint32_t)y >= height) return 0;
    return backbuffer[y * width + x];
}
```

`put_pixel` 是**硬替换**，半透明效果必须手动合成（见第 4 节）。

`fill_rect`（`gui.cpp:1677`）带边界裁剪，逐行 `memset` 风格填充，是所有实心面的底座。

---

## 3. 绘图原语

### 3.1 描边矩形与直线（Bresenham）

```c
void draw_rect(int x, int y, int w, int h, Color c) {
    for (int i = x; i < x + w; i++) { put_pixel(i, y, c); put_pixel(i, y + h - 1, c); }
    for (int i = y; i < y + h; i++) { put_pixel(x, i, c); put_pixel(x + w - 1, i, c); }
}
```

直线用经典 **Bresenham 整数算法**（`gui.cpp:1818`），全程整数加减与位运算，零浮点：

```c
void draw_line(int x0, int y0, int x1, int y1, Color c) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1, sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        put_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}
```

### 3.2 圆角矩形（真实四分之一圆弧）

`fill_rounded_rect`（`gui.cpp:1848`）的聪明做法：先把中间/两侧用普通矩形填满，再对四个角用**圆盘判别** `dx²+dy² ≤ r²` 抠出圆弧，得到真正的圆形角而非方块近似。

### 3.3 真正的抗锯齿圆角：SDF（有符号距离场）

`blend_rounded_rect`（`gui.cpp:1782`）不走逐像素判圆，而是用**到圆角矩形轮廓的有符号距离**决定覆盖度：

```c
int d = (qx==0 && qy==0) ? -r : gfx_isqrt(qx*qx + qy*qy) - r;  // 像素到轮廓的带符号距离(px)
int t = 128 - d * 256;            // (0.5 - d) * 256 → 内部>0, 边缘≈128, 外部<0
if (t <= 0) continue;             // 形状外
if (t > 256) t = 256;             // 完全覆盖
blend_pixel(px, py, c, (t * alpha) >> 8);   // 边缘 1px 平滑过渡
```

距离用**整数平方根** `gfx_isqrt`（`gui.cpp:1835`，逐位逼近，零 libm）计算，且只在薄边界环触发，大面积内部直接满覆盖 → 又快又平滑。这是现代 UI 圆角抗锯齿的极简实现范本。

---

## 4. Alpha 混合：从 sRGB 到线性光的进化

### 4.1 朴素 alpha-over（sRGB 空间）

```c
inline void blend_pixel(int x, int y, Color c, int alpha) {
    if (alpha <= 0) return;
    uint32_t d = backbuffer[y * width + x];
    int dr = (d>>16)&0xFF, dg=(d>>8)&0xFF, db=d&0xFF;
    int cr = (c>>16)&0xFF, cg=(c>>8)&0xFF, cb=c&0xFF;
    int r = (dr*(255-alpha) + cr*alpha)/255;   // 直接按 sRGB 分量插值
    // ... 写入 backbuffer
}
```

问题：在 sRGB（感知）空间混合，细的 AA 字体边缘会发灰、发糊。

### 4.2 Gamma 校正混合（本系统采用的"现代"做法）

解决之道：先在**线性光**空间混合，再转回 sRGB。为了 freestanding（不用 `pow`），把 256 条 `sRGB↔linear` 查找表**离线预计算**成常量数组（`gui.cpp:27` `G_TO_LIN[256]`、`gui.cpp:45` `G_TO_SRGB[256]`）。

```c
inline void blend_pixel_lin(int x, int y, Color c, int alpha) {
    if (alpha >= 255) { put_pixel(x, y, c); return; }
    uint32_t d = backbuffer[y * width + x];
    int dr = G_TO_LIN[(d>>16)&0xFF], ... ;   // sRGB → 线性光
    int cr = G_TO_LIN[(c>>16)&0xFF], ...;
    int r = (dr*(255-a) + cr*a)/255;          // 线性光里插值
    backbuffer[y*width+x] =
        ((uint32_t)G_TO_SRGB[r]<<16) | ((uint32_t)G_TO_SRGB[g]<<8) | G_TO_SRGB[b];  // 回 sRGB
}
```

`G_TO_LIN`/`G_TO_SRGB` 曲线呈 S 形（见 `gui.cpp:28-62`：低值被压到 0，高值迅速拉满），正是 gamma 2.4 的特征。

### 4.3 ClearType 风格子像素混合

矢量字形在 3× 水平分辨率光栅化（`vec_glyph`，见第 5 节），每个目标像素对应 R/G/B 三条独立覆盖率。`blend_subpixel`（`gui.cpp:1763`）把三条覆盖率分别按颜色通道合成，使竖笔在水平方向锐度提升约 3×：

```c
int r = (dr*(255-aR) + fr*aR)/255;   // R 通道用 its 自己的 coverage aR
int g = (dg*(255-aG) + fg2*aG)/255;  // G 通道用 aG
int b = (db*(255-aB) + fb*aB)/255;   // B 通道用 aB
```

`mix_colors`（`gui.cpp:2643`）是玻璃材质的线性空间颜色混合，三处玻璃函数都用它。

---

## 5. 字体渲染：三级降级链路

这是本系统最精妙的部分。`draw_char`（`gui.cpp:1961`）按优先级尝试三条路径，`g_font_mode`（默认 1）控制是否启用高级路径：

```
(1) 矢量 TrueType  (msyh.ttf, 64-bit 内核)
        ↓ 失败
(2) 烘焙 AA Latin  (font_la16.bin, 32-bit 内核高质量路径)
        ↓ 失败
(3) 1-bit 8×16 VGA 点阵  (BIOS 兜底, 像 DOS)
```

### 5.1 矢量路径（最佳质量，`gui.cpp:1965`）

```c
if (g_font_mode != 0 && vec_ready()) {
    const uint8_t* g = gc_get(c, g_font_px, &w, &h, &xo, &yo);  // 先查 LRU 缓存
    if (!g) {
        g = vec_glyph(c, g_font_px, &w, &h, &xo, &yo);          // 否则用 stb_truetype 光栅化 glyf
        const uint8_t* cp = gc_put(c, g_font_px, w, h, xo, yo, g); // 存入环形缓存
        if (cp) g = cp;
    }
    if (g) {
        if (bg != (Color)-1) fill_rect(gx, gy, w, h, bg);
        for (int row = 0; row < h; row++) {
            const uint8_t* src = g + row * (3 * w);     // 3× 水平超采样
            for (int col = 0; col < w; col++) {
                int aR = sample_cov(src, 3*w, col*3 - 1); // 取 R/G/B 三条覆盖率
                int aG = sample_cov(src, 3*w, col*3 + 0);
                int aB = sample_cov(src, 3*w, col*3 + 1);
                if (aR>4 || aG>4 || aB>4) blend_subpixel(gx+col, gy+row, fg, aR, aG, aB);
            }
        }
        return vec_advance(c, g_font_px);                // 真实字宽（比例字体）
    }
}
```

`vec_*` 在 `font_vec.c` 里实现：`vec_init(read_file)` 从 SFS 加载 TTF 并解析表（`font_vec.c:82`）；`vec_glyph(cp, px, ...)` 按像素高度光栅化（`font_vec.c:119`）。**32-bit 内核现在也链接真实的 `font_vec.o`**（FPU/SSE 已在 entry.asm 启用，内核堆 10 MiB 可容纳 2 MiB msyh.ttf 缓冲），矢量字体在 32-bit 下可用，走路径 (1)。

### 5.2 烘焙 AA Latin（32-bit 高质量路径，`gui.cpp:1997`）

`font_la16.bin` 是离线用 `tools/bake_la16.c`（基于 stb_truetype + 微软雅黑子集）烘焙的 16px 灰度覆盖率位图，格式（`gui.cpp:1278`）：

```
"LA16" + base(1B) + count(1B) + h(2B) + 每字形: [width(1B) | advance(1B) | h*h 灰度字节(行主序, 左起 MSB)]
```

加载器 `load_font_la16`（`gui.cpp:1288`）把它读进 `g_fla16`，渲染时直接按灰度值做 alpha 合成：

```c
if (g_font_mode != 0 && g_fla16) {
    const uint8_t* g = fla16_glyph(c, &w, &h);
    if (g) {
        for (int row = 0; row < h; row++)
            for (int col = 0; col < w; col++) {
                int a = sample_cov(src, w, col);
                if (a > 4) blend_subpixel(x+col, y+row, fg, a, a, a); // 灰度 = 单通道 coverage
            }
        return fla16_advance(c);
    }
}
```

### 5.3 兜底：1-bit 8×16 VGA 点阵（`gui.cpp:2014`）

BIOS 风格，无抗锯齿，像经典 PC。MSB-first 逐位取像素：

```c
const uint8_t* glyph = font8x16[c];
for (int row = 0; row < 16; row++) {
    uint8_t byte = glyph[row];
    for (int col = 0; col < 8; col++)
        if (byte & (0x80 >> col)) { ... put_pixel(x+col, y+row, fg); }
}
return 8;  // 固定字宽
```

### 5.4 CJK（中文）渲染

`draw_cjk`（`gui.cpp:2095`）逻辑同 ASCII：先试矢量（中文也走 `vec_glyph(cp, ...)` 真实轮廓），失败再查内嵌 `zfont_data.h` 的 **GB2312 16×16 点阵**（387 字，`zfont_find_unicode` 二分查找，`gui.cpp:2080`）。找不到的字画个方框占位（`gui.cpp:2118`）。

```c
const uint8_t* g = zfont_glyphs + idx * 32;   // 16 行 × 2 字节/行 = 32 字节/字
for (int row = 0; row < 16; row++)
    for (int half = 0; half < 2; half++) {
        uint8_t bits = g[row*2 + half];
        for (int b = 0; b < 8; b++) {
            int col = half*8 + b;
            Color c = (bits & (0x80>>b)) ? fg : bg;
            put_pixel(x+col, y+row, c);
        }
    }
```

### 5.5 混合 UTF-8 字符串

`draw_text_utf8`（`gui.cpp:2174`）逐字节解析 UTF-8：`<0x80` 走 `draw_char`，三字节 `0xE0` 头解析成 codepoint 后走 `draw_cjk`（ASCII 占 8px、CJK 占 16px，自动混排）。

### 5.6 字体初始化（`gui.cpp:8223`）

`gui_init` 按 `g_font_mode` 决定：
- 模式 0（BIOS）：只设 8×16 指标，不加载大字体省内存。
- 模式 1（默认）：`load_font_la()` 装多分辨率 Latin，`load_font_la16()` 装 AA 源，最后 `vec_init()` 装 TTF（仅 64-bit）。
- 全部优雅降级：只要某一级缺失，下一行 `draw_char` 自动落到更弱的路径，**缺资源也不崩**。

> 文字辅助：`draw_text_bold`（4 向 1px 描边仿粗体）、`draw_text_shadow`（偏移阴影做景深）等都在 `gui.cpp:2052` 附近，纯靠多次 `draw_char` 叠加实现。

---

## 6. 毛玻璃（Aero / Acrylic 质感）

Win11 风格的"磨砂玻璃"由**迭代盒状模糊 + 半透明染色**实现（`glass_rounded_rect` / `glass_rect`，`gui.cpp:2691` / `gui.cpp:2727`）。

### 6.1 可分离盒状模糊

`glass_blur_rect`（`gui.cpp:2654`）是经典的**两遍可分离模糊**：先水平、再垂直，用滑动窗口累加和避免重复求和：

```c
// 水平：backbuffer -> tmp
int sr=0,sg=0,sb=0,cnt=0;
for (int k=0; k<=s1; k++) { uint32_t p=bb[base+k]; sr+=...; }
for (int xx=0; xx<w; xx++) {
    tmp[yy*w+xx] = (sr/cnt)<<16 | (sg/cnt)<<8 | (sb/cnt);
    int addx = xx+r+1, remx = xx-r;
    if (addx<w) { ... sr+=...; cnt++; }   // 滑入右窗
    if (remx>=0) { ... sr-=...; cnt--; }   // 滑出左窗
}
// 垂直：tmp -> backbuffer（同结构）
```

### 6.2 多次迭代逼近高斯

单次盒状模糊有方块感；重复 `C_GLASS_BLUR_PASSES`（=2，`gui.cpp:501`）次即收敛成平滑的高斯状（`gui.cpp:2707`、`gui.cpp:2737`）：

```c
for (int p = 0; p < C_GLASS_BLUR_PASSES; p++)
    glass_blur_rect(g, x0, y0, bw, bh, blur_r, tmp);
```

### 6.3 模糊 + 染色 + 圆角蒙版

模糊后，只在圆角矩形**内部**用 `mix_colors` 把背景染上一层半透明玻璃色（`C_GLASS_TINT` 等、`gui.cpp:493`），圆角外保留清晰原像（`gui.cpp:2716` `if (d>0) continue;`）。一次典型调用（`gui.cpp:4097`）：

```c
glass_rounded_rect(gfx, mx, my, mw, mh, WIN_RADIUS,
                   C_STARTMENU_BG, C_STARTMENU_GLASS_A, C_GLASS_BLUR_R);
```

`C_GLASS_BLUR_R=6`、`C_GLASS_ALPHA=180`（`gui.cpp:496-497`）是观感/性能甜点；TCG 软模拟下 2 passes 最划算。

---

## 7. 丝滑动画：缓动曲线

窗口的开/关/最小化/还原都接入**三次缓动**，取代机器人的匀速线性（`gui.cpp:5551`）。全部 `int32` 运算（`1000³=1e9 < INT32_MAX`，无需 64 位）：

```c
static int ease_out_cubic(int p) {            // p: 0..1000
    if (p <= 0) return 0;
    if (p >= 1000) return 1000;
    int u = 1000 - p;
    return 1000 - (u*u*u)/1000000;           // 1 - (1-x)^3
}
static int ease_in_out_cubic(int p) {
    if (p < 500) return 4 * ((p*p*p)/1000000);
    int u = 1000 - p;
    return 1000 - 4 * ((u*u*u)/1000000);
}
```

`draw_window_animated`（`gui.cpp:5571`）按 `anim_state` 计算窗口的 `rx,ry,rw,rh,alpha`：
- 状态 1/2（开/关）：`ease_out_cubic` + 从下方滑入 `ANIM_SLIDE`。
- 状态 3（最小化）：`ease_in_out_cubic` 向任务栏塌缩，宽度/高度/透明度同步收束。
- 状态 4（还原）：反向展开。

---

## 8. 合成与缓存：脏矩形 + 托管/原生分层

### 8.1 脏矩形合成

`render_all()` 累积本帧所有重绘的包围盒，**只把脏矩形 `present_rect()` 翻到 LFB**（`gui.cpp:999`）。整屏重绘的组件（托管 C# 桌面）标记全屏脏矩形→退化为整屏翻；局部浮层（弹窗、IME 条、光标）保持小脏矩形，只拷那一条带。

### 8.2 托管桌面缓存 gotcha（关键坑）

`render_all()` 对托管（C#）桌面层有缓存：**仅当 `g_desk_needs_full` 为真或窗口集合签名 `cur_sig` 变化时才重绘桌面并整屏翻**（`gui.cpp:~5506`）。

- 登录锁屏、桌面内联重命名、IME 条都属于"桌面表面"而非窗口，`cur_sig` 恒定。
- 若只敲键盘不改窗口集合，会**跳过桌面重绘** → 现象是"按键缓冲已更新（串口/登录逻辑正常）但屏幕上的字符（如密码星号）不显示"。
- 修复：在任何修改桌面表面内容的按键路径里置 `g_desk_needs_full = true` 再 `render_all()`（已在 `handle_key` 桌面分支、`gui_toggle_ime`、ESC 取消 IME 处加）。

> 窗口内文本框由每帧窗口循环 `draw_window` 无条件重绘，不受影响。鼠标移动不触发重绘（光标归 VMMDev 硬件光标负责）。

---

## 9. 给学习者的工程经验清单

| 主题 | 要点 |
|------|------|
| **Pitch** | LFB 每行字节数 `pitch` 常 ≠ `width*bpp`，逐行跨步必须用它，否则整屏压成一条乱码带。 |
| **格式交换** | `RGBX32` 与 `BGRX32` 的 R/B 字节顺序相反，曾被写反导致红蓝反色。 |
| **内部格式统一** | 全部绘制只用 `0x00RRGGBB`，格式翻译集中在 `present()`，绘制代码零分支。 |
| **零浮点** | 整数平方根、Bresenham、SDF、缓动全用整数/位运算，freestanding 可编译。 |
| **Gamma 正确混合** | AA 文字/玻璃要在线性光空间合成，LUT 离线预计算绕开 `pow`。 |
| **三级字体降级** | 矢量 → 烘焙 AA → 1-bit 点阵，缺资源不崩；CJK 用内嵌 16×16 点阵。 |
| **脏矩形 + 分层缓存** | 只翻脏区省 CPU；但"桌面表面"变更要手动置 `g_desk_needs_full`，否则屏幕不刷新（经典"按键不显示"陷阱）。 |
| **毛玻璃 = 可分离模糊 ×N + 染色** | 单次盒模糊发方块，2~3 次迭代即近似高斯；圆角用 SDF 蒙版只在内部染色。 |
| **缓动用 int32** | 三次缓动全程整数，斜率自然且不会溢出。 |

---

## 10. 最小阅读路线

1. `gui.cpp:1667` `put_pixel` / `fill_rect` —— 一切的起点。
2. `gui.cpp:1503` `present()` —— 理解 backbuffer→LFB 的格式与 pitch。
3. `gui.cpp:1961` `draw_char` —— 字体三级降级的总入口。
4. `gui.cpp:2654` `glass_blur_rect` + `2691` `glass_rounded_rect` —— 毛玻璃实现。
5. `gui.cpp:1763` `blend_subpixel` + `27/45` LUT —— gamma 正确混合。
6. `gui.cpp:5555` 缓动 + `5571` 窗口动画 —— 丝滑体验来源。
7. `gui.cpp:~5506` 桌面缓存分支 —— 理解"为什么有的更新屏幕不刷新"。
