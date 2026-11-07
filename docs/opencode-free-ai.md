# OpenCode 免费额度（无需账号）接入说明

> 状态：已接入 · 2026-10-31
> 结论：OpenCode Zen 的免费模型可以用匿名哨兵 key `public` 直接调用，**不需要注册、不需要 OAuth、不需要 API Key**。NexOS 的宿主侧 AI 桥已默认切换到这条通路。

---

## 1. 上游协议（实测）

| 项 | 值 |
|---|---|
| Base URL | `https://opencode.ai/zen/v1` |
| 对话端点 | `POST /chat/completions`（OpenAI 兼容） |
| 模型列表 | `GET /models` |
| 鉴权头 | `Authorization: Bearer public` |
| 请求体 | `{"model":"<free-model>","messages":[{"role":"user","content":"..."}],"max_tokens":512}` |
| 响应 | OpenAI 格式：`choices[0].message.content` |

`public` 是**共享匿名 key**，服务端按 IP 限流；免费模型集合会轮换。

### 当前免费模型（以 `GET /models` 中 cost 为 0 为准）

- `deepseek-v4.1-flash-free`（默认，中文友好）
- `glm-5.3-flash-free`
- `big-pickle`
- `mimo-v2.6-flash-free`
- `flint-40b-free`
- `nemotron-3.5-lightning-free`
- `beam-free` / `nemotron-3.5-super-vl-free`

模型列表由 `opencode_free.list_free_models()` 动态拉取并缓存 1 小时；拉取失败时回退到内置列表。

### 限流形态（重要）

额度耗尽时上游返回：

```json
HTTP 429
{"type":"error","error":{"type":"FreeUsageLimitError",
 "message":"Free usage limit reached. Free models work in OpenCode ..."}}
```

并带 `Retry-After` 响应头（本机实测约 19 小时）。**这不是配置错误，是匿名配额用完了**：换个出口 IP 或等窗口重置，或者设置真实 key。

---

## 2. NexOS 里的四条接入通路

### 2.1 宿主 gateway `POST /ai`（AI 面板 / NexOS.AI）

```bash
python tools/net/gateway_server.py            # 默认 --ai-backend opencode
```

- `NexOS.Forms.Backend.AIRequestAsync`（WinHost）→ `POST http://127.0.0.1:19387/ai`
- 请求：`{"prompt":"...","mode":0|1|2,"model":"可选"}`
- 成功：`{"ok":true,"reply":"...","model":"...","mode":n,"quotaLeft":n,"backend":"opencode"}`
- 免费额度耗尽：`429 {"ok":false,"error":"OpenCode free quota exhausted (retry in ~19 min)","retry_after":68471}`
- 其他失败：`502 {"ok":false,"error":"...","attempts":[...]}`
- 失败**不扣**本地 quota；只有成功调用才 `quota_left -= 1`。
- 离线回退：`--ai-backend echo`（旧行为：`(host ai echo) ...`）

### 2.2 宿主 LLM bridge `GET /ask`（`agent run` 的 `net_ask_host`）

```bash
python tools/net/llm_bridge.py                # 默认 --backend opencode
```

- guest 的 `net_ask_host()` → `GET http://10.0.2.2:18080/ask?q=...`
- 返回 `text/plain` 答案正文；429 时返回 500 + 可读原因。
- 仍可用 `--backend lmstudio|ollama --lm http://127.0.0.1:1234` 接本地模型。

### 2.3 宿主 TLS proxy `--preset opencode-free`（agent 的 POST 通路）

```bash
python tools/net/deepseek_proxy.py --preset opencode-free
# 等价于 --host opencode.ai --path /zen/v1/chat/completions \
#        --key public --force-model deepseek-v4.1-flash-free
```

guest 侧：

```
agent config url   http://10.0.2.2:18999/v1/chat/completions
agent config model deepseek-v4.1-flash-free
agent config key   public
agent remote "用一句话介绍 NexOS"
```

这条通路支撑 `net_agent_plan_exec()` 的 ReAct 循环（模型输出 `shell:` 行 → 在 OS 内执行）。

### 2.4 内核 AI：`ai ask` / `NexOS.AI.Request`（guest 自己的网卡）

内核只有明文 HTTP，但 QEMU 用户态网络（SLIRP）把宿主映射为 `10.0.2.2`，
所以 guest 自己的 TCP/IP 栈可以直接 POST 到宿主 gateway，**guest 内不需要跑任何代理**：

```
nexos> ai gateway                 # 显示当前网关主机（默认 10.0.2.2）
nexos> ai gateway 10.0.2.2        # 运行时覆盖
nexos> ai ask 用一句话介绍 NexOS   # POST http://10.0.2.2:19387/ai
```

- 实现：`ai_ask_text()` → `ai_gateway_ask()`（`nexos_ai.cpp`）经 `net_http_post()`
  走 guest 网卡发 `{"prompt":...,"mode":1}`，解析 `reply` 字段（支持 `\uXXXX` 与 UTF-8）。
- `NexOS.AI.Request(...)`（MiniCLR ICall `ic_ai_request`）走同一个 `ai_request()`：
  `Fast(0)` 仍本地（RAG + Markov，符合 AIMode 契约）；`Balanced(1)` / `Heavy(2)` 走网卡 POST 宿主 gateway。
- gateway 不可达时回退本地引擎并置 `code` 表示降级；gateway 明确拒绝（如免费额度 429）时
  把错误原样返回，不伪造答案。
- 前提：QEMU 启动带网卡（默认 `-net nic,model=ne2k_isa -net user`，guest 静态 IP 10.0.2.15），
  且宿主已运行 `python tools/net/gateway_server.py`。

---

## 3. 环境变量

| 变量 | 作用 |
|---|---|
| `OPENCODE_ZEN_MODEL` | 强制模型 id（默认自动挑免费模型） |
| `OPENCODE_ZEN_KEY` | 覆盖匿名 `public`（填真实 key 可提高额度） |
| `OPENCODE_ZEN_BASE` | 覆盖 base URL（指向自建代理） |
| `OPENCODE_ZEN_TIMEOUT` | 单次请求超时秒数（默认 60） |
| `NEXOS_AI_BACKEND` | gateway `/ai` 后端：`opencode` / `echo` |

## 4. 注意

- 匿名配额是**按 IP 共享**的，压测/多人共用会更快耗尽；生产演示建议常备一个真实 key 作为 `OPENCODE_ZEN_KEY`。
- 免费模型可能保留/使用 prompt 做改进（不同促销策略不同）；**不要发送敏感数据**。
- 模型集合轮换后，旧 id 会返回 401/400；客户端会自动尝试下一个候选，但仍建议用 `OPENCODE_ZEN_MODEL` 固定一个当前可用的。
- 相关代码：[`tools/net/opencode_free.py`](../tools/net/opencode_free.py)、[`gateway_server.py`](../tools/net/gateway_server.py)、[`llm_bridge.py`](../tools/net/llm_bridge.py)、[`deepseek_proxy.py`](../tools/net/deepseek_proxy.py)。
