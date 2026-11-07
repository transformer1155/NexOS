# legacy_scripts — 历史调试/测试脚本归档

## 这是什么

这里存放项目开发过程中产生的**一次性调试、探针、验证脚本**。它们原名以 `_` 前缀开头
（`_mk.sh`、`_diag_xxx.sh`、`_probe.py` …），此前散落在仓库的多个目录顶层：

| 原位置 | 数量 | 现位置 |
|---|---|---|
| 仓库根目录 | 7 | `root/` |
| `tools/` 顶层 | 43 | 本目录顶层 |
| `win11-ui/` 顶层 | 17 | `win11-ui/` |
| `bootloader/tools/` | 9 | `bootloader/` |

合计 **76 个文件**，2026-11-07 统一归置，`git mv` 保留了历史。

## 重要：这些脚本不参与构建

**Makefile 不引用本目录任何文件。** 移动它们之前已做过全仓库引用扫描：

- `Makefile`、`test.sh`、`test_uefi.sh`、`.github/` CI —— **零引用**
- 构建后 `build/os_v2.img` 产物字节数完全不变

所以把它们归档**不会**影响 `make`。

## 使用注意

这些脚本大多带有**硬编码的绝对路径**，例如：

```bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
```

这是当年在 WSL 下调试时写死的。搬到本目录后路径已失效，**直接运行会失败**。
若要复用某个脚本，需要先改掉脚本里的 `cd` 目标，或改到脚本原先所在的目录执行。

## 未被搬运的目录

以下三处**有意保留原状**，不属于本次整理范围：

- `drivers/_*.sh`（29 个）—— 该目录全部是下划线脚本，但它们是驱动层仅有的
  手工构建/测试入口（Makefile 直接用 `drivers/*.c` 编译，不走这些脚本）。
- `archive/legacy_scripts/`（44 个）—— 本身已是历史归档区，且被
  `archive/_MANIFEST_files.json` 按文件名索引，再搬会打乱清单。
- `tools/_archive/` —— 内含第三方 `.venv`（pydantic/pip 等），不可改动。

## 子目录内的工具类脚本

`tools/webtest/`、`tools/verify/`、`tools/boot/`、`tools/capture/`、`tools/misc/`
下的下划线脚本属于**该工具目录的组成部分**（例如 `tools/verify/` 里既有正名的
`assert_artifact_changed.sh`，也有配套的 `_selftest_artifact_changed.sh`），
保留在原位以免破坏其内部调用关系。
