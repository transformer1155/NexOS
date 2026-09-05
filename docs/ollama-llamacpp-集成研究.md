# Ollama 如何构建与调用 llama.cpp（调研报告）

> 目标：为 NexOS 把本地 AI 推理拉到「Ollama 级别」寻找务实路线。用户明确：不要从零手写向量化，而是研究 Ollama 是怎么集成 llama.cpp 的（构建 + 调用方式）。  
> 调研日期：2026-08-16

---

## 1. 构建（Build）：llama.cpp 是 vendored 子模块 + CMake 多后端

- **供应商化（vendoring）**：Ollama 把 llama.cpp 作为 git 子模块固定在 `llm/llama.cpp`，钉在某个 commit 上，而非 fork 整个树。
- **补丁机制**：Ollama 对上游的定制改动放在 `llama/patches/`，llama.cpp 升级时用 `make -f Makefile.sync apply-patches` 重新打回。这样既吃到上游更新，又保留自己的改动。
- **真正的构建 = llama.cpp 的 CMake**。后端通过 `-DGGML_*` 开关选择：

| 后端         | CMake 开关                                                  | 适用硬件                |
| :--------- | :-------------------------------------------------------- | :------------------ |
| CPU（SIMD）  | 自动检测；`GGML_NATIVE=ON`（默认）按构建机优化，产出支持 AVX/AVX2/AVX512 的二进制 | 任意 x86/ARM          |
| NVIDIA GPU | `-DGGML_CUDA=ON`                                          | CUDA 工具链            |
| Apple GPU  | `-DGGML_METAL=ON`（macOS 默认开）                              | Apple Silicon 统一内存  |
| 跨厂商 GPU    | `-DGGML_VULKAN=ON`                                        | Intel/AMD/NVIDIA 通用 |
| AMD GPU    | `-DGGML_HIP=ON`（`-DGPU_TARGETS=gfx...`）                   | ROCm                |
| Intel GPU  | `-DGGML_SYCL=ON`                                          | Intel Arc/核显        |
| CPU BLAS   | `-DGGML_BLAS=ON -DGGML_BLAS_VENDOR=OpenBLAS`              | 大 batch 加速          |

- 多个后端可同时编进一个二进制，运行时用 `--device` / `-ngl` 选择。
- **产物**：llama.cpp 静态库、`llama-server` 二进制、以及 `ollama runner`（通过 CGO 链接 llama.cpp）。

---

## 2. 调用（Invocation）：Runner 子进程隔离 + CGO 桥

- **双层架构（控制面 / 数据面分离）**：
  - **Go 控制面**（`ollama serve` 守护进程）：HTTP API、本地模型仓库、调度器、**硬件/显存检测**、**GPU 层卸载（offload）算法**（加载前算模型层数 / KV cache / 显存，决定把多少层卸到 GPU）。
  - **C++ 数据面**（llama.cpp）：最底层的张量计算、GEMM、注意力、KV cache，直接调硬件加速 API。
- **Runner 子进程隔离（关键设计）**：每加载一个模型，`ollama serve` 就 `fork`/`spawn` 一个独立的 `ollama runner` 子进程。
  - 理由：早期 Ollama 用 CGO 把 llama.cpp 直接嵌进 Go 进程，C++ 越界 / GPU 驱动崩溃（CUDA OOM、segfault）会拖垮整个服务。改成子进程后，**推理崩溃只杀 runner，主服务和其他并发请求不受影响**，进程退出时干净回收 VRAM。
- **CGO 桥** `llama/llama.go` → llama.cpp 的 C API：`llama_model_load_from_file`、`llama_decode`（生成循环）、`llama_tokenize` / `llama_detokenize`、`llama_chat_template_*`（**聊天模板**）、`llama_sampler_*`（采样）。
- **Runner 暴露最小 HTTP API**：`/health`、`/completion`、`/embedding`、`/tokenize`、`/detokenize`。守护进程通过 `LlamaServer` 接口与 runner 通信（本地 HTTP）。
- **调度器** `server/sched.go`：跟踪所有已加载 runner、串行化加载（同时只激活一个加载）、处理 keep-alive 超时驱逐。
- **Chat template**：由 llama.cpp 的 `llama_chat_template_*` 应用 GGUF 内嵌的 Jinja 模板（`tokenizer.ggml.chat_template`），自动完成 system/user/assistant 包装与 `<｜im_start｜>` 注入。**这正是我们当前缺的能力（gap #4）。**

---

## 3. 「Ollama 级速度」的秘诀：ggml-cpu 运行时 SIMD 派发

- llama.cpp 的 CPU 性能来自 **ggml-cpu 的运行时指令集派发**：它把多个内核变体（baseline/SSE2、AVX、AVX2+FMA、AVX512）分别编译，启动时用 **CPUID 检测** 选当前 CPU 支持的最优实现（如 `ggml_cpu_has_avx2()` 之类）。
- **核心认知**：Ollama 级速度不是「手写一个 AVX2 核」，而是「**编译多套调优核 + 运行时派发层**」。一个二进制在任何 x86 上都快。
- 对照我们现状：当前 `gguf_infer.cpp` 的 `dot_q*` 全是**纯标量循环、零向量化**、编译仅 `-O2`，这才是吞吐差距根源。

---

## 4. 对 NexOS 的启示与可行路线

**硬约束**：llama.cpp 假设宿主环境（libc、pthread、C++ STL、文件系统、malloc、动态链接）。NexOS 是 freestanding 内核（无 libc）。因此**不能直接照搬二进制**，需要适配。三条路线：

| 路线                                                                  | 做法                                                                                                                                                          | 评价                                                                   |
| :------------------------------------------------------------------ | :---------------------------------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------------- |
| **A. 移植 ggml-cpu + GGUF + chat template 进 64 位内核**（freestanding 适配） | 取 `ggml/src/ggml-cpu/`（SIMD 派发 + 量化反量化核）、GGUF 加载器、`llama_chat_template` 引擎、最小 C API 面。剥离/适配：pthread→我们的线程模型（先单线程）、malloc→我们的 arena 分配器、mmap/文件系统→我们的 FS 加载器 | **推荐**。最贴合「用 llama.cpp 的思路、别重造」；直接获得 AVX2 派发 + 全量化类型 + chat template |
| **B. 保留 `gguf_infer.cpp`，借鉴 llama.cpp 技术**                          | 复用其运行时 AVX2/FMA 派发模式 + 量化反量化核 + chat template，用我们的 freestanding 风格重写                                                                                        | 风险低、不破坏现有代码，但要达到同等水平需重复 llama.cpp 的大量工作                              |
| **C. 把 llama.cpp 当库，配最小 POSIX shim 编进 NexOS**                       | 提供 stub pthread/malloc/fs 让 llama.cpp 编过                                                                                                                    | 最重、最脆，shim 容易漏                                                       |

**推荐路线 A 的分阶段实施**（对应之前勘察的 6 大瓶颈）：

1. **GGUF 格式对齐**：我们已有解析器，确认与 llama.cpp 张量布局/量化几何一致（Q4_K_M 等已支持）。
2. **ggml-cpu 量化核 + AVX2/FMA 派发**（替换标量 `dot_q*`）：引入运行时 CPUID 派发，单核先吃满 SIMD。→ 解决 gap #1。
3. **chat template 引擎**（移植 `llama_chat_template_*`）：修 gap #4，是 QEMU 可直接验证的质量大提升。
4. **malloc/mmap 适配**：把 llama.cpp 的分配/文件映射接到我们的 arena 分配器与 FS，为更大模型铺路（gap #5/#6）。
5. **Runner 隔离思想**：把推理引擎放到独立内核任务/AP 核，崩溃不影响 GUI（对应 Ollama 的 runner 子进程隔离）。

---

## 5. 我们之前的瓶颈 ↔ Ollama 的答案（速查）

| 我们的瓶颈             | Ollama/llama.cpp 的答案                        |
| :---------------- | :------------------------------------------ |
| 纯标量 matmul、零 SIMD | ggml-cpu 运行时 SIMD 派发（多套 AVX/AVX2/AVX512 核）  |
| 单核无并行             | ggml 线程池（需我们补线程模型）；先单核吃满 SIMD               |
| 无 GPU/Vulkan/AMX  | `-DGGML_VULKAN`/`SYCL` 等后端；真机再接             |
| 无 chat template   | `llama_chat_template_*` 应用 GGUF 内嵌 Jinja 模板 |
| 模型被 4GB 封顶        | mmap + 分层卸载 + 运行时分配；需我们扩 arena/内存管理         |
| 量化类型不全            | llama.cpp 全量化核开箱即用                          |
| 单模型、无 mmap        | GGUF + 内容寻址模型仓库 + 运行时加载                     |

---

## 6. 结论

Ollama 没有「自己写推理内核」，而是**把 llama.cpp 作为 vendored 子模块，用 CMake 多后端编**系统。其中 **chat template（阶段 3）可在 QEMU 直接验证质量**，SIMD 派发（阶段 2）需在真机（Iris Xe i5）才显出提速。  
�
