# NexOS 插件整合包格式 —— `.nexbundle`

> 版本：1（`schema_version: 1`）
> 状态：正式
> 实现：[`tools/image/nexbundle.py`](../tools/image/nexbundle.py)
> 协议：[`MCP_PLUGIN_PROTOCOL.md`](MCP_PLUGIN_PROTOCOL.md)
> 旧格式：[`NEXPACK.md`](NEXPACK.md)（`.nexpack`，单体应用）

---

## 1. 为什么需要新格式

`.nexpack` 的模型是 **一个包 = 一个应用 = 一个 `.mex`**。它假设“应用”
是内核认识的原子单位，这与 NexOS 的架构铁律冲突：

> **一切皆插件。** 内核不认识“应用”，只认识插件。

`.nexbundle` 因此把分发单位改成**插件集合**：

```
.nexpack                             .nexbundle
────────                             ──────────
manifest.json                        manifest.json
app.mex                              plugins/a/plugin.json
icon.png                             plugins/a/a.mex
                                     plugins/a/ui/layout.json
                                     plugins/b/plugin.json
                                     plugins/b/b.mex
                                     ui/layout.json
```

一个 `.nexbundle` 至少包含一个插件；桌面上的“一个应用图标”只是这个
Bundle 的入口视图，不是内核概念。

### 1.1 与 `.nexpack` 的关系

| 维度 | `.nexpack` | `.nexbundle` |
| --- | --- | --- |
| 分发单位 | 单个应用 | 多插件 Bundle |
| 包内程序集 | 固定 `app.mex` | 每插件 `plugins/<id>/<name>.mex` |
| 清单 | 一个 `manifest.json` | Bundle 清单 + 每插件 `plugin.json` |
| 能力 | `mcp_client_access` 字符串 | 每个插件独立 `capabilities[]` |
| 沙盒 | 无显式模型 | `/sandbox/<plugin_id>` |
| UI | 应用自带绘制代码 | 声明式 `ui_layout` |
| 签名 | 未定义 | 可选 Ed25519 + 确定性树哈希 |
| 状态 | 兼容保留 | 新分发默认格式 |

`.nexpack` 不会被移除：已安装的包继续可读。新工具链（`nexbundle.py`）
不读取 `.nexpack`，两者互不冒充。

---

## 2. 容器格式

`.nexbundle` 是标准 **ZIP**，但所有成员必须 **STORE（method 0，不压缩）**。

原因不是偏好，而是内核现实：

| 约束 | 来源 | 影响 |
| --- | --- | --- |
| 内核没有 zlib/inflate | `plugins/vnc_client.cpp:393` | 成员必须 STORE |
| 内核没有二进制写原语 | 只有 `kern_fs_read` | 安装由宿主完成，内核只读结果 |
| SFS 只读 | `apps.json` 在 SFS | 安装产物落 MKFS，注册表走 overlay |
| MiniCLR 无反射 | `mex_pack.py` 扁平化 | 工具表必须构建期生成 |
| 32 位内核堆 | 内存受限 | 单包建议 ≤ 16 MiB |

### 2.1 确定性打包

同样的输入必须得到同样的字节：

- 成员按名字排序；`manifest.json` 固定放第一个；
- ZIP 时间戳固定为 `1980-01-01 00:00:00`；
- 文件权限固定为 `0644`；
- 不做压缩、不写额外 extra field；
- 包注释固定为 `NexOS plugin bundle v1`。

因此 `sha256` 与 `tree_sha256` 都可以直接用于发布页、CI 比对和签名。

---

## 3. 目录结构

```
<bundle root>/
├── manifest.json                     必需：Bundle 清单
├── icon.png                          可选：≤64 KiB，PNG
├── ui/
│   └── layout.json                   Bundle 级布局（可被 manifest 引用）
├── signatures/
│   └── ed25519.json                  可选：由 pack --sign 生成
└── plugins/
    └── <plugin_id>/
        ├── plugin.json               必需：单插件清单
        ├── <assembly>.mex            managed 插件必需
        ├── ui/
        │   ├── layout.json           插件窗口布局
        │   └── preview.html          可选：设计稿，不参与运行时
        └── assets/                   可选：图标、数据文件
```

规则：

1. **目录名必须等于插件 id**。`plugins/sysinfo/` ↔ `"id": "sysinfo"`。
2. `plugin.json` 的 `id` / `version` 必须与 Bundle 清单的对应项一致。
3. `entry.assembly` 是插件目录下的**单段文件名**，不允许 `../`。
4. 成员名不允许绝对路径、`\`、`..`、空段、驱动器号、控制字符。
5. 成员不重复；`manifest.json` 必须在。
6. 未在清单里引用的资产可以存在（如 `preview.html`），但不会被安装器
   主动读取。

---

## 4. Bundle 清单

完整字段表见 [`MCP_PLUGIN_PROTOCOL.md` §3](MCP_PLUGIN_PROTOCOL.md#3-bundle-清单-manifestjson)
与 [`schemas/manifest.schema.json`](schemas/manifest.schema.json)。最小示例：

```json
{
  "schema_version": 1,
  "id": "sysinfo.suite",
  "name": "系统信息套件",
  "version": "1.0.0",
  "network": { "allow_hosts": [] },
  "capabilities": ["sys.info"],
  "ui_layout": "ui/layout.json",
  "plugins": [
    {
      "id": "sysinfo",
      "name": "系统信息",
      "version": "1.0.0",
      "entry": {
        "kind": "managed",
        "assembly": "sysinfo.mex",
        "init_symbol": "SysInfo.PluginMain::Init",
        "dispatch_symbol": "SysInfo.PluginMain::Dispatch"
      },
      "network": { "allow_hosts": [] },
      "capabilities": ["sys.info"],
      "ui_layout": "ui/layout.json",
      "tools": [
        {
          "name": "sys_info",
          "description": "Read-only snapshot of CPU load, memory and OS identity.",
          "inputSchema": { "type": "object", "properties": {} }
        }
      ]
    }
  ]
}
```

`network`、`capabilities`、`ui_layout` 必须**显式写出**：空值也要写。
这不是冗余，而是让审计者一眼看到“它没有申请网络”，而不是靠默认值猜。

---

## 5. 树哈希

签名与发布校验都基于**确定性树哈希**，它覆盖包内除 `signatures/` 外的
所有成员：

```python
h = sha256()
for name in sorted(members):
    h.update(name.encode("utf-8"))
    h.update(b"\x00")
    h.update(sha256(members[name]).hexdigest().encode("ascii"))
    h.update(b"\n")
tree_sha256 = h.hexdigest()
```

性质：

- 与 ZIP 容器字节无关（只依赖成员名与内容）；
- 成员顺序无关（强制排序）；
- 增删/改任一成员都会改变结果；
- `signatures/` 不计入，避免“签名改变树哈希”的循环。

查看：

```sh
python tools/image/nexbundle.py tree-hash pkg.nexbundle
```

---

## 6. 签名

### 6.1 算法与待签字节

- 算法：**Ed25519**（RFC 8032）；
- 待签字节：

  ```text
  bundle_id + "\n" + version + "\n" + tree_sha256
  ```

- 签名记录放在 `signatures/ed25519.json`：

  ```json
  {
    "alg": "ed25519",
    "key_id": "rfc8032-test",
    "public_key": "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
    "signature": "e556...100b",
    "subject": {
      "bundle_id": "sysinfo.suite",
      "version": "1.0.0",
      "tree_sha256": "12763e..."
    }
  }
  ```

### 6.2 生成

```sh
# 32 字节种子（hex）。生产环境必须离线保存，绝不进仓库。
python tools/image/nexbundle.py pack \
    --src plugins/samples/sysinfo/bundle \
    --out dist/sysinfo.suite-1.0.0.nexbundle \
    --sign "$NEXBUNDLE_SIGN_KEY" \
    --key-id nexos-samples-2026
```

### 6.3 校验

```sh
# 仅验证自洽性
python tools/image/nexbundle.py verify dist/sysinfo.suite-1.0.0.nexbundle

# 钉住公钥（推荐发布流程）
python tools/image/nexbundle.py verify dist/sysinfo.suite-1.0.0.nexbundle \
    --pubkey d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
```

`verify` 会检查：

1. Ed25519 签名有效；
2. `subject.tree_sha256` 与重新计算的值一致；
3. `subject.bundle_id` / `version` 与 `manifest.json` 一致；
4. 若提供 `--pubkey`，公钥必须完全相等。

任何一项失败都返回非 0。

### 6.4 签名状态与安装策略

| 状态 | 含义 | 安装后果 |
| --- | --- | --- |
| `unsigned` | 无 `signatures/ed25519.json` | 允许安装；摘 `CAP_FS_WRITE`；沙盒根强制 `/sandbox/<id>` |
| `valid` | 签名自洽 | 保留 manifest 申请且合法的能力位 |
| `pinned` | 签名有效且公钥命中 `--pubkey` | 同 `valid`，并可用于更高信任等级 |
| `invalid` | 签名/树哈希/主体不符 | **拒绝安装** |

未签名不是错误，而是**降权安装**。这与“默认拒绝写文件系统”一致：
第三方包可以先跑起来，但不能持久化或改系统。

---

## 7. 安装流程

安装发生在**宿主侧**（内核只有 `kern_fs_read`，没有写原语）：

```
1. 读取 .nexbundle（ZIP）
2. 校验成员名 / STORE / 重复 / 体积
3. 解析 manifest.json 与每个 plugins/<id>/plugin.json
4. 校验 id、版本、工具名、能力名、路径引用
5. 校验每个 .mex 的 MEX1 魔数
6. 计算 tree_sha256；若有签名则验证 Ed25519
7. 展开到 MKFS：
       /apps/<bundle_id>/
           manifest.json
           plugins/<id>/...
       /sandbox/<plugin_id>/         （空目录，权限 0700）
8. 逐插件调用 pl_load_plugin(manifest, is_signed, &handle)
       ├─ 未签名：caps &= ~CAP_FS_WRITE
       ├─ 未签名：sandbox_root = /sandbox/<plugin_id>
       └─ 失败：回滚已加载插件，删除临时目录
9. 注册到 MCP 总线（tools/list 从此可见）
10. 写注册表 overlay（卸载时按 bundle_id 反查所有句柄）
```

### 7.1 路径映射

| Bundle 内 | 安装后 | 说明 |
| --- | --- | --- |
| `plugins/<id>/<a>.mex` | `/apps/<bundle>/plugins/<id>/<a>.mex` | 只读 |
| `plugins/<id>/ui/*` | `/apps/<bundle>/plugins/<id>/ui/*` | 只读 |
| — | `/sandbox/<id>/` | 读写根，按插件隔离 |
| — | `/sandbox/<id>/cache/` | 建议的临时目录 |

沙盒根**不在** `/apps` 下：应用目录是只读的，插件写操作必须落到
`/sandbox`。这避免了“插件改自己的程序集”这种自举攻击。

### 7.2 卸载

```
pl_unload(handle) for each plugin
   ├─ busy > 0 -> PL_ERR_STATE（等待在飞调用结束再重试）
   └─ 成功 -> 槽位回收，旧句柄全部 PL_ERR_STALE
删除 /apps/<bundle_id>/
保留 /sandbox/<plugin_id>/（用户数据，需显式 --purge）
删除注册表 overlay
```

“卸载后旧句柄悬空报错”是强制验收项，见
`plugins/loader/plugin_loader.cpp::run_selftest()`。

---

## 8. 体积与资源限制

| 项 | 上限 | 说明 |
| --- | --- | --- |
| 包文件 | 16 MiB | `MAX_BUNDLE_BYTES` |
| 单 `.mex` | 8 MiB | `MAX_MEX_BYTES` |
| 图标 | 64 KiB | PNG |
| 成员数 | 4096 | `MAX_MEMBERS` |
| 并发插件 | 128 | 内核 `PL_MAX_PLUGINS` |
| 单插件 id | 32 字节 | `PL_MAX_ID` |
| 沙盒根 | 96 字节 | `PL_MAX_ROOT` |

超过上限时 `pack` / `verify` 都返回非 0，安装器不会“尽力而为”。

---

## 9. 工具链

```sh
# 打包（--sign 可选）
python tools/image/nexbundle.py pack --src <dir> --out <file> \
    [--sign HEXSEED] [--key-id ID] [--expect-id ID]

# 校验（--pubkey 钉住公钥，--strict 把警告当失败）
python tools/image/nexbundle.py verify <file> [--pubkey HEX] [--strict]

# 检视
python tools/image/nexbundle.py inspect <file>

# 解包（结构有警告时需 --force）
python tools/image/nexbundle.py unpack <file> --out <dir> [--force]

# 树哈希
python tools/image/nexbundle.py tree-hash <file>

# 自带 22 条自测（RFC 8032 向量、路径、能力、清单）
python tools/image/nexbundle.py selftest
```

`pack` 与 `verify` 是同一套校验函数：打包前拦一次，安装前再拦一次。
没有“打包时宽松、安装时严格”的差异。

---

## 10. 版本演进

| 变更 | 处理 |
| --- | --- |
| 新增可选清单字段 | `schema_version` 不变；旧安装器忽略 |
| 新增插件 `kind` | 旧安装器拒绝未知 kind（fail closed） |
| 新增能力位 | 旧内核拒绝未知位（`PL_ERR_CAP`） |
| 签名算法扩展 | `signatures/<alg>.json`；`alg` 不认识则视为 `invalid` |
| 不兼容容器变更 | `schema_version` +1；安装器同时支持 N 与 N-1 |
| 压缩成员 | 永远不会支持（内核无 inflate） |

Bundle 自身的 `version` 与每个插件的 `version` 独立。升级 Bundle 时可以
只替换其中一个插件，只要新的 `plugin.json` 与清单一致。

---

## 11. 与旧 `.nexpack` 的迁移

| 旧 | 新 |
| --- | --- |
| `app_id: weather` | `plugins[0].id: "weather"` |
| `app.mex` | `plugins/weather/weather.mex` |
| `mcp_server[]` | `plugins[0].tools[]` |
| `mcp_client_access` | 每个插件的 `capabilities[]` |
| 应用自绘窗口 | `ui_layout` 声明（可保留自绘作为 `kind: native`） |
| 无沙盒 | `/sandbox/weather` |

迁移步骤：

1. 用 `nexpack.py unpack` 取出旧包；
2. 建 `plugins/<id>/`，放入 `plugin.json` 与 `.mex`；
3. 把 `mcp_server[]` 搬到 `tools[]`（字段名 `tool_name` → `name`，
   `parameters_schema` → `inputSchema`）；
4. 按实际行为填写 `capabilities`；
5. `nexbundle.py pack/verify`。

---

## 12. 安全清单

- [ ] 所有成员 STORE，无压缩；
- [ ] 成员名通过 `safe_relpath()`：无绝对路径、`..`、`\`、控制字符；
- [ ] `manifest.json` 存在且 `schema_version == 1`；
- [ ] Bundle id / 插件 id / 工具名符合正则；
- [ ] 插件目录名 == 插件 id == `plugin.json.id`；
- [ ] 每个 `entry.assembly` 存在且以 `MEX1` 开头；
- [ ] 每个 `ui_layout` 路径在包内；
- [ ] 能力名已知；`ipc` 被拒绝；
- [ ] 签名（若有）覆盖 `tree_sha256` 且 subject 与 manifest 一致；
- [ ] 未签名安装摘 `CAP_FS_WRITE`、强制 `/sandbox/<id>`；
- [ ] 沙盒根不以 `/apps` 开头（防止写自己的程序集）。
