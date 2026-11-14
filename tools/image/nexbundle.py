#!/usr/bin/env python3
# =====================================================================
#  nexbundle.py  -  NexOS .nexbundle 插件整合包工具
# ---------------------------------------------------------------------
#  一切皆插件：.nexbundle 只分发**多插件 Bundle**，不再有“单体应用包”。
#  格式规范见 docs/NEXBUNDLE.md。与 .nexpack 的关键差异：
#
#    .nexpack   一个包 = 一个应用 = 一个 .mex
#    .nexbundle 一个包 = 一组插件，每个插件有自己的 plugin.json、
#               能力申请、沙盒根、ui_layout 和 MCP 工具表
#
#  容器仍是标准 ZIP，但**所有成员必须 STORE（method 0）**：NexOS 内核
#  没有 zlib/inflate（见 plugins/vnc_client.cpp:393），安装器在内核侧
#  直接按偏移读成员。
#
#  目录结构（pack 的 --src 根）：
#
#      manifest.json                     必需，Bundle 清单
#      plugins/<id>/plugin.json          必需，单插件清单
#      plugins/<id>/<assembly>.mex       托管插件（kind=managed）
#      plugins/<id>/ui/layout.json       ui_layout 指向的布局
#      ui/layout.json                    Bundle 级共享布局（可选）
#      icon.png                          可选，≤64 KiB
#      signatures/ed25519.json           可选，由 --sign 生成
#
#  安全约束（verify 逐条强制）：
#
#    * 成员名禁止绝对路径、`..`、反斜杠、控制字符、Windows 盘符；
#    * 每个插件的 id 必须匹配其目录名；
#    * 能力名必须是已知 CAP_*，`ipc` 一律拒绝（单进程零 IPC）；
#    * `.mex` 必须以 MEX1 魔数开头；
#    * 未签名包**允许分发**，但安装时内核会摘掉 CAP_FS_WRITE，
#      并把沙盒根强制改成 /sandbox/<plugin_id>（见 plugin_loader.cpp）。
#
#  签名（可选，纯 Python Ed25519，无第三方依赖）：
#
#      待签字节 = bundle_id + "\n" + version + "\n" + tree_sha256
#      tree_sha256 = SHA256( 按名字排序的 "<member>\0<sha256(member)>" )
#
#  子命令：
#      pack     --src DIR --out FILE [--sign KEYHEX] [--key-id ID]
#      verify   FILE [--pubkey HEX] [--strict]
#      inspect  FILE
#      unpack   FILE --out DIR [--force]
#      tree-hash FILE
#      selftest
# =====================================================================
import argparse
import hashlib
import json
import os
import re
import sys
import zipfile

SCHEMA_VERSION = 1

# 体积上限（对齐 docs/NEXBUNDLE.md §7）
MAX_BUNDLE_BYTES = 16 * 1024 * 1024
MAX_MEX_BYTES = 8 * 1024 * 1024
MAX_ICON_BYTES = 64 * 1024
MAX_MEMBERS = 4096

# 规范里固定写死的成员前缀
SIGNATURE_PREFIX = "signatures/"

# bundle id：小写字母开头，允许 . _ -，2..64 字符。
# 插件 id 更严格（2..32），因为内核 PluginContext.id 只有 32 字节。
BUNDLE_ID_RE = re.compile(r'^[a-z][a-z0-9._-]{1,63}$')
PLUGIN_ID_RE = re.compile(r'^[a-z][a-z0-9._-]{1,31}$')
TOOL_NAME_RE = re.compile(r'^[a-z][a-z0-9_]{1,63}$')
VERSION_RE = re.compile(r'^(\d+)\.(\d+)\.(\d+)$')
SYMBOL_RE = re.compile(r'^[A-Za-z_][A-Za-z0-9_.]*::[A-Za-z_][A-Za-z0-9_]*$')

MEX_MAGIC = b"MEX1"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

# 与 plugins/loader/plugin_loader.h 的 CAP_* / 工具能力表保持一致。
CAPABILITY_NAMES = {
    "fs.read": 0x00000001,
    "fs.write": 0x00000002,
    "net": 0x00000004,
    "proc": 0x00000008,
    "ui": 0x00000010,
    "ai": 0x00000020,
    "dev": 0x00000040,
    "sys.info": 0x00000080,
}
CAPABILITY_ALIASES = {
    "fs_read": "fs.read",
    "fs_write": "fs.write",
    "net.http": "net",
    "net_http": "net",
    "sys.proc": "proc",
    "ui.patch": "ui",
    "ai.infer": "ai",
    "dev.ioctl": "dev",
    "sys_info": "sys.info",
    "ipc": "ipc",
    "sys.ipc": "ipc",
}

PLUGIN_KINDS = {"managed", "native", "builtin"}


# ---------------------------------------------------------------------
#  基础工具
# ---------------------------------------------------------------------
def err(msg):
    print("error: " + msg, file=sys.stderr)


def warn(msg):
    print("warning: " + msg, file=sys.stderr)


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def parse_version(s):
    m = VERSION_RE.match(s or "")
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)))


def canonical_capability(name):
    """返回规范名；未知返回 None，'ipc' 返回 ('ipc' 特殊值)。"""
    if not isinstance(name, str):
        return None
    name = name.strip()
    if name in CAPABILITY_NAMES:
        return name
    return CAPABILITY_ALIASES.get(name)


def safe_relpath(path):
    """归档成员名 / manifest 内路径的合法性检查。返回错误字符串或 None。"""
    if not isinstance(path, str) or not path:
        return "路径必须是非空字符串"
    if len(path) > 512:
        return "路径过长（>512）"
    if "\\" in path:
        return "%r 含反斜杠（包内一律用 '/')" % path
    if path.startswith("/") or re.match(r"^[A-Za-z]:", path):
        return "%r 是绝对路径" % path
    if any(ord(c) < 0x20 or ord(c) == 0x7F for c in path):
        return "%r 含控制字符" % path
    parts = path.split("/")
    for part in parts:
        if part in ("", ".", ".."):
            return "%r 含空段 / '.' / '..'" % path
    return None


def read_json(path, what):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        err("%s 不存在: %s" % (what, path))
    except json.JSONDecodeError as ex:
        err("%s 不是合法 JSON: %s" % (what, ex))
    except OSError as ex:
        err("读取 %s 失败: %s" % (what, ex))
    return None


def print_problems(problems, headline="校验失败："):
    err(headline)
    for p in problems:
        print("  - " + p, file=sys.stderr)


# =====================================================================
#  1. 清单校验
# =====================================================================
def validate_tool(tool, where, errors):
    if not isinstance(tool, dict):
        errors.append("%s 必须是对象" % where)
        return
    name = tool.get("name")
    if not isinstance(name, str) or not TOOL_NAME_RE.match(name):
        errors.append("%s.name %r 不合法（要求 %s）"
                      % (where, name, TOOL_NAME_RE.pattern))
    if not isinstance(tool.get("description"), str) or not tool.get("description"):
        errors.append("%s.description 缺失或为空" % where)
    schema = tool.get("inputSchema")
    if schema is None:
        schema = tool.get("parameters_schema")
    if not isinstance(schema, dict) or schema.get("type") != "object":
        errors.append("%s.inputSchema 必须是 type=object 的 JSON Schema" % where)


def validate_network(net, where, errors, warnings):
    if net is None:
        return
    if not isinstance(net, dict):
        errors.append("%s 必须是对象" % where)
        return
    hosts = net.get("allow_hosts", [])
    if not isinstance(hosts, list):
        errors.append("%s.allow_hosts 必须是字符串数组" % where)
        return
    for i, h in enumerate(hosts):
        w = "%s.allow_hosts[%d]" % (where, i)
        if not isinstance(h, str) or not h.strip():
            errors.append("%s 必须是非空字符串" % w)
            continue
        if "/" in h or ":" in h or "@" in h:
            errors.append("%s %r 必须是裸域名（不要 scheme/端口/路径）" % (w, h))
        if h.strip() == "*":
            errors.append("%s 不允许裸 '*'（会匹配所有域名）" % w)
            continue
        if "*" in h and not h.startswith("*."):
            errors.append("%s %r 的通配符只能是前缀 '*.'" % (w, h))
        elif h.startswith("*.") and "." not in h[2:]:
            errors.append("%s %r 的通配符基域不合法" % (w, h))
    if hosts:
        warnings.append("%s 申请了出站网络：%s" % (where, ", ".join(hosts)))


def validate_capabilities(caps, where, errors):
    if caps is None:
        return set()
    if not isinstance(caps, list):
        errors.append("%s 必须是字符串数组" % where)
        return set()
    out = set()
    for i, raw in enumerate(caps):
        name = canonical_capability(raw)
        if name is None:
            errors.append("%s[%d] %r 不是已知能力（见 CAP_* 表）" % (where, i, raw))
            continue
        if name == "ipc":
            errors.append("%s[%d] 申请了 ipc：NexOS 单进程零 IPC，禁止加载"
                          % (where, i))
            continue
        out.add(name)
    return out


def validate_plugin(plugin, index, errors, warnings):
    where = "plugins[%d]" % index
    if not isinstance(plugin, dict):
        errors.append(where + " 必须是对象")
        return None

    pid = plugin.get("id")
    if not isinstance(pid, str) or not PLUGIN_ID_RE.match(pid):
        errors.append("%s.id %r 不合法（要求 %s）"
                      % (where, pid, PLUGIN_ID_RE.pattern))

    for key in ("name", "version"):
        if not isinstance(plugin.get(key), str) or not plugin.get(key):
            errors.append("%s.%s 缺失或不是非空字符串" % (where, key))
    if isinstance(plugin.get("version"), str) and parse_version(plugin["version"]) is None:
        errors.append("%s.version %r 不是 MAJOR.MINOR.PATCH" % (where, plugin["version"]))

    entry = plugin.get("entry", {})
    if not isinstance(entry, dict):
        errors.append(where + ".entry 必须是对象")
        entry = {}
    kind = entry.get("kind", "managed")
    if kind not in PLUGIN_KINDS:
        errors.append("%s.entry.kind %r 必须是 %s 之一"
                      % (where, kind, sorted(PLUGIN_KINDS)))

    assembly = entry.get("assembly")
    if kind == "managed":
        if not isinstance(assembly, str):
            errors.append(where + ".entry.assembly 缺失（托管插件必须指定 .mex）")
        else:
            bad = safe_relpath(assembly)
            if bad:
                errors.append(where + ".entry.assembly " + bad)
            elif "/" in assembly:
                errors.append(where + ".entry.assembly 必须是插件目录下的单段文件名")
            elif not assembly.endswith(".mex"):
                errors.append(where + ".entry.assembly 必须以 .mex 结尾")

    for sym in ("init_symbol", "dispatch_symbol"):
        if sym in entry and entry[sym] is not None:
            if not isinstance(entry[sym], str) or not SYMBOL_RE.match(entry[sym]):
                errors.append("%s.entry.%s %r 必须是 Type::Method"
                              % (where, sym, entry[sym]))

    caps = validate_capabilities(plugin.get("capabilities", []),
                                 where + ".capabilities", errors)
    validate_network(plugin.get("network"), where + ".network", errors, warnings)

    layout = plugin.get("ui_layout")
    if layout is not None and not isinstance(layout, (str, dict)):
        errors.append(where + ".ui_layout 必须是路径字符串或内联对象")

    tools = plugin.get("tools", [])
    if not isinstance(tools, list):
        errors.append(where + ".tools 必须是数组")
        tools = []
    seen = set()
    for i, tool in enumerate(tools):
        validate_tool(tool, "%s.tools[%d]" % (where, i), errors)
        if isinstance(tool, dict) and isinstance(tool.get("name"), str):
            if tool["name"] in seen:
                errors.append("%s.tools 里 %r 重复" % (where, tool["name"]))
            seen.add(tool["name"])

    has_native_entry = kind in ("native", "builtin")
    if not tools and not has_native_entry and not entry.get("init_symbol"):
        warnings.append("%s 既没有 MCP 工具也没有 init_symbol" % where)

    return pid if isinstance(pid, str) else None


def validate_manifest(m, errors, warnings):
    if not isinstance(m, dict):
        errors.append("manifest.json 顶层必须是 JSON 对象")
        return None

    if m.get("schema_version") != SCHEMA_VERSION:
        errors.append("schema_version 必须是 %d（实际 %r）"
                      % (SCHEMA_VERSION, m.get("schema_version")))

    bid = m.get("id")
    if not isinstance(bid, str) or not BUNDLE_ID_RE.match(bid):
        errors.append("id %r 不合法（要求 %s）" % (bid, BUNDLE_ID_RE.pattern))

    for key in ("name", "version"):
        if not isinstance(m.get(key), str) or not m.get(key):
            errors.append("%s 缺失或不是非空字符串" % key)
    if isinstance(m.get("version"), str) and parse_version(m["version"]) is None:
        errors.append("version %r 不是 MAJOR.MINOR.PATCH" % m["version"])

    for key in ("publisher", "description", "license", "homepage"):
        if key in m and not isinstance(m[key], str):
            errors.append("%s 必须是字符串" % key)

    # 规范要求 Bundle 清单显式声明这三项，避免“默认放行”的歧义：
    #   network: 空 allow_hosts 也必须写出来，表示“我知道它不联网”
    #   capabilities: 空数组也必须写出来
    #   ui_layout: 必须有，否则前端不知道渲染什么
    for key in ("network", "capabilities", "ui_layout"):
        if key not in m:
            errors.append("%s 缺失（Bundle manifest 必须显式声明）" % key)

    validate_capabilities(m.get("capabilities", []), "capabilities", errors)
    validate_network(m.get("network"), "network", errors, warnings)

    layout = m.get("ui_layout")
    if layout is not None and not isinstance(layout, (str, dict)):
        errors.append("ui_layout 必须是路径字符串或内联对象")

    plugins = m.get("plugins")
    if not isinstance(plugins, list) or not plugins:
        errors.append("plugins 必须是非空数组（.nexbundle 只分发多插件 Bundle）")
        return bid if isinstance(bid, str) else None

    ids = []
    for i, plugin in enumerate(plugins):
        pid = validate_plugin(plugin, i, errors, warnings)
        if pid:
            if pid in ids:
                errors.append("插件 id %r 重复" % pid)
            ids.append(pid)

    sigs = m.get("signatures", [])
    if sigs and not isinstance(sigs, list):
        errors.append("signatures 必须是数组")

    return bid if isinstance(bid, str) else None


# =====================================================================
#  2. 源码目录 -> 成员表
# =====================================================================
SKIP_DIRS = {".git", "__pycache__", "obj", "bin", "dist", ".vs"}
SKIP_FILES = {".DS_Store", "Thumbs.db"}


def collect_members(src):
    """遍历 --src，返回 (members, problems)。members: {relpath: bytes}。"""
    members = {}
    problems = []

    if not os.path.isdir(src):
        return None, ["--src 不是目录: " + src]

    for root, dirs, files in os.walk(src):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for name in sorted(files):
            if name in SKIP_FILES or name.endswith((".pdb", ".mdb")):
                continue
            full = os.path.join(root, name)
            rel = os.path.relpath(full, src).replace(os.sep, "/")
            bad = safe_relpath(rel)
            if bad:
                problems.append(bad)
                continue
            # 签名文件由 --sign 重新生成，不采信目录里的旧文件。
            if rel.startswith(SIGNATURE_PREFIX):
                continue
            try:
                with open(full, "rb") as f:
                    members[rel] = f.read()
            except OSError as ex:
                problems.append("读取 %s 失败: %s" % (rel, ex))

    if len(members) > MAX_MEMBERS:
        problems.append("成员数 %d 超过上限 %d" % (len(members), MAX_MEMBERS))
    return members, problems


def member_tree(members):
    """剔除签名目录后的内容树（用于 tree hash）。"""
    return {k: v for k, v in members.items() if not k.startswith(SIGNATURE_PREFIX)}


def compute_tree_hash(members):
    """确定性树哈希：按名字排序，逐成员拼 "<name>\\0<sha256hex>\\n"。"""
    h = hashlib.sha256()
    for name in sorted(members):
        h.update(name.encode("utf-8"))
        h.update(b"\x00")
        h.update(hashlib.sha256(members[name]).hexdigest().encode("ascii"))
        h.update(b"\n")
    return h.hexdigest()


def signature_subject(bundle_id, version, tree_hash):
    return ("%s\n%s\n%s" % (bundle_id, version, tree_hash)).encode("utf-8")


def cross_check_layouts(bundle, members, errors):
    """manifest 里引用的路径必须在包内存在（内联对象除外）。"""
    top = bundle.get("ui_layout")
    if isinstance(top, str):
        if safe_relpath(top) is None and top not in members:
            errors.append("Bundle ui_layout 指向的 %r 不在包内" % top)

    for i, plugin in enumerate(bundle.get("plugins") or []):
        pid = plugin.get("id")
        layout = plugin.get("ui_layout")
        if not isinstance(layout, str):
            continue
        if safe_relpath(layout) is not None:
            errors.append("plugins[%d].ui_layout %r 路径不合法" % (i, layout))
            continue
        candidates = [layout]
        if isinstance(pid, str) and not layout.startswith("plugins/"):
            candidates.insert(0, "plugins/%s/%s" % (pid, layout))
        if not any(c in members for c in candidates):
            errors.append("plugins[%d].ui_layout 指向的 %r 不在包内（尝试过 %s）"
                          % (i, layout, ", ".join(candidates)))


def cross_check_assemblies(bundle, members, errors):
    for i, plugin in enumerate(bundle.get("plugins") or []):
        entry = plugin.get("entry") or {}
        if entry.get("kind", "managed") != "managed":
            continue
        pid = plugin.get("id")
        assembly = entry.get("assembly")
        if not isinstance(pid, str) or not isinstance(assembly, str):
            continue
        rel = "plugins/%s/%s" % (pid, assembly)
        if rel not in members:
            errors.append("plugins[%d] 的 %r 不在包内（期望 %s）" % (i, assembly, rel))
            continue
        data = members[rel]
        if len(data) > MAX_MEX_BYTES:
            errors.append("%s 体积 %d 超过上限 %d" % (rel, len(data), MAX_MEX_BYTES))
        if not data.startswith(MEX_MAGIC):
            errors.append("%s 魔数不是 MEX1（不是 mex_pack.py 产物）" % rel)


def cross_check_plugin_json(bundle, members, errors, warnings):
    """plugins/<id>/plugin.json 必须存在，且 id/version 与 manifest 一致。"""
    for i, plugin in enumerate(bundle.get("plugins") or []):
        pid = plugin.get("id")
        if not isinstance(pid, str):
            continue
        rel = "plugins/%s/plugin.json" % pid
        if rel not in members:
            errors.append("缺少 %s（每个插件都必须有独立清单）" % rel)
            continue
        try:
            sub = json.loads(members[rel].decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as ex:
            errors.append("%s 解析失败: %s" % (rel, ex))
            continue

        sub_errors = []
        sub_warnings = []
        if not isinstance(sub, dict):
            errors.append("%s 顶层必须是对象" % rel)
            continue
        if sub.get("schema_version") != SCHEMA_VERSION:
            sub_errors.append("%s.schema_version 必须是 %d" % (rel, SCHEMA_VERSION))
        if sub.get("id") != pid:
            sub_errors.append("%s 的 id %r 与目录 / bundle 清单 %r 不一致"
                              % (rel, sub.get("id"), pid))
        if sub.get("version") != plugin.get("version"):
            sub_errors.append("%s 的 version %r 与 bundle 清单 %r 不一致"
                              % (rel, sub.get("version"), plugin.get("version")))
        validate_plugin(sub, i, sub_errors, sub_warnings)
        errors.extend(sub_errors)
        warnings.extend(sub_warnings)


# =====================================================================
#  3. Ed25519（RFC 8032，纯 Python，无依赖）
# =====================================================================
_P = 2 ** 255 - 19
_L = 2 ** 252 + 27742317777372353535851937790883648493
_D = (-121665 * pow(121666, _P - 2, _P)) % _P
_I = pow(2, (_P - 1) // 4, _P)


def _inv(x):
    return pow(x, _P - 2, _P)


def _xrecover(y):
    xx = (y * y - 1) * _inv(_D * y * y + 1)
    x = pow(xx, (_P + 3) // 8, _P)
    if (x * x - xx) % _P != 0:
        x = (x * _I) % _P
    if x % 2 != 0:
        x = _P - x
    return x


_BY = (4 * _inv(5)) % _P
_BX = _xrecover(_BY)
_B = (_BX % _P, _BY, 1, (_BX * _BY) % _P)


def _edwards_add(p, q):
    x1, y1, z1, t1 = p
    x2, y2, z2, t2 = q
    a = (y1 - x1) * (y2 - x2) % _P
    b = (y1 + x1) * (y2 + x2) % _P
    c = 2 * t1 * t2 * _D % _P
    d = 2 * z1 * z2 % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _scalar_mult(point, scalar):
    result = (0, 1, 1, 0)
    while scalar > 0:
        if scalar & 1:
            result = _edwards_add(result, point)
        point = _edwards_add(point, point)
        scalar >>= 1
    return result


def _encode_point(point):
    x, y, z, _t = point
    zi = _inv(z)
    x = x * zi % _P
    y = y * zi % _P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _decode_point(data):
    if len(data) != 32:
        raise ValueError("point must be 32 bytes")
    value = int.from_bytes(data, "little")
    y = value & ((1 << 255) - 1)
    sign = value >> 255
    if y >= _P:
        raise ValueError("y out of range")
    x = _xrecover(y)
    if x & 1 != sign:
        x = _P - x
    if (-x * x + y * y - 1 - _D * x * x * y * y) % _P != 0:
        raise ValueError("point is not on curve")
    return (x, y, 1, x * y % _P)


def ed25519_public_key(seed):
    if len(seed) != 32:
        raise ValueError("Ed25519 seed must be 32 bytes")
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return _encode_point(_scalar_mult(_B, a))


def ed25519_sign(seed, message):
    if len(seed) != 32:
        raise ValueError("Ed25519 seed must be 32 bytes")
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    prefix = h[32:]
    public = _encode_point(_scalar_mult(_B, a))
    r = int.from_bytes(hashlib.sha512(prefix + message).digest(), "little") % _L
    r_encoded = _encode_point(_scalar_mult(_B, r))
    k = int.from_bytes(hashlib.sha512(r_encoded + public + message).digest(),
                       "little") % _L
    s = (r + k * a) % _L
    return public, r_encoded + s.to_bytes(32, "little")


def ed25519_verify(public, message, signature):
    if len(public) != 32 or len(signature) != 64:
        return False
    try:
        a_point = _decode_point(public)
        r_encoded = signature[:32]
        r_point = _decode_point(r_encoded)
    except ValueError:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= _L:
        return False
    k = int.from_bytes(hashlib.sha512(r_encoded + public + message).digest(),
                       "little") % _L
    left = _scalar_mult(_B, s)
    right = _edwards_add(r_point, _scalar_mult(a_point, k))
    return _encode_point(left) == _encode_point(right)


def parse_hex_key(text):
    text = text.strip()
    if text.startswith("0x"):
        text = text[2:]
    text = "".join(text.split())
    data = bytes.fromhex(text)
    if len(data) != 32:
        raise ValueError("key must be 32 bytes (64 hex chars), got %d" % len(data))
    return data


# =====================================================================
#  4. pack
# =====================================================================
def zip_info(name):
    """确定性 ZIP 条目：固定时间戳 + 固定权限，保证同输入同输出。"""
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = (0o644 & 0xFFFF) << 16
    info.create_system = 3
    return info


def cmd_pack(args):
    members, problems = collect_members(args.src)
    if members is None:
        print_problems(problems, "无法读取 --src：")
        return 2

    if "manifest.json" not in members:
        err("缺少 %s/manifest.json" % args.src)
        return 2

    try:
        bundle = json.loads(members["manifest.json"].decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as ex:
        err("manifest.json 解析失败: %s" % ex)
        return 2

    warnings = []
    validate_manifest(bundle, problems, warnings)
    cross_check_assemblies(bundle, members, problems)
    cross_check_plugin_json(bundle, members, problems, warnings)
    cross_check_layouts(bundle, members, problems)

    icon = bundle.get("icon", "icon.png")
    if isinstance(icon, str) and icon in members:
        data = members[icon]
        if len(data) > MAX_ICON_BYTES:
            problems.append("%s 体积 %d 超过上限 %d" % (icon, len(data), MAX_ICON_BYTES))
        if not data.startswith(PNG_MAGIC):
            problems.append("%s 不是 PNG" % icon)

    if args.expect_id and bundle.get("id") != args.expect_id:
        problems.append("bundle id %r 与 --expect-id %r 不一致"
                        % (bundle.get("id"), args.expect_id))

    if problems:
        print_problems(problems, "打包前校验失败：")
        return 1

    for w in warnings:
        warn(w)

    # ---- 树哈希（签名前，剔除 signatures/） ----
    tree_members = member_tree(members)
    tree_hash = compute_tree_hash(tree_members)

    # ---- 可选签名 ----
    if args.sign:
        try:
            seed = parse_hex_key(args.sign)
        except ValueError as ex:
            err("--sign 必须是 32 字节十六进制种子: %s" % ex)
            return 2
        subject = signature_subject(bundle["id"], bundle["version"], tree_hash)
        public, signature = ed25519_sign(seed, subject)
        key_id = args.key_id or sha256_hex(public)[:16]
        record = {
            "alg": "ed25519",
            "key_id": key_id,
            "public_key": public.hex(),
            "signature": signature.hex(),
            "subject": {
                "bundle_id": bundle["id"],
                "version": bundle["version"],
                "tree_sha256": tree_hash,
            },
        }
        members[SIGNATURE_PREFIX + "ed25519.json"] = (
            json.dumps(record, indent=2, ensure_ascii=False) + "\n"
        ).encode("utf-8")
        print("signed: key_id=%s pubkey=%s" % (key_id, public.hex()))
    else:
        warnings.append("包未签名：安装时内核会摘掉 CAP_FS_WRITE 并强制沙盒根为 "
                        "/sandbox/<plugin_id>")

    total = sum(len(v) for v in members.values())
    if total > MAX_BUNDLE_BYTES:
        err("解压后总体积 %d 超过上限 %d" % (total, MAX_BUNDLE_BYTES))
        return 1

    # manifest.json 放最前；其余按名字排序，保证可复现。
    order = ["manifest.json"] + sorted(n for n in members if n != "manifest.json")

    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", compression=zipfile.ZIP_STORED) as z:
        z.comment = ("NexOS plugin bundle v%d" % SCHEMA_VERSION).encode("ascii")
        for name in order:
            z.writestr(zip_info(name), members[name])

    size = os.path.getsize(args.out)
    if size > MAX_BUNDLE_BYTES:
        err("包体积 %d 超过上限 %d" % (size, MAX_BUNDLE_BYTES))
        return 1

    plugins = bundle.get("plugins", [])
    tools = sum(len(p.get("tools", []) or []) for p in plugins)
    print("packed: %s (%d bytes)" % (args.out, size))
    print("  id      : %s" % bundle.get("id"))
    print("  version : %s" % bundle.get("version"))
    print("  plugins : %d" % len(plugins))
    for p in plugins:
        print("            - %s (%d tools)" % (p.get("id"), len(p.get("tools", []) or [])))
    print("  tools   : %d" % tools)
    print("  members : %d" % len(members))
    print("  tree    : %s" % tree_hash)
    print("  sha256  : %s" % sha256_hex(open(args.out, "rb").read()))
    for w in warnings:
        print("  note    : %s" % w)
    return 0


# =====================================================================
#  5. verify
# =====================================================================
def read_bundle(path):
    """打开包并做结构检查。返回 (zipfile, members, problems)。"""
    problems = []
    if not os.path.isfile(path):
        return None, {}, ["包不存在: " + path]
    size = os.path.getsize(path)
    if size > MAX_BUNDLE_BYTES:
        problems.append("包体积 %d 超过上限 %d" % (size, MAX_BUNDLE_BYTES))

    try:
        z = zipfile.ZipFile(path, "r")
    except zipfile.BadZipFile as ex:
        return None, {}, ["不是合法 ZIP: %s" % ex]

    members = {}
    seen = set()
    for info in z.infolist():
        name = info.filename
        bad = safe_relpath(name)
        if bad:
            problems.append("成员名非法：" + bad)
            continue
        if name in seen:
            problems.append("成员重复: " + name)
            continue
        seen.add(name)
        if info.compress_type != zipfile.ZIP_STORED:
            problems.append("成员 %s 使用了压缩（method=%d），必须是 STORE(0) —— "
                            "内核没有 inflate" % (name, info.compress_type))
            continue
        if info.file_size > MAX_MEX_BYTES * 2:
            problems.append("成员 %s 体积异常（%d 字节）" % (name, info.file_size))
            continue
        try:
            members[name] = z.read(name)
        except (zipfile.BadZipFile, OSError, RuntimeError) as ex:
            problems.append("读取成员 %s 失败: %s" % (name, ex))

    if "manifest.json" not in members:
        problems.append("缺少必需成员 manifest.json")
    if len(members) > MAX_MEMBERS:
        problems.append("成员数 %d 超过上限 %d" % (len(members), MAX_MEMBERS))
    return z, members, problems


def verify_signature(members, pinned_pubkey, problems):
    """返回 (status, detail)；status ∈ unsigned / valid / invalid / pinned。"""
    sig_name = SIGNATURE_PREFIX + "ed25519.json"
    if sig_name not in members:
        return "unsigned", None
    try:
        record = json.loads(members[sig_name].decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as ex:
        problems.append("%s 解析失败: %s" % (sig_name, ex))
        return "invalid", None

    if record.get("alg") != "ed25519":
        problems.append("不支持的签名算法 %r（只支持 ed25519）" % record.get("alg"))
        return "invalid", None

    subject = record.get("subject") or {}
    tree_hash = compute_tree_hash(member_tree(members))
    if subject.get("tree_sha256") != tree_hash:
        problems.append("签名 subject.tree_sha256 与包内容不符（包被篡改或重新打包）")
        return "invalid", None

    bundle = members.get("manifest.json")
    if bundle:
        try:
            m = json.loads(bundle.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            m = {}
        if subject.get("bundle_id") != m.get("id") or subject.get("version") != m.get("version"):
            problems.append("签名 subject 的 id/version 与 manifest.json 不符")
            return "invalid", None
    try:
        public = parse_hex_key(record.get("public_key", ""))
        signature = bytes.fromhex(record.get("signature", ""))
    except (ValueError, TypeError) as ex:
        problems.append("签名公钥/密文格式错误: %s" % ex)
        return "invalid", None

    message = signature_subject(subject.get("bundle_id", ""),
                                subject.get("version", ""), tree_hash)
    if not ed25519_verify(public, message, signature):
        problems.append("Ed25519 签名验证失败")
        return "invalid", None

    if pinned_pubkey:
        try:
            pinned = parse_hex_key(pinned_pubkey)
        except ValueError as ex:
            problems.append("--pubkey 格式错误: %s" % ex)
            return "invalid", None
        if pinned != public:
            problems.append("签名公钥与 --pubkey 钉住的值不一致")
            return "invalid", None
        return "pinned", record
    return "valid", record


def cmd_verify(args):
    z, members, problems = read_bundle(args.pkg)
    if z is None:
        print_problems(problems, "校验失败：")
        return 1

    warnings = []
    bundle = None
    if "manifest.json" in members:
        try:
            bundle = json.loads(members["manifest.json"].decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as ex:
            problems.append("manifest.json 解析失败: %s" % ex)

    if isinstance(bundle, dict):
        validate_manifest(bundle, problems, warnings)
        cross_check_assemblies(bundle, members, problems)
        cross_check_plugin_json(bundle, members, problems, warnings)
        cross_check_layouts(bundle, members, problems)

        icon = bundle.get("icon", "icon.png")
        if isinstance(icon, str) and icon in members:
            if not members[icon].startswith(PNG_MAGIC):
                problems.append("%s 不是 PNG" % icon)

    status, record = verify_signature(members, args.pubkey, problems)
    tree_hash = compute_tree_hash(member_tree(members))

    # 未签名包是合法分发方式，但要明确告知降权后果。
    if status == "unsigned":
        warnings.append("未签名：安装时 CAP_FS_WRITE 会被摘除，沙盒根强制为 "
                        "/sandbox/<plugin_id>")

    z.close()

    if problems or (args.strict and warnings):
        print_problems(problems + (["--strict: " + w for w in warnings] if args.strict else []))
        return 1

    for w in warnings:
        warn(w)

    print("OK: %s" % args.pkg)
    if isinstance(bundle, dict):
        plugins = bundle.get("plugins", [])
        print("  id      : %s" % bundle.get("id"))
        print("  version : %s" % bundle.get("version"))
        print("  plugins : %d" % len(plugins))
        for p in plugins:
            print("            - %s (%d tools)" % (p.get("id"), len(p.get("tools", []) or [])))
    print("  members : %d" % len(members))
    print("  tree    : %s" % tree_hash)
    print("  signature: %s" % ("valid (key_id=%s)" % (record or {}).get("key_id")
                               if status in ("valid", "pinned") else status))
    print("  sha256  : %s" % sha256_hex(open(args.pkg, "rb").read()))
    return 0


# =====================================================================
#  6. inspect
# =====================================================================
def cmd_inspect(args):
    z, members, problems = read_bundle(args.pkg)
    if z is None:
        print_problems(problems, "无法打开：")
        return 1

    print("bundle  : %s" % args.pkg)
    print("size    : %d bytes" % os.path.getsize(args.pkg))
    print("sha256  : %s" % sha256_hex(open(args.pkg, "rb").read()))
    print("tree    : %s" % compute_tree_hash(member_tree(members)))

    print("\nmembers (%d):" % len(members))
    for info in z.infolist():
        method = "STORE" if info.compress_type == zipfile.ZIP_STORED else "DEFLATE"
        print("  %-46s %8d bytes  crc=%08x  %s"
              % (info.filename, info.file_size, info.CRC, method))
    z.close()

    bundle = None
    if "manifest.json" in members:
        try:
            bundle = json.loads(members["manifest.json"].decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as ex:
            err("manifest.json 解析失败: %s" % ex)

    if isinstance(bundle, dict):
        print("\nmanifest:")
        for key in ("schema_version", "id", "name", "version", "publisher",
                    "license", "homepage", "description"):
            if key in bundle:
                print("  %-13s: %s" % (key, bundle.get(key)))
        if bundle.get("capabilities"):
            print("  %-13s: %s" % ("capabilities", ", ".join(bundle["capabilities"])))
        net = bundle.get("network") or {}
        print("  %-13s: %s" % ("allow_hosts", net.get("allow_hosts") or "(none)"))
        print("  %-13s: %s" % ("ui_layout", bundle.get("ui_layout")))

        for plugin in bundle.get("plugins", []):
            print("\nplugin %s  (%s)" % (plugin.get("id"), plugin.get("version")))
            print("  name        : %s" % plugin.get("name"))
            entry = plugin.get("entry") or {}
            print("  entry.kind  : %s" % entry.get("kind", "managed"))
            if entry.get("assembly"):
                print("  assembly    : %s" % entry["assembly"])
            if entry.get("init_symbol"):
                print("  init_symbol : %s" % entry["init_symbol"])
            print("  capabilities: %s"
                  % (", ".join(plugin.get("capabilities", [])) or "(none)"))
            net = plugin.get("network") or {}
            print("  allow_hosts : %s" % (net.get("allow_hosts") or "(none)"))
            print("  ui_layout   : %s" % plugin.get("ui_layout"))
            tools = plugin.get("tools", []) or []
            print("  tools (%d):" % len(tools))
            for tool in tools:
                print("    - %-16s %s" % (tool.get("name"), tool.get("description")))

    sig_problems = []
    status, record = verify_signature(members, None, sig_problems)
    print("\nsignature: %s" % status)
    if record:
        print("  key_id   : %s" % record.get("key_id"))
        print("  public   : %s" % record.get("public_key"))
    for p in sig_problems:
        print("  problem  : " + p)

    if problems:
        print("\nwarnings:")
        for p in problems:
            print("  - " + p)
    return 0


# =====================================================================
#  7. unpack / tree-hash
# =====================================================================
def cmd_unpack(args):
    z, members, problems = read_bundle(args.pkg)
    if z is None:
        print_problems(problems, "无法打开：")
        return 1
    z.close()

    if problems and not args.force:
        print_problems(problems, "包结构有问题，拒绝解包（--force 可强制）：")
        return 1

    os.makedirs(args.out, exist_ok=True)
    for name in sorted(members):
        bad = safe_relpath(name)
        if bad:
            continue
        dest = os.path.join(args.out, *name.split("/"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(members[name])
        print("  %s -> %s (%d bytes)" % (name, dest, len(members[name])))
    print("unpacked: %s" % args.out)
    return 0


def cmd_tree_hash(args):
    z, members, problems = read_bundle(args.pkg)
    if z is None:
        print_problems(problems, "无法打开：")
        return 1
    z.close()
    print(compute_tree_hash(member_tree(members)))
    return 0


# =====================================================================
#  8. selftest（签名 + 路径 + 清单元数据）
# =====================================================================
def cmd_selftest(args):
    failures = []
    total = [0]

    def check(cond, name):
        total[0] += 1
        print(("[SELFTEST] PASS  " if cond else "[SELFTEST] FAIL  ") + name)
        if not cond:
            failures.append(name)

    # --- RFC 8032 test vector 1 ---
    seed = bytes.fromhex(
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
    want_pub = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
    want_sig = ("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e0652249015"
                "55fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")
    public = ed25519_public_key(seed)
    check(public.hex() == want_pub, "Ed25519 public key matches RFC 8032 vector 1")
    _pub, sig = ed25519_sign(seed, b"")
    check(sig.hex() == want_sig, "Ed25519 signature matches RFC 8032 vector 1")
    check(ed25519_verify(public, b"", sig), "Ed25519 verify accepts good signature")
    check(not ed25519_verify(public, b"x", sig), "Ed25519 verify rejects changed message")

    # --- 路径 ---
    for bad in ("/etc/passwd", "a/../../b", "a\\b", "C:/x", "", "a//b", "./a"):
        check(safe_relpath(bad) is not None, "unsafe member path rejected: %r" % bad)
    for good in ("manifest.json", "plugins/sysinfo/plugin.json",
                 "plugins/sysinfo/ui/layout.json"):
        check(safe_relpath(good) is None, "safe member path accepted: %r" % good)

    # --- 能力 ---
    check(canonical_capability("sys.info") == "sys.info", "capability sys.info known")
    check(canonical_capability("sys_info") == "sys.info", "capability alias sys_info")
    check(canonical_capability("ipc") == "ipc", "capability ipc recognised (then rejected)")
    check(canonical_capability("root") is None, "unknown capability rejected")

    # --- 清单 ---
    bundle = {
        "schema_version": 1,
        "id": "demo.suite",
        "name": "Demo",
        "version": "1.0.0",
        "network": {"allow_hosts": []},
        "capabilities": [],
        "ui_layout": "ui/layout.json",
        "plugins": [{
            "id": "demo",
            "name": "Demo plugin",
            "version": "1.0.0",
            "entry": {"kind": "managed", "assembly": "demo.mex"},
            "capabilities": ["sys.info"],
            "tools": [{
                "name": "demo_info",
                "description": "demo",
                "inputSchema": {"type": "object"},
            }],
        }],
    }
    e, w = [], []
    check(validate_manifest(bundle, e, w) == "demo.suite" and not e,
          "valid bundle manifest accepted")

    bad = json.loads(json.dumps(bundle))
    bad["plugins"][0]["capabilities"] = ["ipc"]
    e, w = [], []
    validate_manifest(bad, e, w)
    check(any("零 IPC" in x for x in e), "manifest requesting ipc rejected")

    bad = json.loads(json.dumps(bundle))
    bad["plugins"][0]["tools"][0]["name"] = "Bad-Name"
    e, w = [], []
    validate_manifest(bad, e, w)
    check(any("tool" in x for x in e), "invalid tool name rejected")

    bad = json.loads(json.dumps(bundle))
    bad["plugins"] = []
    e, w = [], []
    validate_manifest(bad, e, w)
    check(any("非空数组" in x for x in e), "empty plugin list rejected")

    print("===== selftest done: %d passed, %d failed ====="
          % (total[0] - len(failures), len(failures)))
    return len(failures)


# =====================================================================
#  main
# =====================================================================
def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="nexbundle",
        description="NexOS .nexbundle 插件整合包工具（格式见 docs/NEXBUNDLE.md）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack", help="把 --src 目录打成 .nexbundle")
    p.add_argument("--src", required=True, help="包含 manifest.json / plugins/ 的目录")
    p.add_argument("--out", required=True, help="输出 .nexbundle 路径")
    p.add_argument("--sign", help="Ed25519 私钥种子（32 字节 hex）")
    p.add_argument("--key-id", help="签名 key_id（默认取公钥哈希前 16 位）")
    p.add_argument("--expect-id", help="要求 bundle id 必须等于该值")
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("verify", help="校验结构、清单、.mex 魔数与签名")
    p.add_argument("pkg")
    p.add_argument("--pubkey", help="钉住签名公钥（32 字节 hex）")
    p.add_argument("--strict", action="store_true", help="把警告也当作失败")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("inspect", help="打印成员、清单、插件与工具")
    p.add_argument("pkg")
    p.set_defaults(func=cmd_inspect)

    p = sub.add_parser("unpack", help="解包到目录")
    p.add_argument("pkg")
    p.add_argument("--out", required=True)
    p.add_argument("--force", action="store_true", help="结构有警告也解包")
    p.set_defaults(func=cmd_unpack)

    p = sub.add_parser("tree-hash", help="打印确定性树哈希")
    p.add_argument("pkg")
    p.set_defaults(func=cmd_tree_hash)

    p = sub.add_parser("selftest", help="Ed25519 / 清单 / 路径自测")
    p.set_defaults(func=cmd_selftest)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
