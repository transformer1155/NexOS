# win11-ui —— NexOS 标准 WinUI 3 控件体系

本目录承载 NexOS 的 **Web 端 UI 控件** 与 **WinUI 3 设计令牌**。控件实现来自
[WinUIonWeb](https://github.com/)（一个 ~90% 还原 Windows App SDK / WinUI 3 的
Vue 3 控件库），已转换为 NexOS 的标准 UI 来源。

## 目录结构

```
win11-ui/
├── index.html              # 入口页：应用标准 WinUI 3 字体栈，iframe 加载 ./webui
├── webui/                  # 标准 WinUI 3 控件站点（WinUIonWeb 的 Vite 构建产物）
│   ├── index.html          # 控件画廊入口（相对路径 ./assets/*，可直接 file:// 打开）
│   └── assets/             # 打包后的 JS/CSS/字体（含 SEGOEICONS.TTF 图标字体）
├── WinUIonWeb/             # 上游源码（Vue 3 + Vite + TypeScript）
│   └── src/
│       ├── styles/theme.css  # 标准 WinUI 3 设计令牌（颜色/字体/圆角/阴影的权威来源）
│       ├── assets/Fonts/     # SEGOEICONS.TTF（Segoe UI 图标字体）
│       └── components/       # ~80 个 WinUI 3 控件（WinButton / WinToggleSwitch / ...）
├── winui3-tokens.json      # 从 theme.css 抽取的标准 WinUI 3 设计令牌（单一事实来源）
├── SEGOEICONS.TTF          # 复制到仓库根 assets 供内核/Harness 引用的图标字体
└── legacy/index.html       # 原手写 Win11 桌面设计稿（备份，已被标准控件替换）
```

## 标准 WinUI 3 字体栈

```
'Segoe UI Variable', 'Segoe UI', 'Microsoft YaHei', system-ui, sans-serif
```

- 文本：Segoe UI（英文）/ Microsoft YaHei 微软雅黑（`sfs_files/msyh.ttf` 已有）。
- 图标：Segoe UI Icon Font（`SEGOEICONS.TTF`）。
- 基准字号 14px（Fluent 默认）。

## 设计令牌来源

`winui3-tokens.json` 是从 `WinUIonWeb/src/styles/theme.css` 抽出的标准 WinUI 3
调色板（浅色 + 深色），是所有 NexOS UI 表面的**唯一视觉来源**：

- `csharp/NexOS.Forms/Forms.cs` 的 `C` 颜色类与 `Theme` 已对齐到这些令牌
  （accent `0x0067C0`、文本 `0x171717/0x5E5E5E/0x737373`、描边 `0xE6E6E6` 等）。
- 任何新增 UI 控件都应引用 `winui3-tokens.json` 中的值，保持全平台一致。

## 如何重建 webui（更新控件后）

```bash
cd win11-ui/WinUIonWeb
npm install --no-audit --no-fund
npm run build-only          # 跳过类型检查，产出 dist/
# 把 dist/ 的相对路径修正后放回 webui/：
#   index.html 中 /WinUIonWeb/assets/... -> ./assets/...
#   Move-Item dist ../webui
```

> 注意：`vite build` 默认产出绝对路径 `/WinUIonWeb/assets/*`，
> 需改为相对路径 `./assets/*` 才能以 file:// 或任意子路径打开。

## 原生内核 GUI 的说明

NexOS 真实内核 GUI 由 `gui.cpp` 直接在 framebuffer 上绘制，使用位图字体
（`font16x16.h`，由 BIOS `font8x16` 缩放生成）与 `NexOS.Forms` 的 `C` 颜色类。
Web 控件（Vue）**不运行于内核**，而是供 Windows WinForms harness / WebView /
`win11-ui` 演示页使用。两者通过 `winui3-tokens.json` 与统一的 `C` 颜色类保持
视觉一致（均为标准 WinUI 3）。
