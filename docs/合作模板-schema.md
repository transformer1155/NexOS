# 合作模板（Cooperation Template）· Schema v1

> 状态：schema 定稿，解释器随后实现（阶段一：只读解释器）
> 落点：SFS `ai/cooperation/*.json`，改动即生效，**不重编译内核**
> 64 位优先；32 位不加任何东西

## 1. 设计约束（来自任务要求）

| 要求 | 落在 schema 的哪一部分 |
| --- | --- |
| 热插拔 | 模板是**数据文件**，解释器只读；新增模板 = 新增一个文件 |
| 技能化 | 模板可被 `cooperation.run` 工具调用；`skill` 字段声明可调用性 |
| 角色驱动 | 只写 `roles`，**绝不写队友名字**；名字由运行时匹配 |
| 阶段化 | `phases[]` + 每阶段的 `handoff`（产出物清单） |
| 跨智能体上下文 | 阶段声明 `context_in` / `context_out`，由 ai_context 传递**最小必要**字段 |
| 可嵌套 | 阶段的 `action.kind = template` 可引用另一个模板 |
| 复用既有件 | 任务走 team_task、上下文走 ai_context、技能走 ai_tooluse |

## 2. 顶层结构

    {
      "schema": 1,
      "id": "bugfix-end-to-end",
      "title": "缺陷端到端修复",
      "version": "1.0.0",
      "summary": "从复现到验证的一条龙缺陷修复流程。",
      "tags": ["bugfix", "quality"],
      "skill": { "exposed": true, "tool": "cooperation.run" },
      "inputs": [
        { "name": "symptom", "type": "text", "required": true },
        { "name": "area",    "type": "text", "required": false }
      ],
      "roles": [],
      "phases": [],
      "guards": {}
    }

## 3. roles：角色而非名字

    {
      "role": "reproducer",
      "capability": ["build", "run-qemu", "read-serial"],
      "count": 1,
      "fallback": "blacksmith-liaison"
    }

字段说明：

- `role`：逻辑角色名，模板内部引用它的唯一标识
- `capability`：**匹配依据**。运行时在可用智能体中找具备这些能力的人
- `count`：该角色需要几个实例（默认 1）
- `fallback`：匹配不到时找谁协调。默认不写死；写 `blacksmith-liaison` 表示交给协调者，
  写 null 表示匹配不到就停并报错，绝不静默降级为任意一个智能体

### 为什么禁止写队友名字

写死名字会让模板**绑定到某个时刻的人员配置**：人走了、改名了、同时在忙，模板就失效。
写能力则让同一份模板在任意队伍规模下都能用。这是可热插拔的前提。

## 4. phases：阶段 + 强制交接

    {
      "phase": "reproduce",
      "role": "reproducer",
      "goal": "构造最小复现",
      "action": { "kind": "task", "subject": "复现 {{symptom}}" },
      "context_in": [],
      "context_out": ["repro.steps", "repro.evidence"],
      "handoff": {
        "requires": ["repro.evidence"],
        "to": "fixer",
        "artifact_kind": "evidence"
      }
    }

关键点：

- **`handoff.requires` 是硬门禁**：列出的产出物缺失时，解释器**拒绝推进到下一阶段**并指出缺哪一项。
  这就是产出物不全不往下 的具体实现。
- `context_out` 决定哪些键写进 ai_context 交给下一个角色；其余一律不带
  （最小必要上下文，避免把一个智能体的整段对话塞给另一个）。
- `action.kind` 取值：
  - `task`     —— 在当前任务板上建一条任务（复用 team_task）
  - `report`   —— 只产出结论，不建任务
  - `template` —— **嵌套另一个模板**（可组合）

## 5. 变量替换

`{{name}}` 从 `inputs` 与本阶段可用的 `context_in` 取值。未定义的变量替换为空串并在解释器里记为警告，
不静默保留原始花括号文本（那会被执行者当成字面量）。

## 6. guards：模板级护栏

    "guards": {
      "max_phases": 8,
      "require_handoff_artifacts": true,
      "fail_closed": true
    }

- `require_handoff_artifacts`：是否强制 `handoff.requires`。默认 **true**。
- `fail_closed`：解释器遇到任何不确定（模板缺字段、角色匹配不到且无 fallback）时
  **停止并报错**，而不是猜。默认 **true**。

## 7. 阶段一交付范围（本轮）

**只读解释器**，即：

1. 列出并解析模板（`ai/cooperation/*.json`）
2. 校验 schema（缺字段 / 未知 `action.kind` / role 引用不存在 → 报错不执行）
3. **给出建议分配**：每个角色建议匹配哪个智能体（只建议，不发消息）
4. **给出交接清单**：每阶段需要什么产出物、交给谁
5. 输出一份可读计划，供 Lead 或人确认

**不做**（留待阶段二，跑顺之后）：

- 自动建 team_task
- 自动发消息 / 自动匹配并唤醒队友
- 嵌套模板的实际展开（阶段一只校验引用存在性）

## 8. bugfix-end-to-end 首个模板

    phases:
      1. reproduce  role=reproducer  out: repro.steps, repro.evidence
      2. diagnose   role=diagnoser  out: diag.rootcause
      3. fix        role=fixer      out: fix.diff, fix.files
      4. verify     role=verifier   out: verify.result
      5. report     role=reporter   out: report.summary

交接链：每次 `handoff.requires` 必须满足，否则停在该阶段。

## 9. 与既有组件的接口（不新建平行系统）

| 需要 | 复用 | 不新建 |
| --- | --- | --- |
| 建任务 | team_task（任务板） | 不建第二套任务系统 |
| 传上下文 | ai_context（环形缓冲 + 预算） | 不建第二套记忆 |
| 技能暴露 | ai_tooluse 的 tool 表（cooperation.run） | 不建第二套工具注册 |
| 插件目录 | ai_plugin（nexos.coop.* 条目） | 不建第二套插件体系 |
| 读文件 | vfs / kern_fs_read | 不直接读盘 |
