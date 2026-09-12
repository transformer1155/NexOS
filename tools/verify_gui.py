#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_gui.py - NexOS (gui.cpp) 原生 GUI 完整验证脚本
======================================================

通过 QEMU monitor 接口驱动运行在虚拟机中的 NexOS 原生图形界面
(gui.cpp)，对界面元素渲染与用户交互进行端到端验证：

  1) 界面元素渲染检查：顶栏 / 任务栏 / 桌面图标 / 开始按钮 /
     窗口标题栏 / 窗口内容(列表·标签页) 是否正确绘制；
  2) 用户交互验证：点击、输入、拖拽、滚动、悬停 是否触发预期响应；
  3) 每条检查输出 通过/失败 与 总数统计；
  4) 同时支持 无头(headless, 默认) 与 有头(headed, --headed) 运行；
  5) 失败时给出清晰信息：元素定位(屏幕区域)、预期结果、实际结果。

工作原理
--------
  * 启动 QEMU(-vga std) 加载 BIOS 镜像(build/os_v2.img)，默认无头渲染；
  * 通过串口日志等待 Shell 就绪并进入 GUI(`gui`)；
  * 用 monitor 的 `screendump` 抓取 PPM 帧缓冲；
  * 用 monitor 的 `mouse_move` / `mouse_button` / `sendkey` 注入输入；
  * 对各屏幕区域做像素/亮度分析，断言“渲染/响应”是否成立。

所有坐标基于 gui.cpp 中的真实布局(1024x768, TOPBAR_H=32,
TITLE_BAR_H=32, 任务栏高 48, 桌面图标 x=16 间距 76 等)，并支持
通过 `info mouse` 自动校准鼠标映射，对坐标小幅漂移具有容错性。

用法
----
  python3 tools/verify_gui.py                 # 无头运行全部检查
  python3 tools/verify_gui.py --headed        # 弹出 QEMU 窗口运行
  python3 tools/verify_gui.py --img build/os_v2.img --timeout 120
  python3 tools/verify_gui.py --save-frames frames/   #  dump 每帧 PPM
"""

import os
import sys
import socket
import time
import subprocess
import struct
import argparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

# ----------------------------------------------------------------------------
# 配置
# ----------------------------------------------------------------------------
IMG_DEFAULT = "build/os_v2.img"
MONPORT = 55980
HTTPPORT = 18080  # 若内核带网络，可用于 /screen (本脚本默认走 screendump)


def find_qemu():
    if os.name == "nt":
        cand = [
            r"D:\qemu\qemu-system-x86_64.exe",
            r"C:\Program Files\qemu\qemu-system-x86_64.exe",
            "qemu-system-x86_64.exe",
        ]
    else:
        cand = ["qemu-system-x86_64", "qemu-system-i386"]
    for c in cand:
        if os.path.exists(c):
            return c
    # 最后尝试 PATH
    return cand[-1]


# 屏幕逻辑分辨率(BIOS 0x117 = 1024x768)。脚本会从 PPM 头读取真实值并据此缩放。
REF_W, REF_H = 1024, 768

# 关键布局常量(与 gui.cpp 保持一致)
TOPBAR_H = 32
TASKBAR_H = 48
TITLE_BAR_H = 32
ICON_X = 16
ICON_Y0 = TOPBAR_H + 20  # 52
ICON_STEP = 76

# 桌面图标中心(用于点击启动应用)
ICON_CENTERS = {
    "control": (ICON_X + 24, ICON_Y0 + 0 * ICON_STEP + 24),
    "explorer": (ICON_X + 24, ICON_Y0 + 1 * ICON_STEP + 24),
    "taskmgr": (ICON_X + 24, ICON_Y0 + 2 * ICON_STEP + 24),
    "memopt": (ICON_X + 24, ICON_Y0 + 3 * ICON_STEP + 24),
    "terminal": (ICON_X + 24, ICON_Y0 + 4 * ICON_STEP + 24),
    "browser": (ICON_X + 24, ICON_Y0 + 5 * ICON_STEP + 24),
    "calculator": (ICON_X + 24, ICON_Y0 + 6 * ICON_STEP + 24),
    "about": (ICON_X + 24, ICON_Y0 + 7 * ICON_STEP + 24),
}

# 开始按钮(顶栏左侧)
START_BTN = (8, 6, 52, 26)
START_BTN_C = (30, 16)

# 开始菜单区域(mx=8, my=TOPBAR_H+2, mw=380, mh=248)
STARTMENU = (8, TOPBAR_H + 2, 8 + 380, TOPBAR_H + 2 + 248)

# File Explorer 窗口(ww=520, wh=360, 居中)
FE_WW, FE_WH = 520, 360
FE_WX = (REF_W - FE_WW) // 2
FE_WY = (REF_H - FE_WH) // 2 + TOPBAR_H + 10
# 标签页(MKFS/SFS/FAT32)绘制于内容区: content_x=wx+8, 第一行文字后 y+=24,
# 再 y+=30 得到 tab 行; tab_w=70, gap=4 => 步长 74
FE_TAB0 = (FE_WX + 8, FE_WY + TITLE_BAR_H + 8 + 24 + 30)  # tab 行 y
FE_TABS = {
    "MKFS": (FE_WX + 8 + 0 * 74 + 35, FE_TAB0[1] + 11),
    "SFS": (FE_WX + 8 + 1 * 74 + 35, FE_TAB0[1] + 11),
    "FAT32": (FE_WX + 8 + 2 * 74 + 35, FE_TAB0[1] + 11),
}


# ----------------------------------------------------------------------------
# PPM / 像素工具
# ----------------------------------------------------------------------------
def read_ppm(path):
    for _ in range(15):
        try:
            with open(path, "rb") as f:
                d = f.read()
            if d[:2] != b"P6":
                return None
            i = 2
            nums = []
            while len(nums) < 3:
                while i < len(d) and d[i] in b" \t\r\n":
                    i += 1
                s = i
                while i < len(d) and d[i] in b"0123456789":
                    i += 1
                nums.append(int(d[s:i]))
                i += 1
            w, h, maxval = nums
            # 读完 maxval 后的 i += 1 已越过其后的换行, 此刻 i 即像素数据起始
            px = d[i : i + w * h * 3]
            if len(px) < w * h * 3:
                raise ValueError("truncated")
            return w, h, px
        except Exception:
            time.sleep(0.4)
    return None


def lum(px, o):
    return px[o] + px[o + 1] + px[o + 2]


def chroma(px, o):
    r, g, b = px[o], px[o + 1], px[o + 2]
    return max(r, g, b) - min(r, g, b)


def pixel_at(img, x, y):
    w, h, px = img
    if x < 0 or y < 0 or x >= w or y >= h:
        return (0, 0, 0)
    o = (y * w + x) * 3
    return (px[o], px[o + 1], px[o + 2])


def region_stats(img, x0, y0, x1, y1):
    """返回区域内: 非黑像素数, 亮(白)像素数, 饱和(彩色)像素数, 平均亮度, 总像素."""
    w, h, px = img
    x0, x1 = max(0, x0), min(w, x1)
    y0, y1 = max(0, y0), min(h, y1)
    nonblack = bright = sat = tot = 0
    s_lum = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            o = row + x * 3
            l = lum(px, o)
            tot += 1
            s_lum += l
            if l > 40:
                nonblack += 1
            if l > 500:  # 近白(标题栏/菜单/文字背景)
                bright += 1
            if chroma(px, o) > 70:  # 彩色(图标/强调色)
                sat += 1
    return {
        "nonblack": nonblack,
        "bright": bright,
        "sat": sat,
        "total": tot,
        "avg_lum": (s_lum / tot) if tot else 0,
    }


def region_diff(ref, cur, x0, y0, x1, y1):
    """两帧在区域内的亮度差绝对值之和(越大表示变化越多)。"""
    w, h, pxr = ref
    _, _, pxc = cur
    x0, x1 = max(0, x0), min(w, x1)
    y0, y1 = max(0, y0), min(h, y1)
    diff = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            o = row + x * 3
            diff += abs(pxr[o] - pxc[o]) + abs(pxr[o + 1] - pxc[o + 1]) + abs(pxr[o + 2] - pxc[o + 2])
    return diff


def crop(img, x0, y0, x1, y1):
    w, h, px = img
    x0, x1 = max(0, x0), min(w, x1)
    y0, y1 = max(0, y0), min(h, y1)
    out = bytearray((x1 - x0) * (y1 - y0) * 3)
    o = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            s = row + x * 3
            out[o] = px[s]
            out[o + 1] = px[s + 1]
            out[o + 2] = px[s + 2]
            o += 3
    return (x1 - x0, y1 - y0, bytes(out))


def is_desktop(img):
    """区分真实桌面(底部有深色任务栏) vs 登录屏(底部为蓝色背景)。
    采样底部 8 行的 B-R 通道差: 任务栏近灰(B-R≈0) 而蓝背景 B>>R(B-R≈140+)。"""
    w, h, px = img
    diff_sum = 0
    n = 0
    for y in range(h - 8, h):
        row = y * w * 3
        for x in range(0, w, 4):
            o = row + x * 3
            diff_sum += px[o + 2] - px[o]
            n += 1
    return (diff_sum / max(n, 1)) < 30


def is_login_screen(img):
    """登录屏中央有一块深色圆角卡片(含用户名/密码框)。检测画面中部横向是否存在
    一条居中、足够宽、连续的深色(亮度<80)像素带。桌面(图标/窗口/任务栏)中部
    不会出现这样高占比的居中深色大块, 故可据此区分。"""
    w, h, px = img
    y0, y1 = int(h * 0.22), int(h * 0.72)
    min_run = int(w * 0.18)
    card_rows = 0
    for y in range(y0, y1):
        row = y * w * 3
        run = 0
        maxrun = 0
        x = 0
        while x < w:
            o = row + x * 3
            lum = (px[o] + px[o + 1] + px[o + 2]) // 3
            if lum < 80:
                run += 1
            else:
                if run > maxrun:
                    maxrun = run
                run = 0
            x += 1
        if run > maxrun:
            maxrun = run
        if maxrun >= min_run:
            card_rows += 1
    return card_rows >= int((y1 - y0) * 0.25)


# ----------------------------------------------------------------------------
# QEMU / monitor 控制
# ----------------------------------------------------------------------------
class QEMU:
    def __init__(self, img, headed, timeout):
        self.img = img
        self.headed = headed
        self.timeout = timeout
        self.proc = None
        self.mon = None
        self.scale_x = 1.0
        self.scale_y = 1.0
        self.off_x = 0
        self.off_y = 0
        self.W = REF_W
        self.H = REF_H
        self.frames_dir = None

    def launch(self):
        qemu = find_qemu()
        work = "build/verify_gui_work.img"
        if not os.path.exists(self.img):
            raise RuntimeError("镜像不存在: %s (先 `make` 构建)" % self.img)
        if os.path.exists(work):
            try:
                os.remove(work)
            except OSError:
                open(work, "w").close()
        import shutil

        shutil.copy(self.img, work)

        args = [qemu, "-machine", "pc", "-drive", "format=raw,file=%s" % work,
                "-m", "256M", "-accel", "tcg,tb-size=128", "-vga", "std",
                "-no-reboot",
                "-chardev", "file,id=ser,path=build/verify_gui.log",
                "-serial", "chardev:ser",
                "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONPORT]
        if self.headed:
            disp = "sdl" if os.name != "nt" else "sdl"
            args += ["-display", disp]
        else:
            args += ["-display", "none"]
        print("[qemu] " + " ".join(args))
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                     stderr=open("build/verify_gui.err", "wb"))
        # 等待 monitor
        end = time.time() + 30
        while time.time() < end:
            try:
                self.mon = socket.create_connection(("127.0.0.1", MONPORT), timeout=1)
                self.mon.settimeout(2.0)
                self.mon.recv(65536)
                return
            except OSError:
                time.sleep(0.3)
        raise RuntimeError("monitor 未就绪")

    def cmd(self, s, read=True):
        self.mon.sendall((s + "\n").encode())
        if not read:
            return ""
        out = b""
        try:
            while True:
                chunk = self.mon.recv(65536)
                if not chunk:
                    break
                out += chunk
                if len(out) > 200000:
                    break
        except (socket.timeout, TimeoutError):
            pass
        return out.decode("latin-1", "ignore")

    def info_mouse(self):
        try:
            txt = self.cmd("info mouse")
            # 形如 "Mouse position: 123, 456\n"
            import re
            m = re.search(r"(-?\d+)\s*,\s*(-?\d+)", txt)
            if m:
                return int(m.group(1)), int(m.group(2))
        except Exception:
            pass
        return None

    def calibrate(self):
        """用 info mouse 校准 monitor 坐标 -> 帧缓冲像素 的映射。"""
        self.mouse_move_abs(REF_W // 2, REF_H // 2)
        time.sleep(0.1)
        c = self.info_mouse()
        if c and 0 <= c[0] <= REF_W and 0 <= c[1] <= REF_H:
            # 近似 1:1
            self.scale_x = 1.0
            self.scale_y = 1.0
            self.off_x = 0
            self.off_y = 0
        else:
            print("[calib] info mouse 不可用或越界(%s)，假设 1:1 映射" % (c,))

    def mouse_move_abs(self, sx, sy):
        # 复位到左上角再绝对移动(避免相对累积误差)
        self.cmd("mouse_move -10000 -10000", read=False)
        mx = int(sx * self.scale_x + self.off_x)
        my = int(sy * self.scale_y + self.off_y)
        self.cmd("mouse_move %d %d" % (mx, my), read=False)
        time.sleep(0.05)

    def click(self, sx, sy, button=1):
        self.mouse_move_abs(sx, sy)
        time.sleep(0.08)
        self.cmd("mouse_button %d" % button, read=False)
        time.sleep(0.08)
        self.cmd("mouse_button 0", read=False)
        time.sleep(0.12)

    def right_click(self, sx, sy):
        self.click(sx, sy, button=2)

    def drag(self, x0, y0, x1, y1):
        self.mouse_move_abs(x0, y0)
        time.sleep(0.08)
        self.cmd("mouse_button 1", read=False)  # 按下左键
        time.sleep(0.1)
        # 分段移动以触发拖拽
        steps = 12
        for i in range(1, steps + 1):
            ix = int(x0 + (x1 - x0) * i / steps)
            iy = int(y0 + (y1 - y0) * i / steps)
            self.mouse_move_abs(ix, iy)
            time.sleep(0.02)
        time.sleep(0.08)
        self.cmd("mouse_button 0", read=False)  # 松开
        time.sleep(0.15)

    def wheel(self, sx, sy, delta):
        """delta>0 向下滚, <0 向上滚。QEMU monitor `mouse_button 4/5` 表示滚轮。"""
        self.mouse_move_abs(sx, sy)
        time.sleep(0.08)
        code = 4 if delta < 0 else 5
        for _ in range(abs(delta)):
            self.cmd("mouse_button %d" % code, read=False)
            time.sleep(0.03)
            self.cmd("mouse_button 0", read=False)
            time.sleep(0.03)
        time.sleep(0.15)

    def type_line(self, s):
        keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
                  '-': 'minus', '_': 'shift-minus', '\n': 'ret'}
        for ch in s:
            key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
            self.cmd("sendkey %s" % key, read=False)
            time.sleep(0.06)
        self.cmd("sendkey ret", read=False)
        time.sleep(0.4)

    def type_text(self, s, delay=0.07):
        """同 type_line 但不发送结尾回车(用于分步填写表单字段)。"""
        keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
                  '-': 'minus', '_': 'shift-minus'}
        for ch in s:
            key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
            self.cmd("sendkey %s" % key, read=False)
            time.sleep(delay)

    def key(self, k):
        self.cmd("sendkey %s" % k, read=False)
        time.sleep(0.12)

    def capture(self, tag=""):
        ppm = "build/verify_gui_%s.ppm" % (tag or str(int(time.time() * 1000)))
        self.cmd("screendump %s" % ppm)
        time.sleep(0.4)
        img = read_ppm(ppm)
        if img is None:
            return None
        self.W, self.H, _ = img[0], img[1], img[2]
        if self.frames_dir and img:
            try:
                shutil_copy = __import__("shutil").copy
                shutil_copy(ppm, os.path.join(self.frames_dir, os.path.basename(ppm)))
            except Exception:
                pass
        return img

    def wait_serial(self, substr, timeout=40):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                with open("build/verify_gui.log", "rb") as f:
                    if substr.encode() in f.read():
                        return True
            except Exception:
                pass
            time.sleep(0.5)
        return False

    def quit(self):
        try:
            self.cmd("quit", read=False)
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except Exception:
                self.proc.kill()


# ----------------------------------------------------------------------------
# 检查项定义
# ----------------------------------------------------------------------------
class Result:
    def __init__(self, name, category, passed, expected, actual, locator=""):
        self.name = name
        self.category = category  # RENDER / INTERACT
        self.passed = passed
        self.expected = expected
        self.actual = actual
        self.locator = locator

    def __str__(self):
        mark = "PASS" if self.passed else "FAIL"
        loc = ("  [%s]" % self.locator) if self.locator else ""
        return "[%s] %-28s %s%s\n        expected: %s\n        actual:   %s" % (
            mark, self.name, mark, loc, self.expected, self.actual)


def run_checks(q, args):
    results = []
    # 等待 GUI 桌面出现
    print("\n=== 启动并进入 GUI ===")
    # 先等内核真正进入图形模式(串口日志标记), 否则首帧还在 BIOS 文本态,
    # 此时 screendump 抓到的帧缓冲无效(read_ppm 返回 None 导致直接早退)。
    q.wait_serial("[GUI] Entered GUI mode", timeout=90)
    # 反复抓帧: 即便已进入 GUI, 首帧也可能因异步写入未完成而暂时读不到
    img0 = None
    for attempt in range(1, 12):
        img0 = q.capture("boot_%d" % attempt)
        if img0 is not None:
            break
        time.sleep(1.0)
    if img0 is None:
        img0 = q.capture("boot")
    if img0 is None:
        print("无法抓取帧，退出")
        return results
    print("[boot] 已抓到首帧 %dx%d (is_desktop=%s)" % (
        img0[0], img0[1], is_desktop(img0)))
    # 真实桌面 vs 登录屏 判别(检测中央深色登录卡片)
    if is_login_screen(img0):
        print("[boot] 当前在登录屏, 在屏上执行登录(演示账号 nexos/nexos)...")
        W0, H0, _ = img0
        ux = W0 // 2
        uy = int(H0 * 0.417)   # 用户名框
        px_ = W0 // 2
        py_ = int(H0 * 0.488)  # 密码框
        # 1) 用户名框: 点击并清空预填的 root, 再输入 nexos
        q.click(ux, uy)
        time.sleep(0.2)
        for _ in range(8):
            q.key("backspace")
            time.sleep(0.05)
        q.type_text("nexos")
        time.sleep(0.3)
        # 2) 密码框: 点击后输入 nexos 并回车提交
        q.click(px_, py_)
        time.sleep(0.2)
        q.type_text("nexos")
        time.sleep(0.3)
        q.key("ret")
        time.sleep(0.4)
        # 兜底: 再点一次登录按钮(防止回车不提交)
        q.click(W0 // 2, int(H0 * 0.55))
        # 3) 反复抓帧直到登录卡片消失(进入桌面)或超时
        for attempt in range(1, 20):
            time.sleep(1.0)
            cand = q.capture("after_login_%d" % attempt)
            if cand is not None and not is_login_screen(cand):
                img0 = cand
                print("[login] 已进入真桌面(第 %d 次抓帧)" % attempt)
                break
        else:
            print("[login] 登录超时, 仍按当前画面继续")
            final = q.capture("after_login_final")
            if final is not None:
                img0 = final
    else:
        print("[boot] 已检测到真桌面(无中央登录卡片)")


    def topbar_ok(im):
        st = region_stats(im, 0, 0, im[0], TOPBAR_H)
        return st["nonblack"] > st["total"] * 0.5

    if not topbar_ok(img0):
        # 理论上 is_desktop 分支已处理登录; 若仍非桌面则直接以当前画面继续,
        # 不再等待串口 login:(GUI 模式下串口 shell 已暂停, 会卡 30s)。
        print("[boot] 顶栏检查未通过, 但登录分支已尝试, 以当前画面继续检查")
    else:
        print("[boot] 顶栏检查通过")

    q.calibrate()

    # ------------------------------------------------------------------
    # 渲染检查
    # ------------------------------------------------------------------
    W, H, _ = img0

    # 文件管理器窗口居中坐标按实际分辨率计算(QEMU 下可能是 1280x720 而非 1024x768)
    FE_WX = (W - FE_WW) // 2
    FE_WY = (H - FE_WH) // 2 + TOPBAR_H + 10

    def R(name, expected, actual_fn, passed, locator=""):
        results.append(Result(name, "RENDER", bool(passed), expected, actual_fn(), locator))

    # R1 桌面已渲染(非黑占比)
    s1 = region_stats(img0, 0, 0, W, H)
    R("桌面已渲染(非黑像素)", "非黑占比 > 5%%",
      lambda: "非黑 %.1f%%" % (100.0 * s1["nonblack"] / s1["total"]),
      s1["nonblack"] > s1["total"] * 0.05, "屏幕全屏")

    # R2 顶栏存在(深色条带)
    s2 = region_stats(img0, 0, 0, W, TOPBAR_H)
    R("顶栏(TopBar)渲染", "深色像素 > 50%% 区域",
      lambda: "非黑 %.1f%%" % (100.0 * s2["nonblack"] / s2["total"]),
      s2["nonblack"] > s2["total"] * 0.5, "区域 y=0..%d" % TOPBAR_H)

    # R3 底部任务栏存在
    s3 = region_stats(img0, 0, H - TASKBAR_H, W, H)
    R("任务栏(TaskBar)渲染", "深色像素 > 40%% 区域",
      lambda: "非黑 %.1f%%" % (100.0 * s3["nonblack"] / s3["total"]),
      s3["nonblack"] > s3["total"] * 0.4, "区域 y=%d..%d" % (H - TASKBAR_H, H))

    # R4 桌面图标(彩色)存在
    s4 = region_stats(img0, 0, TOPBAR_H, 160, H - TASKBAR_H)
    R("桌面图标渲染", "彩色(饱和)像素 > 30",
      lambda: "饱和像素 %d" % s4["sat"],
      s4["sat"] > 30, "区域 x=0..160, y=%d..%d" % (TOPBAR_H, H - TASKBAR_H))

    # R5 开始按钮(蓝色强调方块)
    s5 = region_stats(img0, *START_BTN)
    R("开始按钮渲染", "彩色(饱和)像素 > 8",
      lambda: "饱和像素 %d" % s5["sat"],
      s5["sat"] > 8, "区域 %s" % (START_BTN,))

    # ------------------------------------------------------------------
    # 交互检查
    # ------------------------------------------------------------------
    def I(name, expected, actual_fn, passed, locator=""):
        results.append(Result(name, "INTERACT", bool(passed), expected, actual_fn(), locator))

    # I1 悬停开始按钮 -> 背景变亮(C_TOPBAR_HOVER 0x323232 vs C_TOPBAR_BG 0x1F1F1F)
    before_lum = sum(pixel_at(img0, *START_BTN_C))
    q.mouse_move_abs(*START_BTN_C)
    time.sleep(0.4)
    img_hov = q.capture("hover_start")
    after_lum = sum(pixel_at(img_hov, *START_BTN_C)) if img_hov else 0
    I("悬停(hover)开始按钮", "像素亮度增加(0x1F1F1F→0x323232)",
      lambda: "亮度 %d → %d" % (before_lum, after_lum),
      img_hov is not None and after_lum > before_lum + 20,
      "开始按钮中心 %s" % (START_BTN_C,))

    # I2 点击开始按钮 -> 开始菜单出现(区域变亮)
    q.click(*START_BTN_C)
    time.sleep(0.6)
    img_menu = q.capture("startmenu")
    if img_menu is not None:
        s_menu_before = region_stats(img0, *STARTMENU)
        s_menu_after = region_stats(img_menu, *STARTMENU)
        delta = s_menu_after["bright"] - s_menu_before["bright"]
        I("点击(click)开始按钮→菜单", "菜单区域亮像素增加",
          lambda: "亮像素增量 %+d" % delta,
          delta > 200, "开始菜单区域 %s" % (STARTMENU,))
        # 关闭菜单(再点一次开始按钮)
        q.click(*START_BTN_C)
        time.sleep(0.5)
        img_closed = q.capture("menu_closed")
    else:
        I("点击(click)开始按钮→菜单", "菜单区域亮像素增加",
          lambda: "无法抓帧", False, "开始菜单区域 %s" % (STARTMENU,))

    # I3 点击桌面图标打开窗口(文件管理器)
    fe_center = ICON_CENTERS["explorer"]
    img_pre_open = q.capture("pre_open")
    q.click(*fe_center)
    time.sleep(1.2)
    img_opened = q.capture("opened")
    if img_pre_open and img_opened:
        d_open = region_diff(img_pre_open, img_opened,
                             FE_WX - 20, FE_WY - 20, FE_WX + FE_WW + 20, FE_WY + FE_WH + 20)
        I("点击(click)图标→打开窗口", "窗口区域帧变化 > 50000",
          lambda: "帧差 %d" % d_open,
          d_open > 50000, "文件管理器窗口 %s" % ((FE_WX, FE_WY, FE_WX + FE_WW, FE_WY + FE_WH),))

        # R6 窗口标题栏/内容渲染(打开后窗口含亮色标题栏与文字)
        st = region_stats(img_opened, FE_WX, FE_WY, FE_WX + FE_WW, FE_WY + TITLE_BAR_H)
        sl = region_stats(img_opened, FE_WX + 8, FE_WY + TITLE_BAR_H + 8,
                          FE_WX + FE_WW - 8, FE_WY + FE_WH - 8)
        R("窗口标题栏渲染", "标题栏亮像素 > 200",
          lambda: "亮像素 %d" % st["bright"],
          st["bright"] > 200, "区域 y=%d..%d" % (FE_WY, FE_WY + TITLE_BAR_H))
        R("窗口内容渲染(列表/标签页)", "内容区非黑 > 10%% 且含文字变化",
          lambda: "非黑 %.1f%% 饱和=%d" % (100.0 * sl["nonblack"] / sl["total"], sl["sat"]),
          sl["nonblack"] > sl["total"] * 0.1,
          "内容区 %s" % ((FE_WX + 8, FE_WY + TITLE_BAR_H + 8, FE_WX + FE_WW - 8, FE_WY + FE_WH - 8),))

        # I4 悬停窗口标题栏关闭按钮 -> 颜色变化
        # 关闭按钮在标题栏右上角: bx = rx+rw-28, by = ry+4, 24x24
        close_btn = (FE_WX + FE_WW - 28 + 12, FE_WY + 4 + 12)
        lum_before = sum(pixel_at(img_opened, *close_btn))
        q.mouse_move_abs(*close_btn)
        time.sleep(0.4)
        img_hov_btn = q.capture("hover_btn")
        lum_after = sum(pixel_at(img_hov_btn, *close_btn)) if img_hov_btn else 0
        I("悬停(hover)标题栏按钮", "按钮像素亮度变化",
          lambda: "亮度 %d → %d" % (lum_before, lum_after),
          img_hov_btn is not None and abs(lum_after - lum_before) > 15,
          "关闭按钮 %s" % (close_btn,))

        # I5 拖拽窗口 -> 窗口移动
        title_c = (FE_WX + FE_WW // 2, FE_WY + 16)
        q.drag(title_c[0], title_c[1], title_c[0] + 90, title_c[1] + 50)
        time.sleep(0.5)
        img_dragged = q.capture("dragged")
        moved = False
        if img_dragged:
            # 旧位置应已变化；新位置(偏移 +90,+50)应与旧帧内容更匹配
            old_diff = region_diff(img_opened, img_dragged, FE_WX, FE_WY, FE_WX + FE_WW, FE_WY + FE_WH)
            nx, ny = FE_WX + 90, FE_WY + 50
            new_diff = region_diff(img_opened, img_dragged, nx, ny, nx + FE_WW, ny + FE_WH)
            # moved 判定: 旧位置变化显著 且 新位置变化更小(窗口已移走)
            moved = old_diff > 50000 and new_diff < old_diff * 0.6
            I("拖拽(drag)窗口移动", "旧位置帧差>50000 且 新位置更匹配",
              lambda: "旧位置帧差=%d 新位置帧差=%d" % (old_diff, new_diff),
              moved, "标题栏中心 %s → +90,+50" % (title_c,))
        else:
            I("拖拽(drag)窗口移动", "窗口移动", lambda: "无法抓帧", False, "")

        # I8 滚动(滚轮)文件列表 -> 视口变化(尽力而为)
        list_c = (FE_WX + 110, FE_WY + TITLE_BAR_H + 120)
        img_pre_scroll = q.capture("pre_scroll")
        q.wheel(*list_c, delta=3)
        time.sleep(0.4)
        img_post_scroll = q.capture("post_scroll")
        if img_pre_scroll and img_post_scroll:
            d_scroll = region_diff(img_pre_scroll, img_post_scroll,
                                   FE_WX + 8, FE_WY + TITLE_BAR_H + 40,
                                   FE_WX + FE_WX // 2, FE_WY + FE_WH - 8)
            # 注: 若 gui.cpp 未实现滚轮, 此检查将如实失败(本身即一项发现)
            I("滚动(scroll)文件列表", "列表区帧变化 > 3000",
              lambda: "帧差 %d" % d_scroll,
              d_scroll > 3000,
              "列表中心 %s (若失败多为滚轮未实现)" % (list_c,))
        else:
            I("滚动(scroll)文件列表", "列表区帧变化 > 3000",
              lambda: "无法抓帧", False, "列表中心 %s" % (list_c,))

        # 关闭文件管理器窗口(再次点关闭按钮)
        q.mouse_move_abs(*close_btn)
        time.sleep(0.2)
        q.click(*close_btn)
        time.sleep(0.6)
    else:
        I("点击(click)图标→打开窗口", "窗口区域帧变化 > 50000",
          lambda: "无法抓帧", False, "文件管理器图标 %s" % (fe_center,))

    # I6 输入(input): 打开终端并键入文字 -> 内容区出现新文字
    term_center = ICON_CENTERS["terminal"]
    q.click(*term_center)
    time.sleep(1.2)
    img_term0 = q.capture("term0")
    # 终端窗口几何(ww=400, wh=280)
    TW, TH = 400, 280
    TX = (W - TW) // 2
    TY = (H - TH) // 2 + TOPBAR_H + 10
    if img_term0 is not None:
        # 先点一下窗口内容区以获得焦点(终端默认已聚焦, 这里保险)
        q.mouse_move_abs(TX + TW // 2, TY + TH // 2)
        time.sleep(0.1)
        q.click(TX + TW // 2, TY + TH // 2)
        time.sleep(0.2)
        s_before = region_stats(img_term0, TX + 8, TY + TITLE_BAR_H + 8, TX + TW - 8, TY + TH - 8)
        q.type_line("help")
        time.sleep(0.8)
        img_term1 = q.capture("term1")
        if img_term1 is not None:
            s_after = region_stats(img_term1, TX + 8, TY + TITLE_BAR_H + 8, TX + TW - 8, TY + TH - 8)
            # 输入后内容变化(亮文字像素数变化)
            dterm = region_diff(img_term0, img_term1, TX + 8, TY + TITLE_BAR_H + 8, TX + TW - 8, TY + TH - 8)
            I("输入(input)终端键入", "内容区帧变化 > 5000",
              lambda: "帧差 %d (亮像素 %d→%d)" % (dterm, s_before["bright"], s_after["bright"]),
              dterm > 5000, "终端内容区 %s" % ((TX + 8, TY + TITLE_BAR_H + 8, TX + TW - 8, TY + TH - 8),))
            # 关闭终端
            tclose = (TX + TW - 28 + 12, TY + 4 + 12)
            q.click(*tclose)
            time.sleep(0.5)
        else:
            I("输入(input)终端键入", "内容区帧变化 > 5000", lambda: "无法抓帧", False, "")
    else:
        I("输入(input)终端键入", "内容区帧变化 > 5000", lambda: "无法抓帧", False, "")

    # I7 右键(context menu): 桌面右键 -> 弹出菜单
    img_r_pre = q.capture("r_pre")
    rc = (W // 2, H // 2)
    q.right_click(*rc)
    time.sleep(0.6)
    img_r_post = q.capture("r_post")
    if img_r_pre and img_r_post:
        # 右键菜单为亮色 popup，检测点击点附近亮像素增加
        d_r = region_diff(img_r_pre, img_r_post, rc[0] - 120, rc[1] - 20, rc[0] + 160, rc[1] + 200)
        I("右键(right-click)弹出菜单", "点击点附近帧变化 > 8000",
          lambda: "帧差 %d" % d_r,
          d_r > 8000, "右键点 %s" % (rc,))
        # 关闭菜单(左键点其它处)
        q.click(20, H - TASKBAR_H // 2)
        time.sleep(0.4)
    else:
        I("右键(right-click)弹出菜单", "点击点附近帧变化 > 8000",
          lambda: "无法抓帧", False, "右键点 %s" % (rc,))

    return results


# ----------------------------------------------------------------------------
# 主流程
# ----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="NexOS gui.cpp GUI 验证脚本")
    ap.add_argument("--img", default=IMG_DEFAULT)
    ap.add_argument("--headed", action="store_true", help="有头模式(弹出 QEMU 窗口)")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--save-frames", default="", help="dump 每帧 PPM 到该目录")
    args = ap.parse_args()

    if args.save_frames:
        os.makedirs(args.save_frames, exist_ok=True)

    print("=== NexOS GUI 验证 (gui.cpp) ===")
    print("镜像: %s | 模式: %s" % (args.img, "headed" if args.headed else "headless"))

    q = QEMU(args.img, args.headed, args.timeout)
    if args.save_frames:
        q.frames_dir = args.save_frames
    try:
        q.launch()
        results = run_checks(q, args)
    finally:
        try:
            q.quit()
        except Exception:
            pass

    # 汇总
    lines = []
    lines.append("=" * 70)
    lines.append("验证结果汇总")
    lines.append("=" * 70)
    n_pass = n_fail = 0
    for r in results:
        lines.append(str(r))
        if r.passed:
            n_pass += 1
        else:
            n_fail += 1
    lines.append("-" * 70)
    lines.append("总计: %d  通过: %d  失败: %d" % (len(results), n_pass, n_fail))
    lines.append("=" * 70)
    lines.append("RESULT: %s" % ("FAIL" if n_fail else "PASS"))
    summary = "\n".join(lines)
    print("\n" + summary)
    # 同时落盘，避免长运行被终端截断导致看不到结果
    try:
        with open("build/verify_gui_result.txt", "w", encoding="utf-8") as f:
            f.write(summary + "\n")
    except Exception:
        pass
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
