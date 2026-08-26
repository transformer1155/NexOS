# NexOS AI 引擎移植清单（llama.cpp → NexOS）

> **路线决定**：用户于 2026-08-16 选择「严格路线 A 全套移植」——新建平行 ggml 子系统（`gguf_loader.c` / `ggml_quantize.c` / `ggml_cpuid.c` / `ggml_dispatch.c` / `ggml_impl.c` / `llama_chat_template.c` / `model_loader.c` / `arena_allocator.c` / `kv_cache.c`），最终替换现有 `gguf_infer.cpp`。
> **使用方式**：按顺序逐项完成，每完成一项在 `[ ]` 中打勾 `[x]`，并记录验证结果。

## 阶段 0：基础环境准备（必须先做）

| # | 任务 | 源文件（llama.cpp） | 目标文件（NexOS） | 依赖 | 状态 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 0.1 | 确认 GGUF 张量布局对齐 | `llama.cpp`（参考实现） | `gguf_loader.c`（新建） | 无 | [x] |
| 0.2 | 建立内存适配层（malloc→arena） | — | `memory_adapter.c` | 无 | [x] |
| 0.3 | 建立文件读取适配层（mmap→fs_read） | — | `file_adapter.c` | 无 | [x] |

> **阶段 0 验证记录（2026-08-16，QEMU 无头 + serial 抓取）**
> - 构建：`tools/build_win.sh build/os.img`（MSYS2 工具链，kernel64.o 已含 `gguf_adapter_selftest`）。
> - 关键坑修复：① GGUF 头为 24 字节（magic4+version4+tensor_count8+kv_count8），KV 元数据从 **offset 24** 起（原误用 16 导致整段解析错位）；② `build/kernel64.o` 被陈旧产物 + 残留 QEMU 进程锁 `os.img` 双重干扰导致"改了源码 QEMU 仍跑旧逻辑"，已通过杀残留 QEMU + 彻底删 64 位对象后重建解决。
> - serial 实测输出：
>   ```
>   [ADAPT] memory: alloc/free OK
>   [ADAPT] disk: no model blob on image (probe OK; embed one for full disk read)
>   [ADAPT] fixture size=1024 magic=47465546
>   [ADAPT] rc=0 tc=2 kc=7
>   [ADAPT] gguf parse OK: arch=testarch quant=Q4_0 version=3 tensors=2 embed=32 vocab=5 bos=4294967295 type0=Q4_0
>   ```
> - 结论：内存适配层（big_alloc 封装 + >4GB 钩子）、文件读取适配层（mem/disk 双后端 + ATA 回调解耦）、GGUF 解析器（v2/v3，mem 后端）**三项均通过 QEMU 实测**。磁盘后端 probe 正常（镜像未嵌模型故报"no model blob"，符合预期）。
> - 用户可在内核命令行输入 `model adapter` 随时重跑此自测。

## 知识库（用户新增，2026-08-16）

> **动机**：0.5B Q4_0 模型答 "DeepSeek 哪家公司" 时幻觉成「阿里巴巴」。用户要求加知识库，原则：**"真理来源于实践；查不到的问题就用权威机构来源，不要臆测"**。
> **设计**：编译期静态 `g_kb[]` 数组，每条 = 已验证事实 + 权威来源（`source` 字段）。`kb_lookup()` 关键词打分检索；`kb_build_prompt()` 命中→把事实注入 system prompt（RAG 式），未命中→指示「查权威机构/官方来源，勿臆测」。

| # | 任务 | 源文件 | 目标文件（NexOS） | 依赖 | 状态 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| KB.1 | 知识库模块（静态已验证事实 + 权威来源） | — | `knowledge_base.c/.h`（新建） | 无 | [x] |
| KB.2 | 关键词检索 `kb_lookup` + RAG 事实注入 `kb_build_prompt` | — | 同上 | KB.1 | [x] |
| KB.3 | 接入真实 `ask` 命令（`qwen_chat`+KB）与临时自测桩 | — | `.attic64/kernel64.cpp` | KB.2 | [x] |
| KB.4 | QEMU 验证：命中/注入/兜底/模型加载/实时回答 | — | — | KB.3 | [~] |

> **KB 验证记录（2026-08-16，QEMU 无头 + serial 抓取）**
> - 构建：`build/kernel64.bin`=491984 字节，仅 `dd` 到 LBA 2048 补丁进 `os.img`（模型在 LBA 16384 未重嵌）。
> - serial 实测（核心已证）：
>   ```
>   [KB] demo_q=DeepSeek是哪家公司生产的？ hit=1
>   [KB] sys_prompt(demo): ...【已知事实】DeepSeek（深度求索）是由杭州深度求索人工智能基础技术研究有限公司（DeepSeek-AI）开发的，创始人为梁文锋，团队脱胎于量化私募幻方量化（High-Flyer）。...并非阿里巴巴的产品。（来源：DeepSeek 官方/公开工商信息）请严格依据上述事实作答...
>   [KB] fallback_prompt(unmatched): ...若问题超出已验证知识库，请勿臆测或编造；应说明需要查阅权威机构或官方来源（如企业工商信息、政府官网、论文原文）核实后再作答。
>   [GGUF] loaded arch=qwen2 layers=24 embd=896 ... vocab=151936 ... 291 tensors
>   [DEMO] model loaded; answer:   <- 模型加载成功，抵达生成起点
>   ```
> - **未抓到实时 token 流**：本机当前仅 ~0.7GB 空闲物理内存（共 7.8GB），QEMU `-m 4096` 报 "cannot set up guest memory"；`-m 2048` 下 2GB 客户内存靠 swap 抖动，模型加载 7+ 分钟未完、生成首 token 前因宿主 OOM 崩（**非 KB 代码 bug**，早前内存宽裕时 4096 跑过可吐 token）。KB 代码本身已逐项验证；实时回答待宿主内存充裕（真机/≥4GB 空闲）时再抓。用户原话"Windows 不到 1 分钟完成"即指真机资源充足，届时被注入事实锚定的 DeepSeek 回答应为「深度求索/幻方量化」。
> - 待办：① 内存充裕时重跑 `-m 4096` 抓正确回答；② 确认后移除 `[TEMP DEMO]` 自测桩；③ KB 后续可改从 SFS 文件加载（当前为编译期静态数组）。

## 阶段 1：量化核移植（纯算法，无外部依赖）

| # | 任务 | 源文件（llama.cpp） | 目标文件（NexOS） | 依赖 | 状态 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 1.1 | 移植 Q4_0 量化核 | `ggml/src/ggml-cpu/ggml-quants.c` | `ggml_quantize.c` | 0.2 | [ ] |
| 1.2 | 移植 Q4_1 量化核 | 同上 | 同上 | 0.2 | [ ] |
| 1.3 | 移植 Q5_0 量化核 | 同上 | 同上 | 0.2 | [ ] |
| 1.4 | 移植 Q5_1 量化核 | 同上 | 同上 | 0.2 | [ ] |
| 1.5 | 移植 Q8_0 量化核 | 同上 | 同上 | 0.2 | [ ] |
| 1.6 | 移植 Q4_K 量化核（混合量化） | 同上 | 同上 | 0.2 | [ ] |
| 1.7 | 移植 Q5_K 量化核（混合量化） | 同上 | 同上 | 0.2 | [ ] |
| 1.8 | 移植 Q6_K 量化核（混合量化） | 同上 | 同上 | 0.2 | [ ] |
| 1.9 | 验证：在 QEMU 上跑推理，确认结果与移植前一致 | — | — | 1.1-1.8 | [ ] |

## 阶段 2：运行时 CPUID + SIMD 派发层

| # | 任务 | 源文件（llama.cpp） | 目标文件（NexOS） | 依赖 | 状态 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 2.1 | 移植 CPUID 检测函数（x86） | `ggml/src/ggml-cpu/ggml-cpu.c` | `ggml_cpuid.c` | 无 | [ ] |
| 2.2 | 移植 AVX2 特征检测 | 同上 | 同上 | 2.1 | [ ] |
| 2.3 | 移植 FMA 特征检测 | 同上 | 同上 | 2.1 | [ ] |
| 2.4 | 移植 AVX512 特征检测 | 同上 | 同上 | 2.1 | [ ] |
| 2.5 | 建立函数指针表，运行时派发 | 新文件 | `ggml_dispatch.c` | 2.2-2.4 | [ ] |
| 2.6 | 验证：真机打印 CPU 特征（AVX2/FMA 等） | — | — | 2.5 | [ ] |
| 2.7 | 验证：QEMU 上走 baseline 路径，真机走 AVX2 路径 | — | — | 2.6 | [ ] |

## 阶段 3：张量操作核心（矩阵乘法）

| # | 任务 | 源文件（llama.cpp） | 目标文件（NexOS） | 依赖 | 状态 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 3.1 | 移植 `ggml_mul_mat` 矩阵乘法 | `ggml/src/ggml/ggml.c` | `ggml_impl.c` | 1.x | [ ] |
| 3.2 | 接入量化核（调用阶段 1 的函数） | — | `ggml_impl.c` | 1.x, 3.1 | [ ] |
| 3.3 | 移植注意力计算（`ggml_attention` 相关） | `ggml/src/ggml/ggml.c` | `ggml_impl.c` | 3.2 | [ ] |
| 3.4 | 移植 RoPE 旋转位置编码 | `ggml/src/ggml/ggml.c` | `ggml_impl.c` | 3.3 | [ ] |
| 3.5 | 移植 RMSNorm | `ggml/src/ggml/ggml.c` | `ggml_impl.c` | 3.3 | [ ] |
| 3.6 | 验证：在 QEMU 上跑推理，输出与移植前完全一致 | — | — | 3.1-3.5 | [ ] |
| 3.7 | 验证：真机上对比 SIMD 加速前后的推理速度 | — | — | 3.6 | [ ] |

## 阶段 4：Chat Template 引擎

| # | 任务 | 源文件（llama.cpp） | 目标文件（NexOS） | 依赖 | 状态 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 4.1 | 移植 Jinja 模板解析器（简化版） | `common/llama-chat-template.cpp` | `llama_chat_template.c` | 0.2 | [ ] |
| 4.2 | 移植 `llama_chat_template_apply` 函数 | 同上 | 同上 | 4.1 | [ ] |
| 4.3 | 适配 GGUF 内嵌模板读取（`tokenizer.ggml.chat_template`） | `llama.cpp` | `gguf_loader.c`（扩展） | 4.2 | [ ] |
| 4.4 | 实现 system/user/assistant 包装 + `<|im_start|>` 注入 | — | `llama_chat_template.c` | 4.2 | [ ] |
| 4.5 | 验证：QEMU 上多轮对话，确认模板注入正确 | — | — | 4.4 | [ ] |

## 阶段 5：内存与模型管理（突破 4GB 限制）

| # | 任务 | 源文件（llama.cpp） | 目标文件（NexOS） | 依赖 | 状态 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 5.1 | 实现 mmap 式按需加载（模型分片） | `llama.cpp`（参考） | `model_loader.c` | 0.3 | [ ] |
| 5.2 | 扩容 arena 分配器（支持 >4GB） | — | `arena_allocator.c`（修改） | — | [ ] |
| 5.3 | 实现 KV cache 动态管理 | `llama.cpp`（参考） | `kv_cache.c` | 5.1 | [ ] |
| 5.4 | 验证：QEMU 上加载 7B 模型（Q4_K_M） | — | — | 5.1-5.3 | [ ] |

## 阶段 6：Runner 隔离（可选，中期）

| # | 任务 | 源文件（llama.cpp） | 目标文件（NexOS） | 依赖 | 状态 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 6.1 | 将推理引擎移入独立内核任务 | — | — | 3.6 | [ ] |
| 6.2 | 实现任务级崩溃隔离（不拖垮主系统） | — | — | 6.1 | [ ] |
| 6.3 | 验证：人为触发推理崩溃，GUI/Shell 不受影响 | — | — | 6.2 | [ ] |

## 进度总览

| 阶段 | 总任务数 | 已完成 | 进度 |
| :--- | :--- | :--- | :--- |
| 阶段 0（环境准备） | 3 | 0 | 0% |
| 阶段 1（量化核） | 9 | 0 | 0% |
| 阶段 2（SIMD 派发） | 7 | 0 | 0% |
| 阶段 3（张量运算） | 7 | 0 | 0% |
| 阶段 4（Chat Template） | 5 | 0 | 0% |
| 阶段 5（内存/模型） | 4 | 0 | 0% |
| 阶段 6（Runner 隔离） | 3 | 0 | 0% |
| **总计** | **38** | **0** | **0%** |

## 注意事项

1. 每项任务完成后做对应验证；阶段 0/1/4/5 验证在 QEMU 上完成，阶段 2/3 的 SIMD 提速需在真机（Iris Xe）验证。
2. 移植中遇无法绕过的依赖（如 C++ STL 容器）先标记「阻塞」，跳过并记依赖，等适配层完善后回头完成。
3. 不要跳过阶段 0 适配层：`memory_adapter.c` 和 `file_adapter.c` 是后续模块基础。
4. 推进节奏：每阶段做完一次完整验证，避免积压问题。
