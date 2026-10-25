#!/usr/bin/env python3
# =====================================================================
#  nexpack.py  -  NexOS .nexpack 打包 / 校验 / 检视 / 解包工具
# ---------------------------------------------------------------------
#  .nexpack 是 NexOS 的第三方应用包格式（对应 VS Code 的 .vsix）。
#  格式规范见 docs/NEXPACK.md。要点：
#
#    * 标准 ZIP 容器，但**所有成员必须 STORE（method 0，不压缩）**，
#      因为 NexOS 内核没有 zlib/inflate（见 plugins/vnc_client.cpp:393）。
#    * 成员：manifest.json（必需）、app.mex（必需）、icon.png（可选）。
#    * 所有人类可读元数据都在 manifest.json —— MiniCLR 没有反射，
#      扁平化的 app.mex 里读不出工具签名。
#
#  子命令：
#    pack    <--mex F> <--manifest F> [--icon F] <--out F>
#    verify  <pkg>
#    inspect <pkg>
#    unpack  <pkg> <--out DIR>
#    sign-info <pkg>          # 打印 SHA-256 清单（供发布页张贴）
# =====================================================================
import argparse
import hashlib
import json
import os
import re
import sys
import zipfile

SCHEMA_VERSION = 1

# 单包 / 单成员体积上限（见 docs/NEXPACK.md §8）
MAX_PACKAGE_BYTES = 4 * 1024 * 1024
MAX_MEX_BYTES     = 2 * 1024 * 1024
MAX_ICON_BYTES    = 64 * 1024

# app_id：小写字母开头，小写字母/数字/下划线，2..32 字符
APP_ID_RE   = re.compile(r'^[a-z][a-z0-9_]{1,31}$')
# tool_name：小写字母开头，小写字母/数字/下划线，2..64 字符
TOOL_NAME_RE = re.compile(r'^[a-z][a-z0-9_]{1,63}$')
# version：MAJOR.MINOR.PATCH，纯数字
VERSION_RE  = re.compile(r'^(\d+)\.(\d+)\.(\d+)$')

# 内置应用：第三方包不得覆盖（除非 --force）
RESERVED_APP_IDS = {"weather", "assistant", "shell", "nexos"}

AI_MODES = {"Fast", "Balanced", "Heavy"}

# .mex 魔数。mex_pack.py 产物头部是 'MEX1' + u32 版本（见 tools/diag/mex_dump.py:60）。
MEX_MAGIC = b"MEX1"


# ---------------------------------------------------------------------
#  校验工具
# ---------------------------------------------------------------------
def err(msg):
    print("error: " + msg, file=sys.stderr)


def sha256_of(data):
    return hashlib.sha256(data).hexdigest()


def parse_version(s):
    """'1.2.3' -> (1,2,3)；不合法返回 None。"""
    m = VERSION_RE.match(s or "")
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)))


def validate_manifest(m, mex_basename, force=False):
    """校验 manifest dict。返回错误字符串列表（空 = 通过）。"""
    e = []
    if not isinstance(m, dict):
        return ["manifest.json 顶层必须是 JSON 对象"]

    sv = m.get("schema_version")
    if sv != SCHEMA_VERSION:
        e.append("schema_version 必须是 %d（实际 %r）" % (SCHEMA_VERSION, sv))

    app_id = m.get("app_id")
    if not app_id or not isinstance(app_id, str):
        e.append("app_id 缺失或不是字符串")
    elif not APP_ID_RE.match(app_id):
        e.append("app_id %r 不合法（要求 %s）" % (app_id, APP_ID_RE.pattern))
    elif not force and app_id in RESERVED_APP_IDS:
        e.append("app_id %r 是内置应用保留名，拒绝安装（可用 --force 覆盖）" % app_id)
    elif mex_basename and app_id != mex_basename:
        e.append("app_id %r 必须等于 app.mex 的 basename %r"
                 % (app_id, mex_basename))

    for key in ("name", "version", "author"):
        v = m.get(key)
        if not v or not isinstance(v, str):
            e.append("%s 缺失或不是字符串" % key)
    if isinstance(m.get("version"), str) and parse_version(m["version"]) is None:
        e.append("version %r 不是 MAJOR.MINOR.PATCH" % m["version"])

    if "description" in m and not isinstance(m["description"], str):
        e.append("description 必须是字符串")

    # icon：必须是单段文件名
    icon = m.get("icon", "icon.png")
    if not isinstance(icon, str):
        e.append("icon 必须是字符串")
    elif "/" in icon or "\\" in icon or icon in (".", ".."):
        e.append("icon %r 必须是单段文件名（禁止路径分隔符）" % icon)

    # entry
    entry = m.get("entry", {})
    if not isinstance(entry, dict):
        e.append("entry 必须是对象")
    else:
        kind = entry.get("kind", 13)
        if not isinstance(kind, int) or isinstance(kind, bool) or kind < 0:
            e.append("entry.kind 必须是非负整数")
        for b in ("desktop", "taskbar"):
            if b in entry and not isinstance(entry[b], bool):
                e.append("entry.%s 必须是布尔值" % b)

    if "ai_mode_required" in m and m["ai_mode_required"] not in AI_MODES:
        e.append("ai_mode_required %r 必须是 %s 之一"
                 % (m["ai_mode_required"], sorted(AI_MODES)))

    # mcp_server
    names = set()
    srv = m.get("mcp_server", [])
    if not isinstance(srv, list):
        e.append("mcp_server 必须是数组")
    else:
        for i, t in enumerate(srv):
            p = "mcp_server[%d]" % i
            if not isinstance(t, dict):
                e.append(p + " 必须是对象")
                continue
            tn = t.get("tool_name")
            if not tn or not isinstance(tn, str):
                e.append(p + ".tool_name 缺失")
            elif not TOOL_NAME_RE.match(tn):
                e.append(p + ".tool_name %r 不合法（要求 %s）"
                         % (tn, TOOL_NAME_RE.pattern))
            elif tn in names:
                e.append(p + ".tool_name %r 重复" % tn)
            else:
                names.add(tn)
            if not isinstance(t.get("description"), str) or not t.get("description"):
                e.append(p + ".description 缺失")
            ps = t.get("parameters_schema")
            if not isinstance(ps, dict) or ps.get("type") != "object":
                e.append(p + ".parameters_schema 必须是 type=object 的 JSON Schema")

    # mcp_client_access
    acc = m.get("mcp_client_access", [])
    if not isinstance(acc, list) or any(not isinstance(a, str) for a in acc):
        e.append("mcp_client_access 必须是字符串数组")

    return e


def validate_mex(data):
    """校验 .mex 字节流。返回错误字符串列表。"""
    e = []
    if len(data) == 0:
        return [".mex 为空"]
    if len(data) > MAX_MEX_BYTES:
        e.append(".mex 体积 %d 超过上限 %d" % (len(data), MAX_MEX_BYTES))
    if not data.startswith(MEX_MAGIC):
        e.append(".mex 魔数不匹配：期望 %r，实际 %r"
                 % (MEX_MAGIC, data[:len(MEX_MAGIC)]))
    return e


def validate_member_name(name):
    """归档成员必须是根目录下的单段文件名。"""
    if not name or "/" in name or "\\" in name or name in (".", ".."):
        return "%r 必须是单段文件名（禁止目录与路径穿越）" % name
    return None


# ---------------------------------------------------------------------
#  pack
# ---------------------------------------------------------------------
def cmd_pack(args):
    problems = []

    # 读 manifest
    try:
        with open(args.manifest, "r", encoding="utf-8") as f:
            manifest = json.load(f)
    except FileNotFoundError:
        err("manifest 不存在: " + args.manifest)
        return 2
    except json.JSONDecodeError as ex:
        err("manifest.json 不是合法 JSON: %s" % ex)
        return 2

    # 读 .mex
    try:
        with open(args.mex, "rb") as f:
            mex = f.read()
    except FileNotFoundError:
        err(".mex 不存在: " + args.mex)
        return 2

    mex_base = os.path.splitext(os.path.basename(args.mex))[0]
    # 统一以小写比较：app_id 全小写，mex 文件名可能首字母大写
    if mex_base.lower() != (manifest.get("app_id") or "").lower():
        # 允许 Weather.mex vs weather 的情形（大小写差异），但打印提示
        print("note: .mex basename %r 与 app_id %r 大小写不同（已按小写比较通过）"
              % (mex_base, manifest.get("app_id")))

    problems += validate_manifest(manifest, mex_base.lower(), force=args.force)
    problems += validate_mex(mex)

    # 读 icon（可选）
    icon_name = manifest.get("icon", "icon.png")
    icon = None
    if args.icon:
        try:
            with open(args.icon, "rb") as f:
                icon = f.read()
        except FileNotFoundError:
            err("icon 不存在: " + args.icon)
            return 2
        if len(icon) > MAX_ICON_BYTES:
            problems.append("icon 体积 %d 超过上限 %d" % (len(icon), MAX_ICON_BYTES))
        if not icon.startswith(b"\x89PNG\r\n\x1a\n"):
            problems.append("icon 不是 PNG（缺少 PNG 签名）")
    elif icon_name:
        print("note: 未提供 --icon，包内将不含图标（桌面使用字母字形回退）")

    if problems:
        err("打包校验失败：")
        for p in problems:
            print("  - " + p, file=sys.stderr)
        return 1

    # 计算总大小预检
    total = len(mex) + len(json.dumps(manifest, ensure_ascii=False).encode("utf-8"))
    if icon:
        total += len(icon)
    if total > MAX_PACKAGE_BYTES:
        err("包体积 %d 超过上限 %d" % (total, MAX_PACKAGE_BYTES))
        return 1

    # 写 ZIP（全部 STORE）
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    manifest_bytes = json.dumps(manifest, ensure_ascii=False, indent=2).encode("utf-8")

    with zipfile.ZipFile(args.out, "w", compression=zipfile.ZIP_STORED) as z:
        # manifest.json 放最前：流式解包时可先读到元数据
        z.writestr("manifest.json", manifest_bytes)
        z.writestr("app.mex", mex)
        if icon:
            z.writestr(icon_name, icon)

    size = os.path.getsize(args.out)
    print("packed: %s (%d bytes)" % (args.out, size))
    print("  app_id   : %s" % manifest.get("app_id"))
    print("  version  : %s" % manifest.get("version"))
    print("  members  : manifest.json, app.mex%s" % (", " + icon_name if icon else ""))
    print("  tools    : %d" % len(manifest.get("mcp_server", [])))
    for t in manifest.get("mcp_server", []):
        print("             - %s" % t.get("tool_name"))
    print("  sha256   : %s" % sha256_of(open(args.out, "rb").read()))
    return 0


# ---------------------------------------------------------------------
#  verify
# ---------------------------------------------------------------------
def open_package(path):
    """打开并做结构检查。返回 (zipfile, problems)。"""
    problems = []
    if not os.path.isfile(path):
        return None, ["包不存在: " + path]
    if os.path.getsize(path) > MAX_PACKAGE_BYTES:
        problems.append("包体积 %d 超过上限 %d"
                        % (os.path.getsize(path), MAX_PACKAGE_BYTES))
    try:
        z = zipfile.ZipFile(path, "r")
    except zipfile.BadZipFile as ex:
        return None, ["不是合法 ZIP: %s" % ex]

    names = z.namelist()
    # 成员名合法性
    for n in names:
        p = validate_member_name(n)
        if p:
            problems.append(p)
    # 必需成员
    for req in ("manifest.json", "app.mex"):
        if req not in names:
            problems.append("缺少必需成员: " + req)
    # 压缩方法必须是 STORE
    for info in z.infolist():
        if info.compress_type != zipfile.ZIP_STORED:
            problems.append("成员 %s 使用了压缩（method=%d），必须是 STORE(0) —— "
                            "内核没有 inflate" % (info.filename, info.compress_type))
    # 未知成员：仅允许 icon，其它警告
    known = {"manifest.json", "app.mex", "icon.png"}
    m = None
    if "manifest.json" in names:
        try:
            m = json.loads(z.read("manifest.json").decode("utf-8"))
        except Exception as ex:
            problems.append("manifest.json 解析失败: %s" % ex)
    if m:
        known.add(m.get("icon", "icon.png"))
    for n in names:
        if n not in known:
            problems.append("未知成员: %s（规范只允许 manifest.json/app.mex/icon）" % n)

    return z, problems


def cmd_verify(args):
    z, problems = open_package(args.pkg)
    if z is None:
        err("校验失败：")
        for p in problems:
            print("  - " + p, file=sys.stderr)
        return 1

    names = z.namelist()
    manifest = None
    if "manifest.json" in names:
        try:
            manifest = json.loads(z.read("manifest.json").decode("utf-8"))
        except Exception:
            pass

    if "app.mex" in names:
        problems += validate_mex(z.read("app.mex"))

    if manifest is not None:
        mex_base = ""
        if "app.mex" in names:
            mex_base = (manifest.get("app_id") or "").lower()
        problems += validate_manifest(manifest, mex_base, force=True)

        # icon 声明的成员必须存在（若 manifest 声明了 icon 且包内有图标成员）
        icon = manifest.get("icon")
        if icon and icon in names:
            data = z.read(icon)
            if len(data) > MAX_ICON_BYTES:
                problems.append("icon 体积 %d 超过上限" % len(data))
            if not data.startswith(b"\x89PNG\r\n\x1a\n"):
                problems.append("icon 不是 PNG")

    z.close()

    if problems:
        err("校验失败：")
        for p in problems:
            print("  - " + p, file=sys.stderr)
        return 1

    print("OK: %s" % args.pkg)
    print("  app_id : %s" % manifest.get("app_id"))
    print("  version: %s" % manifest.get("version"))
    print("  tools  : %d" % len(manifest.get("mcp_server", [])))
    return 0


# ---------------------------------------------------------------------
#  inspect
# ---------------------------------------------------------------------
def cmd_inspect(args):
    z, problems = open_package(args.pkg)
    if z is None:
        err("无法打开：")
        for p in problems:
            print("  - " + p, file=sys.stderr)
        return 1

    print("package : %s" % args.pkg)
    print("size    : %d bytes" % os.path.getsize(args.pkg))
    print("sha256  : %s" % sha256_of(open(args.pkg, "rb").read()))
    print("members :")
    for info in z.infolist():
        method = "STORE" if info.compress_type == zipfile.ZIP_STORED else "DEFLATE"
        print("  %-16s %8d bytes  crc=%08x  %s"
              % (info.filename, info.file_size, info.CRC, method))

    names = z.namelist()
    if "manifest.json" not in names:
        err("缺少 manifest.json，无法继续检视")
        return 1
    m = json.loads(z.read("manifest.json").decode("utf-8"))

    print("\nmanifest:")
    print("  schema_version : %s" % m.get("schema_version"))
    print("  app_id         : %s" % m.get("app_id"))
    print("  name           : %s" % m.get("name"))
    print("  version        : %s" % m.get("version"))
    print("  author         : %s" % m.get("author"))
    print("  ai_mode        : %s" % m.get("ai_mode_required", "Balanced"))
    entry = m.get("entry", {})
    print("  entry.kind     : %s" % entry.get("kind", 13))
    print("  entry.desktop  : %s" % entry.get("desktop", True))

    srv = m.get("mcp_server", [])
    print("\nmcp_server (%d tools):" % len(srv))
    for t in srv:
        print("  - %s : %s" % (t.get("tool_name"), t.get("description")))
        ps = t.get("parameters_schema", {})
        props = ps.get("properties", {})
        req = ps.get("required", [])
        for pname, pdef in props.items():
            mark = "*" if pname in req else " "
            print("      %s %-14s %s" % (mark, pname, pdef.get("type", "?")))

    acc = m.get("mcp_client_access", [])
    print("\nmcp_client_access: %s" % (acc if acc else "(none)"))

    if problems:
        print("\nwarnings:")
        for p in problems:
            print("  - " + p)
    z.close()
    return 0


# ---------------------------------------------------------------------
#  unpack
# ---------------------------------------------------------------------
def cmd_unpack(args):
    z, problems = open_package(args.pkg)
    if z is None:
        err("无法打开：")
        for p in problems:
            print("  - " + p, file=sys.stderr)
        return 1
    if problems:
        err("包结构有问题，拒绝解包：")
        for p in problems:
            print("  - " + p, file=sys.stderr)
        z.close()
        return 1

    os.makedirs(args.out, exist_ok=True)
    for info in z.infolist():
        # 已通过 validate_member_name，这里再做一次防御性检查
        if validate_member_name(info.filename):
            continue
        dest = os.path.join(args.out, info.filename)
        with open(dest, "wb") as f:
            f.write(z.read(info.filename))
        print("  %s -> %s (%d bytes)" % (info.filename, dest, info.file_size))
    z.close()
    print("unpacked: %s" % args.out)
    return 0


# ---------------------------------------------------------------------
#  sign-info
# ---------------------------------------------------------------------
def cmd_sign_info(args):
    z, problems = open_package(args.pkg)
    if z is None:
        err("无法打开：")
        for p in problems:
            print("  - " + p, file=sys.stderr)
        return 1
    print("file: %s" % args.pkg)
    print("sha256: %s" % sha256_of(open(args.pkg, "rb").read()))
    print("members:")
    for info in z.infolist():
        data = z.read(info.filename)
        print("  %-16s sha256=%s" % (info.filename, sha256_of(data)))
    z.close()
    return 0


# ---------------------------------------------------------------------
#  main
# ---------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="nexpack",
        description="NexOS .nexpack 应用包工具（格式见 docs/NEXPACK.md）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack", help="把 .mex + manifest.json (+icon) 打成 .nexpack")
    p.add_argument("--mex", required=True, help="托管程序集（mex_pack.py 产物）")
    p.add_argument("--manifest", required=True, help="manifest.json 路径")
    p.add_argument("--icon", help="图标 PNG 路径（可选）")
    p.add_argument("--out", required=True, help="输出 .nexpack 路径")
    p.add_argument("--force", action="store_true", help="允许覆盖内置应用 app_id")
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("verify", help="校验包结构与 manifest")
    p.add_argument("pkg")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("inspect", help="打印包内容与 MCP 工具清单")
    p.add_argument("pkg")
    p.set_defaults(func=cmd_inspect)

    p = sub.add_parser("unpack", help="解包到目录")
    p.add_argument("pkg")
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_unpack)

    p = sub.add_parser("sign-info", help="打印 SHA-256 清单")
    p.add_argument("pkg")
    p.set_defaults(func=cmd_sign_info)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
