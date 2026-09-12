# Chromium 核心移植路线（NexOS 平台先补课）

> 目标：把 **Chromium 的核心**编译成 NexOS 生态里的产物。
> **非目标（本路线内不做）**：窗口/显示接入 —— 由后续单独处理（"串个窗户"）。
> 因此**启动时间与产物体积不在约束内**：允许重、允许慢。

---

## 0. 结论先行：为什么不能"直接编译"

| 项 | 实测 |
|---|---|
| `chromium-main.zip` | 1681 MB 压缩 / **4099 MB 解压** / **550,843 个条目** |
| Chromium 目标要求 | **64 位 only**（2016 起无 32 位 Linux 构建）、libc++(C++20)、pthreads/TLS、真 mmap(含 W^X)、多进程、数 GB 内存 |
| NexOS 现状 | **32 位主内核**；64 位内核存在但为次级路径（经 32→64 切换进入）；内核堆 **14 MiB(32 位) / 16 MiB(64 位)**；无 pthread/TLS/mmap/mprotect/ELF64 动态加载 |

所以"移植 Chromium"在当前形态下不是工作量问题，而是**前置条件缺失**。本路线按依赖顺序补齐这些前置条件。

---

## 1. 现状盘点（实测，2026-09-12）

### 已有（可复用）
- **PMM + 4 级分页 VMM**：`kernel64.cpp`（`.attic64/`）已有物理页分配、页表构造、`kmalloc/kfree`（16 MiB 链表首次适配堆，带 magic/合并）。
- **64 位内核可构建**：`make` 产出 `build/kernel64.bin`（约 821,400 字节），链接了 `gui64 / mforms64 / clr64 / net64 / proc64 / vfs64`。
- **32→64 切换通道**：`kernel.cpp: do_switch64()` + `.attic64/switch32to64.asm`；`KERNEL64_SECTORS=1610`，`kernel64.bin` 位于 LBA 2048。
- **图形/托管层**：线性帧缓冲 GUI、MiniCLR（跑 Roslyn 编译的 C#）、mforms、Win32 PE 加载器（`.attic64/chrome.c` 就是用 Win32 API 画的 Chrome 外观，**不是真 Chromium**）。
- **网络**：NE2000 + TCP/HTTP 客户端（`net_http_get`）。远端桌面 ABI：`GET /screen`（NXFB 帧）+ `POST /input`。

### 缺失（Chromium 的硬前置）
1. **64 位不是主内核**：主 GUI 走 32 位；64 位需手动切换（见 §1.1 基线）。
2. **无 libc / libc++**：只有 `usr/libc.c` 这个 32 位 Linux-compat 最小 shim，64 位侧没有可链接的 libc。
3. **无线程**：无 pthread/TLS/`clone`；`linux_compat.cpp` 的 futex 等待者上限 32，`mmap` 竞技场上限 24 区域（均在 32 位侧）。
4. **无真 mmap**：无文件映射、无 `MAP_SHARED`、无 `mprotect`；**V8 JIT 依赖 W^X 权限切换**，这正是当前完全缺口。
5. **无 ELF64 动态加载器**：Chromium 产物是多 `.so` 动态链接。
6. **内存量级差 2~3 个数量级**：Chromium 需要 GB 级，当前 RAM 64–256 MB、堆 14/16 MiB。

### 1.1 本次实测的基线（Stage 0 产出）
- 以 `0x501E=1` 启动 `build/os_v2.img`：**只关掉自动 GUI 并停在 32 位文本 shell**，**不会**进入 64 位（早期注释里的说法已过时）。
- 在文本 shell 用 monitor `sendkey` 输入 `switch` 后：串口日志仅 +30 字节，**无任何 64 位标记**，疑似未响应或静默失败。
  → **Stage 1 的第一件事就是把这个通道查清并修好**，否则后续所有验证都没有立足点。

---

## 2. 分阶段路线与验收标准

> 每阶段都必须"可验证"：给出一条命令 + 一条可判定的输出。

### Stage 1 · 64 位主内核
- **做**：让 64 位内核成为可直接引导的主路径；查清 `switch` 静默失败的原因；把堆与可用物理内存提到 ≥512 MiB；保留文本 shell 作为验证界面。
- **验收**：一条命令启动镜像即进入 64 位（含版本横幅），`mem`/`heap` 报告 ≥512 MiB，连续启动 10 次无 fault。
- **为什么第一**：Chromium 无 32 位构建；且后续 libc/libc++/Chromium 全部要在 64 位下编译与运行。

### Stage 2 · libc + x86_64-nexos 工具链
- **做**：定义 `x86_64-nexos` 目标；移植 musl（先 `malloc/free`、`string`、`stdio`、`errno`、`syscall` 封装、`setjmp`）；提供 crt0 与链接脚本。
- **验收**：用该工具链编译一个使用 `malloc/printf/memcpy/qsort` 的 C 程序，在 64 位内核里跑出正确输出。

### Stage 3 · libc++
- **做**：在 Stage 2 的 libc 上移植 libc++（异常、RTTI、`std::thread` 先打桩）。
- **验收**：编译并运行使用 `std::string`/`std::vector`/`std::unordered_map`/异常抛接的 C++20 程序。

### Stage 4 · 线程与同步
- **做**：`clone`/`pthread_create`、TLS（`fs` 基址 + `%fs:` 访问）、`futex`（wait/wake，容量提升到数千）、互斥/条件变量、原子操作完备。
- **验收**：`pthread` 压力测试（多线程累加 + 互斥 + 条件变量）结果正确，无丢唤醒。

### Stage 5 · 真 mmap / mprotect（W^X）
- **做**：匿名 + 文件映射、`MAP_SHARED`、`munmap`、`mprotect`；页表 NX 位；W^X 切换。
- **验收**：V8 风格场景——mmap 可写页 → 写入代码 → `mprotect` 为可执行 → 跳转执行成功。

### Stage 6 · ELF64 动态加载器
- **做**：`PT_LOAD` 映射、`PT_DYNAMIC` 解析、`DT_NEEDED` 递归加载、`R_X86_64_*` 重定位、`ld.so` 角色。
- **验收**：加载一个依赖两个 `.so` 的程序并运行成功。

### Stage 7 · 内存与内核服务放大
- **做**：物理内存管理扩到 GB 级；信号、时钟、定时器、`/proc` 式自省最小集。
- **验收**：在一个进程里分配 ≥1 GiB 并读写校验；V8 独立可执行程序跑通（`d8` 风格）。

### Stage 8 · Chromium 本体
- **做**：先 `base/`，再 `net/`，然后最小 `content`（headless，无 GPU/Skia 软件栅格化）。
- **验收**：`headless_shell` 能加载一个本地 HTML 并输出 DOM 文本。
- 说明：GN/Ninja 需要 `x86_64-nexos` 的 sysroot；此阶段开始才需要把 `vendor/chromium` 的内容拉下来。

---

## 3. 已知风险 / 明确的不可行声明

- **不要**把 Chromium 源码树放进本仓库：4.1 GB 会违反 `RULES.md` 的体积规则并导致 GitHub 拒收（`GH001`）。已按 submodule 方式只记录引用（见根目录 `.gitmodules`）。
- `chrome.c` / `chrome.exe`（`.attic64/`）是**用 Win32 API 画的 Chrome 外观**，与 Chromium 引擎无关；不要把它当成移植进度。
- 本路线**不产出**可用的浏览器窗口——显示接入由后续单独处理。
- Stage 1 的 `switch` 静默失败原因尚未定位，可能牵连 `switch32to64.asm` 与 64 位入口；这是当前最大的未知项。

---

## 4. 进度记录

| 日期 | 阶段 | 结果 |
|---|---|---|
| 2026-09-12 | Stage 0 | submodule 引用（`vendor/chromium` @ `a2bee68`）建立；64 位通道基线实测（文本 shell 不进 64 位、`switch` 无响应） |
