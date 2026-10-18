#!/usr/bin/env python3
# =====================================================================
#  verify_ai_overlay.py  -  验证内核 apps.json overlay 合并逻辑
# ---------------------------------------------------------------------
#  这台机器没有 WSL Debian / g++，无法编译内核。所以这里做两件事：
#
#    A. 源码结构断言（静态）：确认 nexos_ai.cpp 里
#         * 存在 MERG sentinel 的 merge 分支
#         * merge 模式下 upsert 命中时重置 g_new_tool_count
#         * ai_gateway_init 读 apps.overlay.json 并加 MERG 前缀
#         * 读取用的是扁平文件名（不是 "mkfs/..."，因为 kern_fs_read
#           是扁平查找且 MKFS 优先）
#
#    B. 行为模拟（动态）：用 Python 按同一算法重放
#         system apps.json -> parse -> overlay apps.json -> merge
#       并断言：内置 app 保留、第三方 app 新增、同 app_id 升级不重复。
#
#  用法: python tools/verify_ai_overlay.py
# =====================================================================
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AI_CPP = os.path.join(ROOT, "nexos_ai.cpp")

pass_n = 0
fail_n = 0


def check(what, ok, detail=""):
    global pass_n, fail_n
    if ok:
        pass_n += 1
        print("PASS  " + what)
    else:
        fail_n += 1
        print("FAIL  " + what + ("  -> " + str(detail) if detail else ""))


def main():
    if not os.path.isfile(AI_CPP):
        print("error: nexos_ai.cpp not found at " + AI_CPP, file=sys.stderr)
        return 2

    src = open(AI_CPP, encoding="utf-8", errors="replace").read()

    print("=== A. 源码结构断言 ===")

    # A1: merge 分支存在
    check("parse_apps_json has MERG sentinel branch",
          "json[0] == 'M' && json[1] == 'E'" in src,
          "sentinel test missing")

    # A2: 非 merge 才重置 registry
    check("registry reset is guarded by !merge",
          re.search(r"if\s*\(!merge\)\s*\{", src) is not None,
          "no 'if (!merge) {' guard")

    # A3: merge 模式 upsert 按 app_id 查找
    check("merge mode upserts via app_registry_find",
          re.search(r"if\(merge\)\s*\{\s*\n\s*struct AppRegistry\* ex = app_registry_find",
                    src) is not None,
          "no app_registry_find upsert in merge path")

    # A4: upsert 命中时重置工具计数（升级不重复）
    check("upsert resets per-app tool count",
          re.search(r"g_new_tool_count\[cur\] = 0;\s*\n\s*last_tool = 0;\s*\n\s*continue;",
                    src) is not None,
          "tool count not reset on upsert")

    # A5: ai_gateway_init 读 overlay
    check("ai_gateway_init reads apps.overlay.json",
          'kern_fs_read("apps.overlay.json"' in src,
          "overlay read missing")

    # A6: overlay 被加上 MERG 前缀
    check("overlay gets MERG prefix",
          "ovl[0] = 'M'; ovl[1] = 'E'; ovl[2] = 'R'; ovl[3] = 'G';" in src,
          "sentinel not written")

    # A7: 用扁平文件名（不能带 mkfs/ 前缀）
    check("overlay read uses flat filename (no dir prefix)",
          "kern_fs_read(\"mkfs/" not in src,
          "must not use a directory prefix: kern_fs_read is flat and MKFS-first")

    # A8: overlay 缓冲区留出 4 字节给 sentinel
    check("overlay buffer reserves 4 bytes for sentinel",
          "sizeof(ovl) - 5" in src,
          "buffer size must leave room for 4-byte prefix + NUL")

    print()
    print("=== B. 行为模拟（按 C++ 算法重放）===")

    system_json = {
        "schema_version": 1,
        "gateway": {"host": "127.0.0.1", "port": 19387, "quota_pool": 1000},
        "apps": [
            {
                "app_id": "weather", "name": "Weather App", "version": "1.0.0",
                "author": "nexos", "ai_mode_required": "Fast",
                "mcp_server": [{"tool_name": "get_weather",
                                "description": "Return weather.",
                                "parameters_schema": {"type": "object"}}],
                "mcp_client_access": []
            },
            {
                "app_id": "assistant", "name": "Personal Assistant",
                "version": "1.0.0", "author": "nexos",
                "ai_mode_required": "Balanced",
                "mcp_server": [{"tool_name": "summarize",
                                "description": "Summarize.",
                                "parameters_schema": {"type": "object"}}],
                "mcp_client_access": ["weather"]
            }
        ]
    }

    # 安装器产出的 overlay：第三方应用 notes + 升级 weather（新工具集）
    overlay_json = {
        "schema_version": 1,
        "apps": [
            {
                "app_id": "notes", "name": "Notes", "version": "2.1.0",
                "author": "third-party", "ai_mode_required": "Balanced",
                "mcp_server": [
                    {"tool_name": "add_note", "description": "Add a note.",
                     "parameters_schema": {"type": "object"}},
                    {"tool_name": "list_notes", "description": "List notes.",
                     "parameters_schema": {"type": "object"}}
                ],
                "mcp_client_access": ["weather"]
            }
        ]
    }

    def parse_registry(manifest, registry=None, merge=False):
        """按 parse_apps_json 的语义重放。"""
        if not merge:
            registry = []
        registry = list(registry)
        by_id = {a["app_id"]: a for a in registry}

        for app in manifest.get("apps", []):
            aid = app.get("app_id")
            if not aid:
                continue
            if merge and aid in by_id:
                # upsert：替换工具集（对应 C++ 里 g_new_tool_count[cur]=0）
                idx = next(i for i, a in enumerate(registry) if a["app_id"] == aid)
                registry[idx] = dict(app)
                by_id[aid] = registry[idx]
            else:
                registry.append(dict(app))
                by_id[aid] = registry[-1]
        return registry

    reg = parse_registry(system_json, None, merge=False)
    check("system manifest registers weather+assistant",
          [a["app_id"] for a in reg] == ["weather", "assistant"],
          [a["app_id"] for a in reg])

    reg = parse_registry(overlay_json, reg, merge=True)
    ids = [a["app_id"] for a in reg]
    check("overlay appended third-party app",
          "notes" in ids, ids)
    check("overlay preserved built-in weather",
          "weather" in ids, ids)
    check("overlay preserved built-in assistant",
          "assistant" in ids, ids)
    check("no duplicate app ids after merge",
          len(ids) == len(set(ids)), ids)

    # 升级：同 app_id 的新版本覆盖，不新增条目
    upgrade = {
        "apps": [{
            "app_id": "notes", "name": "Notes", "version": "3.0.0",
            "author": "third-party", "ai_mode_required": "Fast",
            "mcp_server": [{"tool_name": "search_notes",
                            "description": "Search.",
                            "parameters_schema": {"type": "object"}}],
            "mcp_client_access": []
        }]
    }
    reg = parse_registry(upgrade, reg, merge=True)
    notes = [a for a in reg if a["app_id"] == "notes"]
    check("upgrade keeps single notes entry", len(notes) == 1, len(notes))
    check("upgrade replaced version", notes[0]["version"] == "3.0.0",
          notes[0]["version"])
    check("upgrade replaced tool set",
          [t["tool_name"] for t in notes[0]["mcp_server"]] == ["search_notes"],
          [t["tool_name"] for t in notes[0]["mcp_server"]])

    # 重复安装相同 overlay 必须幂等
    before = json.dumps(reg, sort_keys=True)
    reg = parse_registry(overlay_json, reg, merge=True)
    reg = parse_registry(overlay_json, reg, merge=True)
    after_ids = [a["app_id"] for a in reg]
    check("repeated install is idempotent (no duplicate ids)",
          len(after_ids) == len(set(after_ids)), after_ids)

    print()
    print("pass=%d fail=%d" % (pass_n, fail_n))
    return 0 if fail_n == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
