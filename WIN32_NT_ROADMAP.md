# NexOS Win32 子系统 → 真正 NT 兼容层 路线图

> 目标：把 `win32.cpp` / `win32.h` / `winloader.cpp` 这套"玩具级"Win32 子系统，  
> 逐步扩展成能跑**越来越复杂**的真实 Windows 程序的兼容层，最终逼近能跑  
> Chrome 这类现代大型应用。
>
> 当前状态（调查结论）：加载器只收 i386 PE32；~40 个最基础 API；  
> GDI 仅为"矩形/文字/线条显示列表"；无文件 IO / 线程 / socket / COM / GPU。  
> 真实 Chrome 用 x64、上百 MB、导入成千上万符号、多进程 + GPU + 网络栈——**目前完全跑不了**。

---

## 0. 先讲清现实尺度（别盲目乐观）

| 兼容目标       | 代表软件                     | 需要的能力                         | 工作量参照                    |
| ---------- | ------------------------ | ----------------------------- | ------------------------ |
| **L0**（现状） | hello32.exe、画几个矩形的 GUI   | 当前已有                          | 已完成                      |
| **L1**     | 带文件读写 + 多线程的小工具          | 真堆/VM、线程、文件 IO、CRT            | 数周                       |
| **L2**     | 老式 32 位 GUI 程序（记事本级、老软件） | 完整 user32/gdi32、DC、控件、消息      | 数月                       |
| **L3**     | 需要 COM / 网络的程序（部分现代软件）   | ole32、winsock、advapi、shell    | 半年~1 年                   |
| **L4**     | **Chrome / 现代浏览器**       | 多进程 + IPC + 沙箱 + GPU + 全部 API | **数年 / Wine·ReactOS 级别** |

**结论**：Chrome 是 L4，不是"改几个桩"能到的。本路线图把 L0→L2 做成**每阶段都有可运行演示**的务实路径；  
L3→L4 只给方向，不保证在 hobby 规模内真正跑通 Chrome（Wine 做了 25 年、ReactOS 做了 20 年）。  
但**每一阶段产出的兼容层本身都有价值**，能跑 progressively 更复杂的真实 Windows 程序。

---

## 1. 关键架构决策（必须先定）

1. **位宽**：现代 Chrome 只发 x64。NexOS 已有 64 位内核路径  
   （`kernel64.cpp` / `switch32to64.asm` / `win32_64.o`），所以兼容层要**补齐 x64（PE32+）分支**：  
   解析 `machine==0x8664`、`magic==0x20B`、64 位重定位（REL64）、8 字节字段、64 位调用约定。  
   → 短期先让 32 位程序跑得更稳，中期切 x64 才能碰 Chrome。
2. **运行模型**：当前是"单映像 + 私有 bump 堆"。要支持多进程（Chrome 必需）需引入  
   "进程 = 独立地址空间映像 + 独立堆"的抽象，以及进程间 IPC（管道 / 共享内存）。
3. **GDI 后端**：弃用"显示列表"桥接，改为**真正的 HDC 实现**，后端直接画到 NexOS 帧缓冲  
   （已有 `gfx` 绘图原语可复用）。
4. **API 注册表**：把 `GetProcAddress`/导入解析从"返回 0 的桩"改成**真正的符号表查询**  
   （兼容层导出表 + 可加载的"伪 DLL"导出库）。

---

## 2. 阶段里程碑

### 阶段 1 — 加载器硬化（x64 + 真 DLL 模型）【L0→L1 基础】

- [ ] 支持 PE32+（x64）：机器类型、magic、REL64 重定位、8 字节 IAT。
- [ ] 真 `LoadLibraryA` / `GetProcAddress`：从兼容层导出表解析，未命中返回 NULL（不再假基址）。
- [ ] 依赖 DLL 导入解析：扫描 IAT 的 DLL 名，查"伪 DLL 导出库"（kernel32/user32/gdi32/… 的桩集合）。
- [ ] 支持 TLS 目录、延迟加载导入、.NET 检测保留拒绝。
- [ ] 取消 8MB SizeOfImage 硬上限（改为按可用内存动态分配）。
- [ ] **演示**：加载一个"空导入表的自包含 32/64 位 PE"并调用其入口，打印返回值。

### 阶段 2 — Kernel32 / CRT 实质化【L1】

- [ ] 真堆：`HeapCreate`/`HeapAlloc`/`HeapFree`（多堆，取代 bump）。
- [ ] 虚拟内存：`VirtualAlloc`(提交/保留/页对齐)/`VirtualFree`/`VirtualProtect`。
- [ ] 线程：`CreateThread`/`ExitThread`、关键段、`Event`/`Mutex`/`Semaphore`、`WaitForSingleObject`。
- [ ] 文件 IO：`CreateFile`/`ReadFile`/`WriteFile`/`SetFilePointer`/`FindFirst/NextFile`（接 NexOS VFS）。
- [ ] 时间：`GetTickCount`/`QueryPerformanceCounter`/`Sleep`；控制台：`WriteConsole`/`ReadConsole`。
- [ ] 完整 `msvcrt`：`printf`/`sprintf`/`memcpy`/`strlen`/…（接已有 libc 桩）。
- [ ] **演示**：跑一个**真做文件读写 + 开线程**的 32 位 Win32 小程序。

### 阶段 3 — User32 / GDI 实质化【L1→L2】

- [ ] 多窗口、z-order、子窗口、控件（`BUTTON`/`EDIT`/`STATIC` 真实行为）。
- [ ] 消息循环：定时器、键盘、鼠标（NexOS 输入 → Win32 消息）。
- [ ] 真 `HDC`：设备上下文、位图（`CreateCompatibleDC`/`BitBlt`/`DIB`）、画刷/画笔、字体度量、`GetTextExtent`。
- [ ] 用 NexOS 帧缓冲做 GDI 后端，淘汰 W32CmdKind 显示列表。
- [ ] **演示**：跑一个**带按钮/文本框/能打字**的 32 位 GUI 程序（记事本级）。

### 阶段 4 — 网络与系统库【L2→L3】

- [ ] Winsock：`WSAStartup`/`socket`/`connect`/`send`/`recv`（接 `net.cpp` 的 TCP）。
- [ ] `advapi32`：真实注册表（已有 sim）、服务/令牌桩、`crypt32` 轻量实现。
- [ ] `shell32`：`ShellExecute`/`SHGetFolderPath`；`wininet`/`winhttp`：HTTP 客户端（桥接到 `net.cpp`）。
- [ ] **演示**：跑一个**能联网下载网页**的 32 位 Win32 程序（自研小浏览器雏形）。

### 阶段 5 — COM 与现代应用底座【L3】

- [ ] `ole32`/`oleaut32`：`CoInitialize`/`CoCreateInstance`/基础 COM 对象与接口（大量现代程序依赖）。
- [ ] 最小化 COM 容器（类厂、引用计数、`QueryInterface`）。
- [ ] **演示**：跑一个**依赖 COM** 的 32 位程序（如简单 ActiveX/Shell 扩展demo）。

### 阶段 6 — 多进程 + 沙箱 + GPU（Chrome 专属）【L4，长期】

- [ ] 真 `CreateProcess`：子进程 = 独立映像 + 独立堆 + IPC（管道/共享内存）。
- [ ] Chrome 沙箱：受限进程令牌、到 broker 的 IPC。
- [ ] GPU 进程：软件光栅（如 SwiftShader）或 NexOS 帧缓冲后端；需要图形 API 抽象。
- [ ] **这是 Chrome 真正可跑的前提，仅在阶段 1–5 扎实后才可能。**

---

## 3. 建议的"第一个真动手"里程碑

路线图上**最值得先做的第一步**是 **阶段 1 + 阶段 2 的精简版**，目标：

> 让 NexOS 能运行一个"**做文件读写 + 开线程**"的真实 32 位 Win32 程序  
> （用 MinGW 交叉编译一个 hello-with-fileio.exe），并跑通。

这能立刻证明兼容层从"玩具"变成"真能跑 Windows 程序"，且工作量可控（数周），  
不需要先碰 x64 / COM / GPU 这些无底洞。

---

## 4. 风险与诚实提示

- **Chrome 不在可承诺范围内**：除非投入 Wine/ReactOS 级别的数年工程，否则无法保证跑通真实 Chrome。
- **x64 是硬前置**：不补 PE32+ 与 64 位内核用户态，连 Chrome 的入口都进不去。
- **每阶段都可独立交付演示**：即使最终没到 L4，L1/L2/L3 的兼容层对"在 NexOS 上跑旧 Windows 软件"已有实用价值。
- **建议节奏**：每完成一阶段就构建一个对应 demo 并截图/录屏验证，避免"写了几年还跑不出任何东西"。
