# NexOS 托盘宿主 (nexos_tray.py)

WebView2 的 JS->Python Bridge 在这台机器上不可靠，因此后端的运行状态改为**直接显示在 Windows 系统托盘的图标与悬停提示**上。托盘宿主零第三方依赖（纯 ctypes + Win32 `Shell_NotifyIcon`），不依赖 pip / pywebview / WebView2。

## 启动

```bat
nexos            :: 启动托盘（默认）
nexos tray       :: 同上
nexos status     :: 打印自检信息后退出
nexos stop       :: 停止 gateway
nexos gui        :: 打开 WebView2 GUI（可选）
nexos cli ...    :: 透传给 nexos_cli.py
```

或直接 `python tools/nexos_tray.py`。

## 图标状态（这就是"后端状态可视化"）

| 图标 | 含义 | 悬停提示示例 |
|------|------|--------------|
| 🟢 绿环 + 白 N | gateway 在线 | `gateway ONLINE 127.0.0.1:19387 \| 3 pkgs` |
| 🔴 红环 + 白 N | gateway 已停止 | `gateway STOPPED (unreachable)` |

状态每 **2 秒**自动刷新（`SetTimer`），无需手动操作。

## 右键菜单

| 项 | 作用 |
|----|------|
| `status: ...` | 只读显示当前状态与已装包数量 |
| `Start gateway` | 拉起 `tools/net/gateway_server.py`（在线时置灰） |
| `Stop gateway` | 结束 gateway 进程（离线时置灰） |
| `Open GUI (WebView2)` | 启动 `nexos_gui.py` |
| `Open log` | 打开 `%TEMP%\nexos_gateway.log` |
| `Refresh status` | 立即刷新 |
| `Exit` | 移除图标并退出（同时结束 gateway） |

## 鼠标

- **左键单击** 立即刷新状态，并弹出气泡显示当前状态
- **左键双击** 打开 GUI
- **右键** 打开菜单

## 文件与日志

| 路径 | 说明 |
|------|------|
| `tools/nexos_tray.py` | 托盘宿主（零依赖） |
| `%TEMP%\nexos_tray.log` | 托盘自身日志 |
| `%TEMP%\nexos_gateway.log` | gateway 的 stdout/stderr |
| `%TEMP%\nexos_tray_green.ico` / `_red.ico` | 运行时生成的图标（32×32 32bpp） |

## 自检

```bat
nexos status
```

输出路径解析结果、gateway 探测结果、图标生成结果，用于排查环境问题。

## 实现要点

- 路径解析向上探测 `tools/` 目录，脚本放在任意子目录都能正确定位仓库根。
- 隐藏窗口类 + `NOTIFYICONDATAW`（完整 v4 结构，含 `guidItem`/`hBalloonIcon` 字段以保证 `cbSize` 正确）。
- 图标由 Python 手写 ICO 字节（`BITMAPINFOHEADER` + BGRA + AND mask），无需 Pillow。
- gateway 子进程用 `CREATE_NO_WINDOW` 启动，不闪黑框。
- `Stop gateway` 若没有子进程句柄，会按端口 19387 反查监听进程并结束（清理残留）。
