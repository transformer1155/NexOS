#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""行号寻址协议 (Line-Number Addressing Protocol).

AI 生成代码时, 在每一行前面加 `[NNN]` 编号 (3~5 位宽度, 10 进制, 从 1 开始,
不足补零)。例如:

    [001] import sys
    [002] def add(a, b):
    [003]     return a + b
    [004] print(add(1, 2))

编码 (encode):  把纯 Python 源码变成带编号的协议文本 (交给模型去"续写/修改")。
解码 (decode):  把协议文本还原成可执行的纯源码 (去掉前缀), 同时记住
                "逻辑行号 -> 物理行号" 的映射, 供报错定位与单行修补使用。
修补 (patch):   给定一处"逻辑行号 + 新行内容", 仅替换那一行, 其它行不动,
                并返回新的协议文本 —— 这样 Debug 时回传给模型的 diff 极小。

设计要点:
- 前缀正则允许行首任意空白后跟 `[digits]`, 因此模型输出的缩进也会保留。
- 空行 / 注释行同样编号, 编号连续, 保证"行号"与肉眼一致。
- `decode` 会严格校验编号连续性 (允许缺失但会警告), 方便发现模型"跳号"。
"""
import re
from dataclasses import dataclass, field

# 协议前缀: 行首任意空白 + [编号]. 编号默认 3 位, 但 1~5 位都接受。
_PREFIX_RE = re.compile(r"^(?P<pad>\s*)\[(?P<num>\d{1,5})\]\s?(?P<body>.*)$")
_PREFIX_WIDTH = 3  # 默认编号宽度


@dataclass
class LogicalLine:
    """协议中的一行。"""
    num: int          # 逻辑行号 (从 1 开始)
    body: str         # 去掉前缀后的正文 (含缩进)
    physical: int     # 在"纯源码"中的行号 (从 1 开始)


@dataclass
class DecodedScript:
    """decode() 的结果。"""
    source: str                                 # 可执行的纯 Python 源码
    lines: list[LogicalLine] = field(default_factory=list)
    width: int = _PREFIX_WIDTH
    warnings: list[str] = field(default_factory=list)

    def line_by_num(self, num: int) -> "LogicalLine | None":
        for ln in self.lines:
            if ln.num == num:
                return ln
        return None

    def physical_of(self, num: int) -> "int | None":
        """逻辑行号 -> 纯源码物理行号 (给 Python traceback 用)。"""
        ln = self.line_by_num(num)
        return ln.physical if ln else None


def encode(source: str, width: int = _PREFIX_WIDTH) -> str:
    """把纯源码编码成带 `[NNN]` 前缀的协议文本。

    source 以 '\n' 分隔 (允许末尾无换行)。空行也保留并编号。
    """
    if width < 1:
        width = 1
    rows = source.split("\n")
    out = []
    for i, body in enumerate(rows, start=1):
        # 注意: 编码时只输出前缀 + 正文, 不复制原缩进逻辑 —— 缩进由 body 自带。
        out.append("[%0*d] %s" % (width, i, body))
    return "\n".join(out)


def decode(text: str, expected_width: int = _PREFIX_WIDTH) -> DecodedScript:
    """把协议文本解码成纯源码, 并记录逻辑行号映射。

    不认识的格式 (没有前缀的行) 会被原样保留并打印 warning, 但其物理行号仍可
    用于报错定位 (只是无法做"按逻辑行号"精确修补)。
    """
    rows = text.split("\n")
    src_rows: list[str] = []
    logical: list[LogicalLine] = []
    warnings: list[str] = []
    last_num = 0
    width = expected_width
    for phys, raw in enumerate(rows, start=1):
        m = _PREFIX_RE.match(raw)
        if not m:
            warnings.append("第 %d 行为非协议格式, 原样保留: %r" % (phys, raw[:40]))
            src_rows.append(raw)
            continue
        num = int(m.group("num"))
        body = m.group("body")
        width = max(width, len(m.group("num")))
        if num != last_num + 1:
            warnings.append("逻辑行号不连续: 期望 %d, 实际 %d (物理行 %d)"
                            % (last_num + 1, num, phys))
        last_num = num
        logical.append(LogicalLine(num=num, body=body, physical=phys))
        src_rows.append(body)
    return DecodedScript(
        source="\n".join(src_rows),
        lines=logical,
        width=width,
        warnings=warnings,
    )


def patch(text: str, num: int, new_body: str,
          new_prefix: "str | None" = None) -> str:
    """单行修补: 把逻辑行号 num 的正文替换为 new_body, 返回新协议文本。

    - new_body 是"去掉前缀后"的正文 (应保留自身缩进)。
    - new_prefix 可选, 用于让模型直接给出"完整行"(含 `[NNN]`) 时自动识别。
    若找不到该行, 抛出 KeyError。
    """
    rows = text.split("\n")
    target = None
    for phys, raw in enumerate(rows):
        m = _PREFIX_RE.match(raw)
        if m and int(m.group("num")) == num:
            target = phys
            break
    if target is None:
        raise KeyError("协议文本中找不到逻辑行号 %d" % num)

    if new_prefix is None:
        # 沿用原行前缀宽度
        m = _PREFIX_RE.match(rows[target])
        width = len(m.group("num")) if m else _PREFIX_WIDTH
        replacement = "[%0*d] %s" % (width, num, new_body.rstrip("\n"))
    else:
        replacement = new_prefix.rstrip("\n")
    rows[target] = replacement
    return "\n".join(rows)


def parse_traceback_error_line(tb_text: str) -> "int | None":
    """从 Python traceback 文本里提取出错行 (物理行号)。

    支持两种格式:
      - 运行时异常: `File "x.py", line 12, in <fn>`  (取最后一个 = 最内层用户帧)
      - 语法错误:   `File "x.py", line 4`            (无 ", in", 只有一处)
    返回纯源码里的物理行号, 找不到返回 None。
    """
    last = None
    for line in tb_text.splitlines():
        # 同时匹配 "line N, in fn" 与 "line N" (SyntaxError)
        m = re.search(r'File "[^"]*", line (\d+)', line)
        if m:
            last = int(m.group(1))
    return last


def map_physical_to_logical(decoded: DecodedScript, physical: int) -> "int | None":
    """把纯源码物理行号映射回协议逻辑行号 (用于把报错准确指给模型)。"""
    for ln in decoded.lines:
        if ln.physical == physical:
            return ln.num
    return None


def strip_prefixes(text: str) -> str:
    """仅去掉前缀, 不做映射 (快速执行用)。decode 更完整。"""
    out = []
    for raw in text.split("\n"):
        m = _PREFIX_RE.match(raw)
        out.append(m.group("body") if m else raw)
    return "\n".join(out)
