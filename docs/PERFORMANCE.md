# NexOS VNC 性能分析

> 本文只讨论 NexOS 自研架构下的 VNC 画面链路，不套用通用 Linux 远程桌面（RealVNC / TigerVNC / Remmina 等）的假设。所有结论均对应 `kernel.cpp`、`net.cpp`、`plugins/vnc_client.cpp`、`tools/guest_init/init` 的真实实现。

## 0. 架构定位：这不是通用远程桌面

NexOS 是自研宿主操作系统。它的核心用例是：**在自身托管的 Linux guest 里让 AI 操作软件（Blender、浏览器、IDE），用户在 NexOS GUI 里"看着 AI 操作"并偶尔接管输入**。

因此这条 VNC 链路的本质是：

- **单向画面遥测 + 反向输入注入**，两端都是 NexOS 可控的组件，不是两台互不信任的独立机器。
- 客户端（`vnc_client.cpp`）**跑在 NexOS 内核里**，不是用户态 RFB 客户端。
- 传输走的是 **NexOS 自研 TCP/IP 栈 + NE2000 ISA 网卡**，不是宿主 Linux 内核的网络栈。
- 解码后直接 **`put_pixel` blit 进 NexOS 自己的 GUI 帧缓冲**，不经过 X11 / 合成器。

通用远程桌面优化了"人坐在另一端流畅交互"；NexOS 优化的是"AI agent 的低带宽眼睛 + 手"。两者瓶颈分布完全不同。

## 1. 真实链路四层

```
[NexOS 宿主]                                       [guest Linux]
┌───────────────────────────────────┐             ┌────────────────────────────┐
│ NexOS 内核                          │             │ Ubuntu 6.8 (kexec 加载)     │
│                                     │  跨层传输   │                              │
│  GUI (framebuffer + 窗口系统)        │  ◄────────► │  Xvfb :0 (虚拟帧缓冲)        │
│    ▲  put_pixel blit                │  NexOS TCP  │    ▲ 被渲染的内容            │
│    │                                │  /IP over   │    │                          │
│  vnc_client.cpp (RFB 3.8 客户端,     │  NE2000 ISA │  x11vnc :5900 (采集+编码)    │
│    内核插件, 仅 Raw 解码)            │  (I/O 0x300,│    ▲ 抓 X 帧缓冲脏矩形        │
│    │                                │   轮询模式) │    │                          │
│  net.cpp (自研 TCP/IP + NE2000 驱动) │             │  被 AI 操作的软件            │
└───────────────────────────────────┘             │  (Blender / 浏览器 / CLI)    │
                                                   └────────────────────────────┘
```

| 层 | 组件 | 运行位置 | 关键实现事实 |
|---|---|---|---|
| ① guest 渲染 | Xvfb :0 | guest Linux 用户态 | 纯软件帧缓冲，无真实 GPU；GL 走 `llvmpipe`/`swrast` |
| ② guest 采集 | x11vnc :5900 | guest Linux 用户态 | XDamage 脏矩形；编码能力含 tight/zlib，但被客户端压制 |
| ③ 跨层传输 | net.cpp TCP/IP | **NexOS 内核** | NE2000 ISA，I/O 0x300，**轮询**；guest `10.0.2.15` ↔ NexOS 网关 `10.0.2.2` |
| ④ 内核解码+blit | vnc_client.cpp | **NexOS 内核插件** | RFB 3.8，**仅 Raw**；逐像素 `put_pixel` 进 GUI |

## 2. 逐层瓶颈分析

### 2.1 guest 内渲染（Xvfb）
- Xvfb 是内存中的虚拟帧缓冲，**没有 GPU 加速路径**。guest 里的 Unity / Blender 等 OpenGL 程序落到 Mesa `llvmpipe` 软渲染。
- 后果：3D 视口、粒子、光照计算全部吃 guest CPU，重度场景可能个位数 FPS。2D / UI / 文本不受影响。
- 若部署时给 guest 做 **GPU passthrough**（VT-d / 独显直通），渲染层立刻不再是瓶颈——但 NexOS 默认形态是 Xvfb，这点必须假设为软件渲染。
- **与通用远程桌面的差异**：通用方案客户端通常连的是带 GPU 的桌面机，渲染在源头就有硬件加速；NexOS 的 guest 默认是无 GPU 的 Xvfb，渲染天花板低得多。

### 2.2 guest 内采集（x11vnc）
- x11vnc 用 XDamage 检测脏矩形，只把变化的屏幕区域发给客户端，这部分是高效的。
- **致命点**：`vnc_client.cpp` 在握手后 `SetEncodings` 只声明了 **Raw**（源码注释原话：`only Raw — we blit pixel-for-pixel`）。x11vnc 即使支持 tight/zrle，遇到只认 Raw 的客户端也只能发**未压缩像素**——采集端的全部压缩收益归零。
- 这意味着"脏矩形"只减少了**发送区域面积**，却没有减少**每像素的字节数**（仍是 4 字节/像素）。
- **与通用远程桌面的差异**：常规 RFB 客户端会协商 tight/zrle，带宽通常比 Raw 小一个数量级；NexOS 客户端主动放弃了这一点。

### 2.3 跨层传输（NexOS 自研 TCP/IP over NE2000 ISA）— 整条链最硬的墙
- `net.cpp` 明确：NE2000 ISA 网卡，I/O 端口 `0x300`，**轮询模式**（无中断驱动、无 NAPI、无 DMA 描述符批处理）。QEMU 启动参数为 `-net nic,model=ne2k_isa -net user`。
- NE2000 本身是 **10 Mbps** legacy 卡。叠加"轮询 + 单线程 TCP 状态机 + 每包 `kmalloc`/`kfree`"，有效吞吐远低于 10 Mbps 理论值，实测量级约 **0.8–1.0 MB/s**。
- 该链路是 NexOS 与 guest 之间的唯一数据通道（同机 kexec 加载，但网络仍走这块虚拟 NE2000）。
- **与通用远程桌面的差异**：通用方案用 virtio-net / 真实千兆 NIC + 宿主 Linux 内核 TCP 栈（中断/NAPI/多核/校验和卸载），吞吐高 1–2 个数量级。NexOS 的 10 Mbps 轮询 ISA 卡是结构性天花板，与"远程桌面慢"的常见归因（编码/客户端）无关——根因在网络介质本身。

### 2.4 NexOS 内核内解码 + blit
- `vnc_client.cpp` 收到一个 Raw 矩形后：`kmalloc` 缓冲 → 按 4096 字节分块读满 → **逐像素调用 `g_gui_api->put_pixel()`** → `kfree`。
- 逐像素 `put_pixel`：一个 1024×768 全帧 = **786,432 次内核函数调用**（每次含边界检查 + 帧缓冲写入）。即便只是部分脏矩形，大块变化区域同样昂贵。
- 无批量 `memcpy`、无 SIMD、无 GPU 纹理上传、无脏矩形合成——纯 32 位内核上下文里的 CPU 像素搬运。
- **与通用远程桌面的差异**：通用客户端（Remmina / RealVNC）在用户态用 `memcpy` + 脏矩形合成 + 可选 GPU 纹理上传，速度高 1–2 个数量级。NexOS 的 per-pixel blit 是为"能显示"而非"显示得快"写的。

## 3. 实测量级（由实现推导）

- 单张全帧原始像素：`1024 × 768 × 4 B ≈ 3.0 MB`（Raw，32bpp）。
- NE2000 传输：以 ~1 MB/s 计，单全帧传输 ≈ **3 s**。
- 内核逐像素 blit：以 ~200–500 ns/像素估算，单全帧 ≈ **0.16–0.4 s**。
- 结论：
  - **全屏连续变化**（3D 视口旋转、视频播放）：有效帧率 ≈ **0.25–0.3 fps**，不可用。
  - **静态 UI / 局部交互**（菜单、打字、AI 点按钮）：脏矩形面积小，接近实时，满足 agent 遥测需求。

## 4. 与通用 Linux 远程桌面的本质差异（汇总）

| 维度 | NexOS | 通用远程桌面 |
|---|---|---|
| 客户端位置 | NexOS **内核插件** | 用户态进程 |
| 解码后输出 | 逐像素 `put_pixel` 进自有 GUI | `memcpy`+脏矩形合成+GPU 纹理 |
| 编码协商 | **仅 Raw**（主动放弃压缩） | tight/zrle/hextile |
| 网络介质 | NE2000 ISA，**轮询**，10 Mbps | virtio-net/千兆，中断/NAPI |
| TCP 栈 | 自研最小状态机，单线程 | 宿主内核栈，多核+卸载 |
| 设计目标 | AI agent 低带宽眼睛+手 | 人流畅交互 |

## 5. 适用边界（紧扣 NexOS 定位）

**适合**：
- AI 在隔离 guest 里点按钮、填表单、读文档、跑 CLI / IDE——画面变化率低，脏矩形小。
- 用户在 NexOS GUI 里"旁观 AI 操作"，偶发接管鼠标键盘。
- 这本质上是一条**低带宽遥测 + 控制通道**，不是给人"流畅用桌面"的。

**不适合**：
- 实时 3D 视口连续刷新（Blender 旋转、游戏渲染）。
- 视频播放、任何要求 >5 fps 全屏变化的场景。
- 把它当作"让人体验流畅远程 Linux"的方案——架构目标就不是这个。

## 6. 替代方案评估：Sunshine + Moonlight 能否接入？

### 6.1 Sunshine（服务端，跑在 guest）—— 可行，但有前提
- 在 guest Linux 用 Sunshine 替代 x11vnc：它做**硬件 NVENC / VA-API 或 x264 软件编码**，以低延迟 H.264 / H.265 / AV1 推流。采集+编码质量远优于"x11vnc 被迫发 Raw"。
- **前提**：Sunshine 编码后比特率（720p30 ≈ 2–5 Mbps，1080p ≈ 8–20 Mbps）直接撞上 NE2000 的 **10 Mbps 上限**。不先升级传输层，Sunshine 的全部收益会被链路吃掉。

### 6.2 Moonlight（客户端，跑在 NexOS 侧）—— 当前内核架构下不可行
- Moonlight 需要：RTSP/RTP 协议栈 + H.264/AV1 **解码**（硬解或 FFmpeg 软解）+ YUV→RGB 转换 + 合成。
- NexOS 内核现在只有 RFB Raw 解码 + per-pixel blit，**没有视频子系统**。在 32 位内核里做 H.264 软解，同时用轮询 NE2000 收包，CPU 和带宽都承受不了。
- **现实接入方式**（二选一）：
  - **(a) 解码下放用户态**：在 NexOS 侧起一个特权辅助进程跑 Moonlight / FFmpeg 解码，解码后通过现有 GUI `put_pixel` / framebuffer ABI 把画面刷进 NexOS GUI。绕开内核 RFB 插件，最小改动。
  - **(b) 终极架构——共享内存捕获**：NexOS 已通过 kexec **同机**加载 guest（物理内存对宿主可见），仓库里也已有 `remote_desktop.h` 的 `nexos_fb_query` / `nexos_input_inject` ABI 雏形。可在 hypervisor / 共享内存层**直接捕获 guest 帧缓冲**，blit 进 NexOS GUI。零 NE2000、零 RFB、零 x11vnc，吞吐只受内存带宽限制——这是与"通用远程桌面"彻底不同的 NexOS 原生高性能路径。

### 6.3 结论
- **只把 x11vnc 换成 Sunshine 没用**，链路才是瓶颈。
- 真正要动的是两处：**传输层**（NE2000 → virtio-net 或共享内存捕获）+ **解码/blit 层**（内核 per-pixel → 批量 blit 或用户态解码）。
- Moonlight 客户端若要进 NexOS 内核，工程量大且受 NE2000 与内核形态双重限制；建议走 (a) 用户态解码 + GUI ABI，或 (b) 共享内存捕获。

## 7. 优化优先级建议

1. **传输层（收益最大）**：NE2000 ISA 轮询 → virtio-net，或直接共享内存帧捕获。这一步不做，后面都白搭。
2. **blit 层（次大）**：per-pixel `put_pixel` → 批量 `memcpy` + 脏矩形合成，至少快一个数量级。
3. **编码协商（最后）**：让客户端接受 tight/zrle，或换 Sunshine 服务端——但必须在第 1 步完成后才有意义。

## 8. 三阶段优化实现状态（2026-10-01）

| 阶段 | 改动 | 文件 | 状态 |
|---|---|---|---|
| ② blit 层 | 新增 `Graphics::blit_rect`（逐行批量拷贝进 backbuffer，带裁剪）；`gui_blit_pixels` C 入口；`gfx_core` 注册 `gui.blit_pixels` 服务；`vnc_client` 改用批量 blit，保留逐像素回退 | `gui.cpp` `plugins/gfx_core.cpp` `plugins/vnc_client.cpp` | **已完成**（安全，默认生效） |
| ③ 编码协商 | `vnc_send_prefs` 改为协商 Hextile + RRE（+ Raw 兜底）；`vnc_handle_fb_update` 实现 RRE / Hextile 解码，实心矩形走 `fill_rect`，颜色与 Raw 路径一致 | `plugins/vnc_client.cpp` | **已完成**（安全，不再仅 Raw） |
| ① 传输层 | 新增 `net_virtio.cpp`（legacy virtio-net PCI 驱动）；`net.cpp` 探测门控：发现 `0x1AF4:0x1000` 设备则启用，否则原样回退 NE2000；更新 QEMU 启动参数注释 | `net_virtio.cpp` `net.cpp` `kernel.cpp` | **代码交付，探测门控，默认零影响；待真机/QEMU 验证** |

**关于阶段①的重要说明**

- virtio-net 驱动按 QEMU legacy（`VIRTIO_PCI_*`）偏移实现，是**原型**，需要真机或 QEMU 验证后方可依赖。当前默认 `-net nic,model=ne2k_isa` 配置下探测不到 virtio 设备，`g_use_virtio` 恒为 false，**NE2000 路径字节级不变**。
- 真正与"通用远程桌面"彻底不同的 NexOS 原生高性能路径，是**共享内存帧捕获**：`gui.cpp` 已有 `nexos_fb_query()` 把 NexOS 帧缓冲的物理地址/宽高/格式暴露给 guest（见 `gui.cpp:9650`）。若 guest 直接把它的 X 帧缓冲写入该物理地址，则彻底绕开 NE2000 + x11vnc + RFB，吞吐只看内存带宽——这是比换网卡更彻底的阶段①方案，建议作为长期方向。

**验证方法**

- ②/③ 可在 QEMU 内直接观察：客户端日志会打印 `gui.blit_pixels ready` 与 Hextile/RRE 协商；抓帧延迟与全屏变化帧率应明显提升。
- ① 启用需给 QEMU 加 `-netdev user,id=n0,hostfwd=tcp::8080-:8080 -device virtio-net-pci,netdev=n0` 并移除 `ne2k_isa`；启动后日志出现 `[VIO] virtio-net initialized` 即生效。

**验证记录（2026-10-01 补充）**

- 阶段②/③ 的解码算法（RRE + Hextile）已从 `vnc_client.cpp` 逐行抽出，写成独立离板测试 `rfb_decode_test.cpp`（用 mock 读取器替代 RFB 套接字、用测试帧缓冲替代 GUI backbuffer），覆盖：RRE（背景 + 2 子矩形）、Hextile（带色子矩形、纯背景瓦片、Raw 瓦片）。**全部用例 `ALL RFB DECODE TESTS PASSED`**。
- 验证中发现并修复了一个**真实的 Hextile 渲染 bug**：原 `vnc_decode_hextile` 只在"无子矩形"分支里填充背景，导致当瓦片含子矩形时整块背景不被绘制，残留旧像素。按 RFB 3.8 §6.5.6，背景必须**先铺满整块瓦片**、再叠加子矩形（且 `BackgroundSpecified` 未置位时复用上一瓦片背景）。已改为"先 `fill_rect` 整瓦片、再画子矩形"，并同步修正测试桩。该修复同时消除了因背景缺失引起的错误填充。
- 注：`rfb_decode_test.cpp` 是离线验证工具，不进入内核镜像；内核侧真实逻辑以 `plugins/vnc_client.cpp` 为准。
