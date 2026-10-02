# NexOS · 云端 DeepSeek 自主 Agent 演示视频脚本（真实 GUI 定稿版）

> 用途：演示视频分镜 + 解说脚本。**本版以 NexOS 真实图形桌面为绝对主角**——真实 Win11 风格桌面、真实 Terminal 窗口，Agent 输出流式打进真终端，全程无远程控制面板遮罩。
> 风格：科技感、深色主题（Catppuccin 风）、产品发布向。
> 成片：`build/agent_gui_demo.webm`（16s 精剪宣传版，VP9）；长版素材 `build/agent_gui_demo.avi`（136s，mpeg4 1024×768@12fps）。
> BGM：低沉电子 Ambient；关键节点 UI 音效（连接、逐行点亮、完成提示）。

---

## 0. 基本信息

| 项 | 内容 |
|---|---|
| 宣传版时长 | 16s（精剪自 136s 原始录制，剪掉首尾死时间） |
| 主标题 | 一个会自己干活的桌面——NexOS × 云端 DeepSeek |
| 副标题 | 开机即进真实图形桌面，Agent 在真终端窗口里把活干完 |
| 核心卖点 | 真实 GUI（非面板遮罩）+ 云端大模型 + 真实闭环执行 + 流式逐行输出 |
| 技术三要素 | 0x501F 录制模式（自动开终端窗口）/ sink→`g_remote_out` 流式管线 / 密钥打码 `remote_mask_secrets` |

---

## 1. 分镜表（5 镜，对齐 16s 成片时间轴）

| 镜号 | 成片时间 | 画面 | 旁白 / 字幕 |
|---|---|---|---|
| S1 | 0:00–0:02 | 深色桌面就绪：Win11 风格任务栏 + 深色壁纸 + 真实 Terminal 窗口（标题栏 / 最小化·关闭按钮齐全） | （字幕）开机即桌面。这不是终端模拟器，是操作系统自己的 GUI。 |
| S2 | 0:02–0:06 | 终端窗口打出第一行：`> agent config url http://…/v1/chat/completions` → `Agent config updated.`（URL 属宿主代理，密钥不入屏） | （旁白）两行命令，接通云端 DeepSeek。密钥全程不落屏。 |
| S3 | 0:06–0:08 | 窗口短暂等待——Agent 经宿主 TLS 代理连上 `api.deepseek.com`，ReAct 规划中 | （字幕）模型在云端，规划在此刻。 |
| S4 | 0:08–0:11 | **高潮：输出逐行流入真终端**——`nexos$ mkfs`（MKFS formatted, 135 KB）→ `nexos$ fwrite demo.txt hello nexos`（Wrote 11 bytes）→ `nexos$ cat demo.txt`（`hello nexos` 原样回来） | （旁白）规划、执行、验证——mkfs、写文件、读回来，三步闭环，全程在真实系统里。 |
| S5 | 0:11–0:16 | 定格完整工作日志：两条 `SUMMARY`（"Formatted the data disk… verified it with cat" / "All requested steps already executed"），画面停留 | （旁白）一句话目标进去，验收报告出来。NexOS：让大模型在真实系统里，动手。 |

---

## 2. 完整解说词（可配音 · 约 130 字）

> 开机即桌面。这不是终端模拟器，是操作系统自己的图形界面。
>
> 两行命令，接通云端 DeepSeek——密钥全程不落屏。
>
> 模型在云端，规划在此刻。
>
> 看，输出正一行一行，流进这个真实的终端窗口：格式化磁盘、写入 demo.txt、再原样读回来——hello nexos。
>
> 三步闭环，一条验收报告。
>
> NexOS：让大模型在真实系统里，动手。
> 开源：gitee.com/transformer1155/NexOS

---

## 3. 屏幕内嵌字幕（关键帧）

- S1：`真实桌面 · 真实终端窗口`
- S2：`接通云端 DeepSeek`
- S3：`ReAct：规划 → 执行 → 回看`
- S4：`mkfs ✓  fwrite ✓  cat ✓`
- S5：`SUMMARY：闭环完成`

---

## 4. 录制与复现要点

1. **构建**：`make` → `build/os_v2.img`（含 0x501F 录制模式支持）。
2. **启动**：QEMU `-device loader,addr=0x501F,data=1,data-len=1` —— 内核读到该 boot flag 后自动打开 Terminal 窗口并启用"远程输出→真终端"模式（`gui_set_remote_to_terminal(1)`），同时抑制全屏远程遮罩。
3. **驱动**：串口（COM1）向 guest 发送 `agent config url/key/model` → `agent run <GOAL>`；agent 经宿主 `10.0.2.2:18999` TLS 代理访问 DeepSeek。
4. **录制**：Xvfb 虚拟屏 + QEMU `-display sdl` + `ffmpeg -f x11grab` 抓桌面。
5. **流式原理**：内核 sink（`Terminal::put_char → g_ssh_out_fn`）逐字符进入 `gui_remote_output`，跨调用累加成行后压入 14 行环形缓冲 `g_remote_out`，终端窗口 demo 分支逐行渲染——成片中可见输出**逐行点亮**而非一次性蹦出。
6. **剪辑**：原始录制 136s 中 agent 约 13s 内完成全部工作，13s 后为静止尾帧；精剪取 2s–18s（桌面就绪 → 配置 → 流式执行 → 定格），保留 ~5s 收尾停留。

---

## 5. 长版素材备注（136s AVI）

| 时间段 | 内容 | 用途 |
|---|---|---|
| 0–2s | 引导画面 → 桌面就绪 | 可做片头 |
| 2–5s | Terminal 窗口 + `agent config` 配置上屏 | S2 素材 |
| 5–10s | 云端连接 / ReAct 规划（画面静止属正常） | S3 素材 |
| 10–13s | 输出逐行流入（mkfs → fwrite → cat → SUMMARY） | S4/S5 核心素材 |
| 13s–末尾 | 静止定格（agent 已收工） | 可截定格帧做封面 |
