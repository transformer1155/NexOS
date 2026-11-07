# AI 线工作计划（基于当前线上状态）

> 作者：ai-engineer · 依据：本次会话的实际构建与测试结果
> 原则：已验证的必须附可复现命令；未验证的明确标注

## 一、现状

### 已完成且已验证

| 交付物 | 验证方式 | 状态 |
| --- | --- | --- |
| SHA-256 / HMAC / HKDF | RFC 向量 12/12 | 通过 |
| ChaCha20-Poly1305 AEAD | RFC 8439 + OpenSSL 差分 28 组 | 通过 |
| X25519 | RFC 7748 全部向量 | 通过 |
| TLS 1.3 客户端 | openssl s_server，含证书链+PSS 验签+SAN | 通过 |
| X.509 链验证 | 正反向 4 用例 | 通过 |
| TLS 进 64 位内核 | make RC=0 / k64 undefined 0 / 符号存在 | 通过 |
| ai_config 配置模块 | 25 项单测 | 通过 |
| 隔离框架（4 层） | 仅代码，未编译未接入 | 框架 |

### 已写但未接入构建

- ai_config.h / ai_config.cpp（未进 Makefile）
- proc_isolation.h / proc_isolation.cpp（刻意不链，避免无窗口时破坏构建）
- net_tls.* 已链入 64 位；32 位不含

## 二、P0 补完已铺好的路（需要 1 次构建窗口）

| # | 任务 | 产物 | 依赖 |
| --- | --- | --- | --- |
| P0-1 | ai_config 进 Makefile（64 位） | Makefile 3 行 | 无 |
| P0-2 | 启动读 MKFS ai.cfg 并解析 | 约 20 行 | P0-1 |
| P0-3 | ai config [show / set k v] 命令 | 约 30 行 | P0-1 |
| P0-4 | nexos_ai.cpp 的 64 位分支调 net_https_post | 约 40 行 | 无 |
| P0-5 | 真实 key 跑通 ai ask 端到端 | 无代码 | P0-2/3/4 + QEMU 窗口 |

**验收**：QEMU 内 ai config set key sk-... 后，ai ask 你好 返回真实模型回答。

## 三、P1 新功能（按价值排序）

### P1-1 AI tool-use（最高价值）

让内核 AI 能自主调用 MCP 工具，而不是只会问答。

- 复用 mcp_dispatch_request 的身份模型（resident-set），不另造身份
- 模型输出结构化 tool_call，内核校验后派发，结果回灌，继续生成
- 风险控制：工具白名单 + 每次调用记入 trace（GUI 可看）
- 工作量：内核侧约 300 行，含停止条件与循环上限

### P1-2 多轮对话上下文

- 环形缓冲保留最近 N 轮；超预算时调用模型做摘要压缩
- 与 P1-1 共用上下文结构（tool 调用也是对话的一部分）
- 约 150 行

### P1-3 用量计量账本

- 记录每次请求的 token 估算、耗时、provider、成功/失败
- 为将来计费打底，现在就能在设置里显示
- 约 120 行

## 四、P2 隔离框架落地（需要更大的改动窗口）

| # | 任务 | 阻塞点 |
| --- | --- | --- |
| P2-1 | 补 PML4 遍历释放叶子页 | 需要 VMM 导出遍历器 |
| P2-2 | CR0.WP 置位与恢复 | 需确认内核不依赖 WP=0 |
| P2-3 | proc.cpp 接创建/销毁钩子 | 需要构建窗口验证 |
| P2-4 | syscall.cpp 白名单门禁 | 同上 |
| P2-5 | vfs.cpp 沙盒路径解析 | 需处理 rename 绕过风险 |

不建议在没有干净窗口时动 P2：它改的是进程与文件系统核心路径，
构建失败会挡住全队。

## 五、资源需求

| 需求 | 时长 | 用途 | 并行约束 |
| --- | --- | --- | --- |
| 构建窗口 1 次 | 15-25 分钟 | P0-1..P0-4 编译验证 | 不能与任何 make 并行 |
| QEMU 窗口 1 次 | 3-5 分钟 | P0-5 端到端（不测性能） | 不能与其他 QEMU 并行 |
| 单元测试 | 30 秒 | ai_config / TLS 回归 | 在 /tmp 编译，不占 build/ |

申请排在 svm-porter 之后；若能搭 distnet 的 2 分钟窗口，P0-5 可提前。

## 六、风险与边界

1. 容量：32 位镜像余量约 350 B。任何进 32 位的代码会立刻撞墙，
   因此 AI 新功能一律只进 64 位。
2. 证书链长度：TLS13_MAX_HANDSHAKE=8192 约容纳 4-5 张证书，更长的链会干净失败。
   DeepSeek 实测 3 张、约 3.5 KB。
3. ECDSA 未实现：遇到 ECDSA 证书链会 fail-closed（明确报错，不静默放行）。
   当前目标站点是 RSA，不影响。
4. 密钥安全：key 存 MKFS 明文。若将来要审计，需要加密存储 + 口令派生。
5. 我不动：sfs_files/、build/、裸 make clean；任何 rm 构建产物先报备。

## 七、外部依赖（会阻塞我）

- NIC 状态：若 NIC DOWN 是显示 bug（PCI 判据 vs ISA 网卡），需一次构建验证；
  若是真实网络不通，则 P0-5 无法进行。
- Guru Meditation 1155：属显示/MMIO 路径，不直接影响 AI 功能，但拖慢验证。
- 构建工具：vm-run make 需保持失败能报失败。

## 八、64 位收敛（对齐最新指示）

所有 AI 新能力**只存在于 64 位内核**，32 位镜像保持不动。

### 8.1 已有的 64 位专属

- TLS：tls_crypto64.o / tls1364.o / tls_x50964.o / net_tls64.o（已在 KERNEL64_OBJS）
- 32 位那 4 条 TLS 编译规则是惰性的（无链接引用），可在方便时删除

### 8.2 待加入 64 位的两个新模块

Makefile 追加（仅 64 位，等构建空闲时应用）：

    $(BUILD)/ai_config64.o: ai_config.cpp ai_config.h | $(BUILD)
        $(CC64) $(CXX64FLAGS) -c ai_config.cpp -o $@
    $(BUILD)/proc_isolation64.o: proc_isolation.cpp proc_isolation.h | $(BUILD)
        $(CC64) $(CXX64FLAGS) -c proc_isolation.cpp -o $@

并把这两个对象加进 KERNEL64_OBJS（紧跟 tls_x50964.o 之后）。

### 8.3 32 位一律不碰

- 不给 32 位加 ai_config / proc_isolation 规则
- 32 位 ai 通路维持现状（宿主 gateway），不改一行
- 理由：32 位镜像余量约 350 B，任何新增都会立刻失败

### 8.4 代码层注意事项（64 位专属）

- proc_isolation.cpp 用 uint64_t 地址与 CR3，天然 64 位语义；32 位编译无意义
- ai_config.cpp 无指针宽度依赖，但按约定同样只给 64 位
- 引用方用 #if defined(__x86_64__) 包住，避免 32 位误调
