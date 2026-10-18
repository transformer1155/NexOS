#!/usr/bin/env python3
# =====================================================================
#  mcp_scan.py  -  NexOS MCP SDK 构建期工具发现
# ---------------------------------------------------------------------
#  MiniCLR 没有反射（程序集被 mex_pack.py 扁平化成数组，没有
#  ECMA-335 元数据可枚举），所以 [McpTool] 标记的方法**不能在运行时**
#  被发现。本工具在**构建期**做这件事，产出三样东西：
#
#    1. mcp_tools.json          —— 工具描述符（可直接并入 manifest.json）
#    2. <Class>.Dispatch.g.cs   —— Invoke 派发表（partial 实现，无 if-else 手写）
#    3. 人读的摘要              —— 打印到 stderr/stdout
#
#  扫描方式：纯语法扫描（正则 + 大括号配平），不依赖 Roslyn / dotnet。
#  对 SDK 的用法（[McpTool("name", Description="...")] 修饰静态方法）足够，
#  也不需要解析整个 C# 语言。
#
#  用法：
#    # 扫描一个目录，产出工具描述符
#    python tools/mcp_scan.py --src apps/Weather --out mcp_tools.json
#
#    # 同时生成派发表
#    python tools/mcp_scan.py --src apps/Weather --out mcp_tools.json \
#        --emit-dispatch apps/Weather/Weather.Dispatch.g.cs
#
#    # 只做校验（CI 用）：发现非法工具名/重复名/缺 description 时非 0 退出
#    python tools/mcp_scan.py --src apps/Weather --check
# =====================================================================
import argparse
import json
import os
import re
import sys

# 与 nexos_ai.h / docs/NEXPACK.md 保持一致
TOOL_NAME_RE = re.compile(r'^[a-z][a-z0-9_]{1,63}$')
MAX_TOOLS_PER_APP = 16          # NEXOS_AI_MAX_TOOLS

# ---------------------------------------------------------------------
#  C# 源码的最小语法扫描
# ---------------------------------------------------------------------
#  我们不实现完整解析器，只识别：
#     [McpTool("name", Description = "...", Schema = "...")]
#     public static string MethodName(string json_args)
#  以及可选的 [McpParam("x", Description="...")] 参数注解。
#
#  步骤：
#    1. 去注释（行注释 / 块注释 / 字符串内的 // 不算）
#    2. 找 [McpTool...] 特性，记录其位置与参数
#    3. 从特性结尾向后找方法签名（跳过其它特性）
#    4. 用大括号配平确定方法体范围（为生成派发表取方法名即可，不需要体）
# ---------------------------------------------------------------------

def strip_comments(src):
    """去掉 C# 注释，保留字符串字面量内容与换行（保持行号稳定）。"""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        # 行注释
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
            continue
        # 块注释
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                if src[i] == '\n':
                    out.append('\n')
                i += 1
            i += 2
            continue
        # 字符串（含逐字 @"..." 与普通 "..."）
        if c == '@' and i + 1 < n and src[i + 1] == '"':
            out.append(src[i]); out.append(src[i + 1]); i += 2
            while i < n:
                if src[i] == '"' and i + 1 < n and src[i + 1] == '"':
                    out.append('"'); out.append('"'); i += 2; continue
                out.append(src[i])
                if src[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == '"':
            out.append(c); i += 1
            while i < n:
                if src[i] == '\\' and i + 1 < n:
                    out.append(src[i]); out.append(src[i + 1]); i += 2; continue
                out.append(src[i])
                if src[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        # 字符字面量
        if c == "'":
            out.append(c); i += 1
            while i < n:
                if src[i] == '\\' and i + 1 < n:
                    out.append(src[i]); out.append(src[i + 1]); i += 2; continue
                out.append(src[i])
                if src[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def find_matching(src, start, open_ch, close_ch):
    """从 start（指向 open_ch）开始找配对的 close_ch，返回其索引；失败 -1。"""
    depth = 0
    i = start
    n = len(src)
    while i < n:
        c = src[i]
        if c == '"':
            # 跳过字符串
            i += 1
            while i < n:
                if src[i] == '\\' and i + 1 < n:
                    i += 2; continue
                if src[i] == '"':
                    i += 1; break
                i += 1
            continue
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


# [McpTool(...)] / [McpTool] 的匹配（允许 NexOS.Mcp.McpTool 全限定、允许 Attribute 后缀）
ATTR_RE = re.compile(
    r'\[\s*(?:NexOS\.Mcp\.)?McpTool(?:Attribute)?\s*(?:\(([^)]*)\))?\s*\]',
    re.S)

# 从特性参数里取 name（第一个位置参数）和 Description=/Schema= 命名参数
POS_NAME_RE   = re.compile(r'^\s*"([^"]*)"', re.S)
NAMED_RE      = re.compile(r'(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"', re.S)

# 方法签名：可选修饰符 + 返回类型 + 方法名 + 参数表
METHOD_RE = re.compile(
    r'(?:public|private|internal|protected|static|virtual|override|sealed|async|new|extern|partial|\s)*'
    r'([A-Za-z_][\w<>\[\],\.\?]*)\s+'      # 返回类型
    r'([A-Za-z_]\w*)\s*'                    # 方法名
    r'\(([^)]*)\)\s*',                      # 参数表
    re.S)


CLASS_DECL_RE = re.compile(
    r'\b(?:class|struct)\s+([A-Za-z_]\w*)', re.S)

NAMESPACE_RE = re.compile(r'\bnamespace\s+([A-Za-z_][\w\.]*)', re.S)


def detect_namespace(path):
    """从 .cs 文件里读第一个 namespace 名；没有则返回 None。"""
    try:
        with open(path, 'r', encoding='utf-8-sig', errors='replace') as f:
            src = strip_comments(f.read())
    except Exception:
        return None
    m = NAMESPACE_RE.search(src)
    return m.group(1) if m else None


def _owner_class(src, attr_pos):
    """找出位于 attr_pos 的最内层 class/struct 名字。

    做法：在 [0, attr_pos) 内找所有 class/struct 声明，取其中**最后一个
    未闭合**的（即大括号尚未收尾的那个）。这正是特性所在的方法的宿主类。
    """
    matches = list(CLASS_DECL_RE.finditer(src, 0, attr_pos))
    if not matches:
        return None
    # 从后往前找第一个其 { ... } 区间覆盖 attr_pos 的类
    for m in reversed(matches):
        brace = src.find('{', m.end())
        if brace < 0 or brace > attr_pos:
            continue
        close = find_matching(src, brace, '{', '}')
        if close < 0 or close > attr_pos:
            return m.group(1)
    return matches[-1].group(1)


def camel_to_snake(name):
    """GetWeather -> get_weather；GetHTTPStatus -> get_http_status。"""
    out = []
    for i, ch in enumerate(name):
        if ch.isupper():
            if i > 0 and (not name[i - 1].isupper() or
                          (i + 1 < len(name) and name[i + 1].islower())):
                out.append('_')
            out.append(ch.lower())
        else:
            out.append(ch)
    s = ''.join(out)
    s = re.sub(r'_+', '_', s).strip('_')
    return s


def parse_attr_args(argtext):
    """解析 [McpTool(...)] 的参数，返回 (name, description, schema)。"""
    name = None
    desc = None
    schema = None
    if not argtext:
        return None, None, None

    # 位置参数：第一个字符串
    m = POS_NAME_RE.match(argtext)
    if m:
        name = m.group(1)

    # 命名参数
    for nm, val in NAMED_RE.findall(argtext):
        val = val.replace('\\"', '"').replace('\\\\', '\\')
        if nm == 'Description':
            desc = val
        elif nm == 'Schema':
            schema = val
    return name, desc, schema


def scan_file(path):
    """扫描单个 .cs 文件，返回工具列表与问题列表。"""
    with open(path, 'r', encoding='utf-8-sig', errors='replace') as f:
        raw = f.read()
    src = strip_comments(raw)
    src = src.replace('\r\n', '\n').replace('\r', '\n')

    tools = []
    problems = []

    for am in ATTR_RE.finditer(src):
        argtext = am.group(1)
        attr_name, attr_desc, attr_schema = parse_attr_args(argtext)

        # 从特性结束后找方法签名
        rest = src[am.end():]
        mm = METHOD_RE.match(rest)
        if not mm:
            problems.append("%s: [McpTool] 之后未找到方法签名" % path)
            continue
        ret_type, method_name, params = mm.group(1), mm.group(2), mm.group(3)

        # 确定方法体范围（仅用于确认它是带体的方法）
        brace = rest.find('{', mm.end())
        semi = rest.find(';', mm.end())
        if brace < 0 or (0 <= semi < brace):
            # 表达式体 => 或抽象方法
            if semi >= 0 and rest[max(0, semi - 1)] == '>':
                pass  # 表达式体方法，可接受
            else:
                problems.append("%s: [McpTool] 方法 %s 没有方法体"
                                % (path, method_name))
                continue

        name = attr_name if attr_name else camel_to_snake(method_name)

        # 推断返回类型是否合法（工具必须返回 string）
        rt = ret_type.strip()
        if rt not in ('string', 'String'):
            problems.append("%s: 工具方法 %s 返回 %r，必须是 string"
                            % (path, method_name, rt))

        # 参数表检查：SDK 约定工具方法接收单个 string json_args
        plist = [p.strip() for p in params.split(',') if p.strip()]
        if len(plist) > 1:
            problems.append("%s: 工具方法 %s 有 %d 个参数；MCP 工具只接收"
                            "单个 string json_args（JSON 里自己解析）"
                            % (path, method_name, len(plist)))

        tools.append({
            "tool_name": name,
            "method": method_name,
            # 方法所在类（派发表里用 ClassName.Method 限定，避免多个
            # McpServerBase 子类的方法名冲突）。
            "owner_class": _owner_class(src, am.start()),
            "description": attr_desc or "",
            "schema_literal": attr_schema,
            "file": path,
        })

    return tools, problems


def scan_tree(src_dir):
    """递归扫描目录下所有 .cs。"""
    all_tools = []
    problems = []
    for root, _dirs, files in os.walk(src_dir):
        for fn in sorted(files):
            if not fn.endswith('.cs'):
                continue
            # 跳过生成的派发表，避免自扫描
            if fn.endswith('.Dispatch.g.cs') or fn.endswith('.g.cs'):
                continue
            path = os.path.join(root, fn)
            t, p = scan_file(path)
            all_tools.extend(t)
            problems.extend(p)
    return all_tools, problems


def validate_tools(tools):
    """校验工具描述符。返回问题列表。"""
    problems = []
    if len(tools) > MAX_TOOLS_PER_APP:
        problems.append("工具数 %d 超过内核上限 NEXOS_AI_MAX_TOOLS=%d"
                        % (len(tools), MAX_TOOLS_PER_APP))
    seen = {}
    for t in tools:
        n = t["tool_name"]
        if not TOOL_NAME_RE.match(n):
            problems.append("工具名 %r 不合法（要求 %s）" % (n, TOOL_NAME_RE.pattern))
        if n in seen:
            problems.append("工具名 %r 重复（%s 与 %s）"
                            % (n, seen[n], t["method"]))
        seen[n] = t["method"]
        if not t["description"]:
            problems.append("工具 %r 缺少 Description —— LLM 无法判断何时调用" % n)
    return problems


# ---------------------------------------------------------------------
#  派发表生成
# ---------------------------------------------------------------------
DISPATCH_HEADER = """// =====================================================================
//  {cls}.Dispatch.g.cs  -  MCP 工具派发表（自动生成，请勿手改）
// ---------------------------------------------------------------------
//  由 tools/mcp_scan.py 生成。作用：把 MCP 总线传进来的 tool_name 映射到
//  带 [McpTool] 特性的静态方法，**不需要运行时反射**（MiniCLR 没有反射）。
//
//  这是 {cls} 的 partial 实现，提供 McpServerBase.Invoke 的覆写。
//  因此用户类必须声明为 `partial`：
//      public sealed partial class {cls} : McpServerBase
//
//  重新生成：
//    python tools/mcp_scan.py --src {src} --emit-dispatch {out}
// =====================================================================
// <auto-generated />
namespace {ns}
{
    public sealed partial class {cls}
    {
        // 返回 JSON 字符串。未知工具返回 code=5。
        public override string Invoke(string tool_name, string json_args)
        {
"""

DISPATCH_BODY_EMPTY = """            return "{\\"code\\":5,\\"error\\":\\"unknown tool\\"}";
        }
    }
}
"""


def _fill(template, **kw):
    """安全模板填充：只替换已知占位符，不动 C# 里的字面大括号。

    不能用 str.format —— C# 代码里满是 {"code":...} 之类的 JSON 字面量，
    format 会把它们当成格式占位符并抛 KeyError。
    """
    out = template
    for k, v in kw.items():
        out = out.replace('{' + k + '}', str(v))
    return out


def emit_dispatch(tools, cls, ns, sealed_kw, out_path, src_dir,
                  base_cls="McpServerBase"):
    """生成 `partial class <cls>` 的 Invoke 覆写。

    tools 必须都属于同一个宿主类（调用方负责分组）。
    """
    lines = []
    lines.append(_fill(DISPATCH_HEADER, cls=cls, src=src_dir,
                       out=out_path, ns=ns, sealed=sealed_kw))
    if not tools:
        lines.append(DISPATCH_BODY_EMPTY)
    else:
        first = True
        for t in tools:
            kw = "if" if first else "else if"
            first = False
            # 同类的静态方法直接按名字调用即可（部分方法私有也能调到）。
            lines.append('            %s (tool_name == "%s")\n'
                         % (kw, t["tool_name"]))
            lines.append('                return %s(json_args);\n' % t["method"])
        lines.append('            return "{\\"code\\":5,\\"error\\":\\"unknown tool\\"}";\n')
        lines.append("        }\n    }\n}\n")

    text = ''.join(lines)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return text


# ---------------------------------------------------------------------
#  main
# ---------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="mcp_scan",
        description="扫描 [McpTool] 生成 NexOS MCP 工具描述符与派发表")
    ap.add_argument("--src", required=True, help="要扫描的目录或单个 .cs 文件")
    ap.add_argument("--out", help="工具描述符输出 JSON（可并入 manifest.json）")
    ap.add_argument("--emit-dispatch", help="生成派发表的输出 .cs 路径")
    ap.add_argument("--class", dest="cls", default="McpTools",
                    help="派发表的类名（默认 McpTools）")
    ap.add_argument("--namespace", dest="ns", default="App",
                    help="派发表的命名空间（默认 App）")
    ap.add_argument("--sealed", action="store_true",
                    help="生成 sealed class")
    ap.add_argument("--check", action="store_true",
                    help="仅校验，有问题时非 0 退出（CI 用）")
    ap.add_argument("--force", action="store_true",
                    help="发现问题也继续生成产物")
    args = ap.parse_args(argv)

    if not os.path.exists(args.src):
        print("error: 源码路径不存在: %s" % args.src, file=sys.stderr)
        return 2

    if os.path.isdir(args.src):
        tools, problems = scan_tree(args.src)
    else:
        tools, problems = scan_file(args.src)

    problems += validate_tools(tools)

    # 人读摘要
    print("scanned: %s" % args.src)
    print("tools  : %d" % len(tools))
    for t in tools:
        print("  - %-24s %-28s %s"
              % (t["tool_name"], t["method"],
                 t["description"] or "(no description)"))

    if problems:
        print("\nproblems:", file=sys.stderr)
        for p in problems:
            print("  - " + p, file=sys.stderr)
        if args.check or not args.force:
            # 有实质问题时不产出（除非 --force）
            if args.check:
                return 1
            if any("不合法" in p or "重复" in p or "没有方法体" in p or
                   "必须是 string" in p for p in problems):
                print("拒绝生成产物（用 --force 可覆盖）", file=sys.stderr)
                return 1

    # 产出描述符
    if args.out:
        descs = []
        for t in tools:
            d = {
                "tool_name": t["tool_name"],
                "description": t["description"] or t["method"],
                "parameters_schema": {
                    "type": "object",
                    "properties": {
                        # MCP 工具统一接收自由 JSON；具体字段由实现解析。
                        "json_args": {
                            "type": "string",
                            "description": "Tool arguments as a JSON string."
                        }
                    }
                }
            }
            # 若开发者在特性里给了 Schema 字面量，优先用它
            if t.get("schema_literal"):
                try:
                    d["parameters_schema"] = json.loads(t["schema_literal"])
                except Exception:
                    print("warning: %s 的 Schema 不是合法 JSON，已忽略"
                          % t["tool_name"], file=sys.stderr)
            descs.append(d)

        payload = {"mcp_server": descs}
        os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2)
            f.write("\n")
        print("\nwrote descriptors: %s" % args.out)

    # 产出派发表
    if args.emit_dispatch:
        # 派发表是 <cls> 的 partial 实现，所以这些工具必须都属于同一个类。
        owners = {}
        for t in tools:
            owners.setdefault(t.get("owner_class"), []).append(t)

        if len(owners) > 1:
            print("\nwarning: 本次扫描发现 %d 个不同的宿主类：%s"
                  % (len(owners), sorted(str(k) for k in owners)),
                  file=sys.stderr)
            print("         --emit-dispatch 每个文件只能生成一个类的 Invoke；"
                  "已选中工具最多的那个类。请用 --class 指定，或对每个应用"
                  "单独调用一次。", file=sys.stderr)

        # 选工具最多的类（稳定：同数量时按类名排序）
        best = sorted(owners.items(),
                      key=lambda kv: (-len(kv[1]), str(kv[0])))[0]
        owner_cls, owner_tools = best
        cls_name = args.cls if args.cls != "McpTools" else (owner_cls or "McpTools")

        # 命名空间：从源文件里探测 `namespace X`
        ns_name = args.ns
        if args.ns == "App" and owner_tools:
            f0 = owner_tools[0].get("file")
            if f0:
                ns_name = detect_namespace(f0) or args.ns

        emit_dispatch(owner_tools, cls_name, ns_name,
                      "sealed " if args.sealed else "",
                      args.emit_dispatch, args.src)
        print("wrote dispatch   : %s  (class %s, namespace %s, %d tools)"
              % (args.emit_dispatch, cls_name, ns_name, len(owner_tools)))

    if args.check and problems:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
