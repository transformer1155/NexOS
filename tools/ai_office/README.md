# ai_office — AI 办公任务并发执行 + 行号寻址协议

针对"处理 100 个 Excel"这类 AI 办公任务, 本工具箱实现两件事:

1. **多进程并发执行** —— 通过 MCP 让多个模型 (DeepSeek V4.1 Flash 等) 并行生成
   多份脚本, 丢入各自子进程 (Terminal) 并发测试; 失败的只走"行号寻址协议"做最小
   化重写, 而不是整段重来。
2. **行号寻址协议** —— AI 生成代码时, 在每一行前加 `[001]` 编号。Debug 时系统精准
   定位报错行号, 只把那一行 (±2 行上下文) 回传给模型重写, 极大节省 Token。

> 纯 Python 标准库, 无第三方依赖; Windows / Linux 直接运行。

---

## 1. 行号寻址协议 (Line-Number Addressing Protocol)

AI 生成的代码每一行都带前缀:

```
[001] import sys
[002] def add(a, b):
[003]     return a + b
[004] if __name__ == '__main__':
[005]     print('result =', add(2, 3))
```

- 编号 3 位零填充, 从 1 连续递增; 空行/注释行同样编号。
- `lnum.encode(src)` 把纯源码变成协议文本; `lnum.decode(proto)` 还原纯源码,
  并记录 **逻辑行号 → 物理行号** 映射 (供 traceback 定位)。
- `lnum.patch(proto, num, new_body)` 只替换第 `num` 行, 其余不动。
- `lnum.parse_traceback_error_line(tb)` 从报错里抽物理行号;
  `map_physical_to_logical` 把它映射回逻辑行号, 只把该行的 ±2 行上下文发给模型。

**为什么省 Token**: 一个 50 行的脚本整段重发约 2000 字符; 行号寻址下, 报错修复
通常只回传 ~5 行 (~150 字符) + 报错信息, 节省 90%+。

---

## 2. 多进程并发执行

- **生成并发**: `Orchestrator` 用 `multiprocessing.Pool` 让多个模型后端并行产出脚本
  (对应"通过 MCP 让模型并行生成多份脚本")。
- **执行并发**: 生成后的脚本用独立子进程 (Terminal) 并发执行 —— `runner.run_one`
  每个脚本一个进程, 编排层不串行, 自然并发。
- **调试闭环**: 脚本报错 → 行号寻址定位 → 只重写那一行 → 重跑, 每脚本最多
  `max_fix_iters` 次。

---

## 3. 模型后端 (可插拔)

由环境变量 `AIO_BACKEND` 选择 (`models.get_backend`):

| 后端 | 说明 | 配置 |
|------|------|------|
| `mock` (默认) | 离线确定性, 供自测/演示 | 无需网络 |
| `openai` | OpenAI 兼容端点 (DeepSeek 等) | `AIO_BASE_URL` / `AIO_MODEL` / `AIO_API_KEY` |
| `mcp` | 通过 MCP (stdio JSON-RPC) 把生成/修复委托给模型类 MCP 服务 | `AIO_MCP_CMD` |

示例 (指向 DeepSeek V4.1 Flash, 已实测可用):

```bat
set AIO_BACKEND=openai
set AIO_BASE_URL=https://api.deepseek.com/v1
set AIO_MODEL=deepseek-flash        :: 即 DeepSeek-V4.1-Flash (1M 上下文)
set AIO_API_KEY=sk-xxxx
```

可用模型 (实测 `/v1/models`): `deepseek-flash` (V4.1-Flash)、`deepseek-v4-pro` (V4-Pro)。

---

## 4. MCP Server (题目要求的"通过 MCP 并行生成")

`python -m ai_office.mcp_server` 启动一个纯标准库 MCP server (stdio JSON-RPC),
暴露三个工具 (见 `mcp.config.json`):

- `ai_generate(user, system?, backend?)` → 带行号前缀的脚本文本。
- `ai_fix_line(protocol, num, error_context, backend?)` → 只重写出错的那一行。
- `ai_run(tasks, backend?, workers?)` → 多任务**并发**生成+执行+单行修复闭环。

编排器 (`MCPBackend`) 在每个 worker 进程里拉起一个 MCP 子进程、发 `tools/call`,
从而形成"通过 MCP 让模型并行生成 → Terminal 并发测试"的闭环。

把 `mcp.config.json` 交给支持 `command/args` 式 stdio MCP 的客户端即可注册本服务。

---

## 5. 快速开始

```bat
cd E:\NexOS\NexOS\tools

:: 离线自测 (并发 + 行号寻址单行修复 + Token 节省, 无需网络)
python -m ai_office.self_test

:: 真实 DeepSeek 测试 (需 AIO_API_KEY): 并发生成 4 个脚本 + 真实单行修复
python -m ai_office.real_test

:: 以 MCP 客户端身份验证整条 MCP 链路 (生成/并发执行/行号修复)
python -m ai_office.mcp_client_test

:: 跑一份任务清单 (JSON 数组)
python -m ai_office.cli run tasks.json --workers 8 --workdir ./data

:: 启动 MCP server
python -m ai_office.cli mcp
```

任务清单 `tasks.json` 格式 (支持每任务独立 `workdir`):

```json
[
  {"id": "excel-01", "prompt": "读取 ./01.xlsx 求每列均值", "timeout": 60, "max_fix_iters": 3, "workdir": "./data"},
  {"id": "excel-02", "prompt": "读取 ./02.xlsx 导出 CSV", "timeout": 60, "max_fix_iters": 3, "workdir": "./data"}
]
```

### 实测结果 (真实 DeepSeek V4.1 Flash)

| 测试 | 结果 |
|------|------|
| `self_test` (离线) | 10 个任务并发 0.4s, 10/10 通过, Token 节省 **71%** |
| `real_test` | 4 个任务并发 4.7s, **4/4 通过**, 1 次真实单行自动修复 |
| `mcp_client_test` | MCP initialize / tools/list / ai_generate / ai_run 全通, 并发 2/2 通过 |

---

## 6. 文件结构

```
tools/ai_office/
├── __init__.py        包入口
├── lnum.py            行号寻址协议 (encode/decode/patch/map)
├── models.py          可插拔后端 (Mock/OpenAI/MCP) + 协议提示词
├── runner.py          执行协议脚本、解析报错行 (含 SyntaxError)、单行修补
├── orchestrator.py    多进程并发生成 + 并发执行 + 调试闭环
├── mcp_server.py      stdlib MCP server (ai_generate/ai_fix_line/ai_run)
├── mcp_client_test.py MCP 客户端链路测试
├── cli.py             CLI 入口 (self-test / run / mcp)
├── self_test.py       离线自测 (Mock)
├── real_test.py       真实 DeepSeek 测试
├── mcp.config.json    MCP 服务注册配置
└── README.md          本文
```

---

## 7. 与 100 个 Excel 的结合

把 100 个 Excel 拆成 100 个 `JobSpec` (prompt 里写明文件路径与处理目标), 用
`ai_run` 或 `Orchestrator.run` 以 `workers=N` 并发: 每个文件一个进程 → 生成脚本 →
执行 → 报错则行号寻址单行修复。单文件失败不影响其余 99 个, 且修复代价被压到最低。
