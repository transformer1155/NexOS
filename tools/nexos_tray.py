#!/usr/bin/env python3
"""NexOS Developer Toolkit - 系统托盘宿主 (零第三方依赖: ctypes + Win32 Shell_NotifyIcon)

后端状态直接显示在托盘图标与 tooltip 上:
  绿色 = gateway 在线 / 灰红 = gateway 已停止
右键菜单: 启动/停止 gateway, 打开 WebView2 GUI, 查看日志, 刷新状态, 退出

用法:
  python tools/nexos_tray.py          # 常驻托盘
  python tools/nexos_tray.py --once   # 只查一次状态后退出 (自检)
"""

import ctypes
import ctypes.wintypes as wt
import http.client
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import traceback

# ------------------------------------------------------------------ 路径解析
_HERE = os.path.dirname(os.path.abspath(__file__))
_PROBE = _HERE
while True:
    _parent = os.path.dirname(_PROBE)
    if _parent == _PROBE:
        break
    if os.path.basename(_PROBE) == "tools":
        break
    _PROBE = _parent
if os.path.basename(_PROBE) == "tools":
    TOOLS = _PROBE
    REPO_ROOT = os.path.dirname(_PROBE)
else:
    TOOLS = os.path.join(os.path.dirname(_HERE), "tools")
    REPO_ROOT = os.path.dirname(TOOLS)

TMP = tempfile.gettempdir()
LOG_PATH = os.path.join(TMP, "nexos_tray.log")
GW_LOG = os.path.join(TMP, "nexos_gateway.log")
ICON_GREEN = os.path.join(TMP, "nexos_tray_green.ico")
ICON_RED = os.path.join(TMP, "nexos_tray_red.ico")

GW_HOST = os.environ.get("NEXOS_GW_HOST", "127.0.0.1")
GW_PORT = int(os.environ.get("NEXOS_GW_PORT", "19387"))


def _log(msg):
    line = time.strftime("%H:%M:%S ") + str(msg)
    print(line, flush=True)
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def _find(name, *candidates):
    for c in candidates:
        if c and os.path.exists(c):
            return c
    for root, _dirs, files in os.walk(TOOLS):
        if name in files:
            return os.path.join(root, name)
    return None


GW_SCRIPT = _find("gateway_server.py",
                  os.path.join(TOOLS, "net", "gateway_server.py"),
                  os.path.join(TOOLS, "gateway_server.py"))
GUI_SCRIPT = _find("nexos_gui.py",
                   os.path.join(TOOLS, "nexos_gui.py"),
                   os.path.join(TOOLS, "_archive", "nexos_gui.py"))


# ------------------------------------------------------------------ 图标生成
def _build_ico(path, ring):
    """手写 32x32 32bpp ICO: 圆环 + 白色 N 字母."""
    W = H = 32
    px = [[(0, 0, 0, 0)] * W for _ in range(H)]

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            px[y][x] = (c[0], c[1], c[2], 255)

    def disc(cx, cy, r, c):
        rr = r * r
        for y in range(H):
            for x in range(W):
                if (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr:
                    put(x, y, c)

    def line(x0, y0, x1, y1, c, t=3):
        steps = 60
        half = t // 2
        for i in range(steps + 1):
            f = i / steps
            x = x0 + (x1 - x0) * f
            y = y0 + (y1 - y0) * f
            for dy in range(-half, half + 1):
                for dx in range(-half, half + 1):
                    put(int(round(x)) + dx, int(round(y)) + dy, c)

    disc(16, 16, 15, ring)             # 外圈 = 状态色
    disc(16, 16, 12, (13, 20, 34))     # 内底深色
    line(10, 24, 10, 8, (255, 255, 255))
    line(22, 24, 22, 8, (255, 255, 255))
    line(10, 8, 22, 24, (255, 255, 255))

    xor = bytearray()
    for y in range(H - 1, -1, -1):
        for x in range(W):
            r, g, b, a = px[y][x]
            xor += bytes((b, g, r, a))
    and_mask = bytes(W * H // 8)
    bih = struct.pack("<IiiHHIIiiII", 40, W, H * 2, 1, 32, 0,
                      len(xor) + len(and_mask), 0, 0, 0, 0)
    body = bih + bytes(xor) + and_mask
    entry = struct.pack("<BBBBHHII", W, H, 0, 0, 1, 32, len(body), 6 + 16)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, 1) + entry + body)
    return path


# ------------------------------------------------------------------ Win32 常量
WM_APP = 0x8000
WM_TRAY = WM_APP + 1
WM_COMMAND = 0x0111
WM_TIMER = 0x0113
WM_DESTROY = 0x0002
WM_RBUTTONUP = 0x0205
WM_LBUTTONUP = 0x0202
WM_LBUTTONDBLCLK = 0x0203

NIM_ADD = 0
NIM_MODIFY = 1
NIM_DELETE = 2
NIF_MESSAGE = 0x01
NIF_ICON = 0x02
NIF_TIP = 0x04
NIF_INFO = 0x10
NIIF_INFO = 0x01

MF_STRING = 0x0000
MF_GRAYED = 0x0001
MF_SEPARATOR = 0x0800
TPM_RIGHTBUTTON = 0x0002
TPM_BOTTOMALIGN = 0x0020

IMAGE_ICON = 1
LR_LOADFROMFILE = 0x0010

ID_START = 101
ID_STOP = 102
ID_GUI = 103
ID_LOG = 104
ID_REFRESH = 105
ID_EXIT = 110


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wt.DWORD), ("Data2", wt.WORD),
                ("Data3", wt.WORD), ("Data4", ctypes.c_byte * 8)]


class NOTIFYICONDATAW(ctypes.Structure):
    _fields_ = [
        ("cbSize", wt.DWORD),
        ("hWnd", wt.HWND),
        ("uID", wt.UINT),
        ("uFlags", wt.UINT),
        ("uCallbackMessage", wt.UINT),
        ("hIcon", wt.HICON),
        ("szTip", wt.WCHAR * 128),
        ("dwState", wt.DWORD),
        ("dwStateMask", wt.DWORD),
        ("szInfo", wt.WCHAR * 256),
        ("uVersion", wt.UINT),
        ("szInfoTitle", wt.WCHAR * 64),
        ("dwInfoFlags", wt.DWORD),
        ("guidItem", GUID),
        ("hBalloonIcon", wt.HICON),
    ]


class MSG(ctypes.Structure):
    _fields_ = [("hWnd", wt.HWND), ("message", wt.UINT),
                ("wParam", wt.WPARAM), ("lParam", wt.LPARAM),
                ("time", wt.DWORD), ("pt", wt.POINT)]


WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT,
                             wt.WPARAM, wt.LPARAM)


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE), ("hbrBackground", wt.HANDLE),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]


user32 = ctypes.WinDLL("user32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
kernel32.GetModuleHandleW.restype = wt.HINSTANCE
user32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
user32.RegisterClassW.restype = wt.ATOM
user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
user32.CreateWindowExW.restype = wt.HWND
user32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.LoadImageW.argtypes = [wt.HINSTANCE, wt.LPCWSTR, wt.UINT,
                              ctypes.c_int, ctypes.c_int, wt.UINT]
user32.LoadImageW.restype = wt.HANDLE
user32.CreatePopupMenu.restype = wt.HMENU
user32.AppendMenuW.argtypes = [wt.HMENU, wt.UINT, ctypes.c_size_t, wt.LPCWSTR]
user32.AppendMenuW.restype = wt.BOOL
user32.TrackPopupMenu.argtypes = [wt.HMENU, wt.UINT, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_int, wt.HWND, wt.LPVOID]
user32.TrackPopupMenu.restype = wt.BOOL
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.DestroyMenu.argtypes = [wt.HMENU]
user32.SetTimer.argtypes = [wt.HWND, ctypes.c_size_t, wt.UINT, ctypes.c_void_p]
user32.KillTimer.argtypes = [wt.HWND, ctypes.c_size_t]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.PostQuitMessage.argtypes = [ctypes.c_int]
user32.DestroyWindow.argtypes = [wt.HWND]
user32.GetMessageW.argtypes = [ctypes.POINTER(MSG), wt.HWND, wt.UINT, wt.UINT]
user32.GetMessageW.restype = ctypes.c_int
user32.TranslateMessage.argtypes = [ctypes.POINTER(MSG)]
user32.DispatchMessageW.argtypes = [ctypes.POINTER(MSG)]
user32.DispatchMessageW.restype = ctypes.c_ssize_t
shell32.Shell_NotifyIconW.argtypes = [wt.DWORD, ctypes.POINTER(NOTIFYICONDATAW)]
shell32.Shell_NotifyIconW.restype = wt.BOOL


# ------------------------------------------------------------------ 状态探测
def gw_health(host=GW_HOST, port=None):
    """返回 (online, pkg_count, report_str)."""
    p = port or GW_PORT
    try:
        c = http.client.HTTPConnection(host, p, timeout=0.8)
        c.request("GET", "/health")
        r = c.getresponse()
        r.read()
        ok = (r.status == 200)
        c.close()
    except Exception:
        return False, 0, "unreachable"
    if not ok:
        return False, 0, "http " + str(r.status)
    count = 0
    try:
        c = http.client.HTTPConnection(host, p, timeout=1.2)
        c.request("GET", "/package/status")
        r = c.getresponse()
        body = r.read().decode("utf-8", "replace")
        c.close()
        data = json.loads(body) if body else {}
        pkgs = data.get("installed") or data.get("packages") or []
        count = len(pkgs)
    except Exception:
        pass
    return True, count, "online"


# ------------------------------------------------------------------ 托盘应用
class TrayApp(object):
    def __init__(self):
        self.hwnd = None
        self.hinstance = kernel32.GetModuleHandleW(None)
        self.icon_ok = None
        self.icon_bad = None
        self.gw_proc = None
        self.online = False
        self.pkg_count = 0
        self.status_text = "starting"
        self.class_name = "NexOSTrayWnd"
        self._wndproc_ref = None
        self._last_logged = None

    # -- gateway 控制
    def gw_start(self):
        if self.gw_proc and self.gw_proc.poll() is None:
            _log("gateway already running (pid %s)" % self.gw_proc.pid)
            return
        if not GW_SCRIPT:
            _log("gateway_server.py not found under " + TOOLS)
            return
        pkg_dir = os.path.join(REPO_ROOT, "mkfs")
        args = [sys.executable, GW_SCRIPT, "--port", str(GW_PORT), "--pkg-dir", pkg_dir]
        try:
            out = open(GW_LOG, "a", encoding="utf-8")
            self.gw_proc = subprocess.Popen(args, cwd=REPO_ROOT,
                                            stdout=out, stderr=subprocess.STDOUT,
                                            creationflags=0x08000000)
            _log("gateway started pid %s -> %s:%s" % (self.gw_proc.pid, GW_HOST, GW_PORT))
        except Exception as e:
            _log("gateway start failed: %s" % e)
        time.sleep(1.0)
        self.refresh(force=True)

    def gw_stop(self):
        if self.gw_proc and self.gw_proc.poll() is None:
            try:
                self.gw_proc.terminate()
                for _ in range(20):
                    if self.gw_proc.poll() is not None:
                        break
                    time.sleep(0.1)
                if self.gw_proc.poll() is None:
                    self.gw_proc.kill()
                _log("gateway stopped (pid %s)" % self.gw_proc.pid)
            except Exception as e:
                _log("gateway stop failed: %s" % e)
        else:
            self._kill_stray()
        time.sleep(0.3)
        self.refresh(force=True)

    def _kill_stray(self):
        """没有子进程句柄时, 按端口找并结束占用进程."""
        try:
            cmd = ("$p=Get-NetTCPConnection -LocalPort %d -State Listen "
                   "-ErrorAction SilentlyContinue; if($p){Stop-Process -Id $p.OwningProcess -Force}"
                   % GW_PORT)
            subprocess.run(["powershell", "-NoProfile", "-Command", cmd],
                           capture_output=True, timeout=10)
            _log("killed stray listener on port %d" % GW_PORT)
        except Exception as e:
            _log("stray kill failed: %s" % e)

    def open_gui(self):
        if not GUI_SCRIPT:
            _log("nexos_gui.py not found under " + TOOLS)
            return
        try:
            subprocess.Popen([sys.executable, GUI_SCRIPT], cwd=REPO_ROOT,
                             creationflags=0x08000000)
            _log("opened GUI: " + GUI_SCRIPT)
        except Exception as e:
            _log("open GUI failed: %s" % e)

    def open_log(self):
        for p in (GW_LOG, LOG_PATH):
            if os.path.exists(p):
                try:
                    subprocess.Popen(["notepad.exe", p])
                except Exception:
                    pass
                return
        _log("no log file yet")

    # -- 状态刷新 -> 图标 + tooltip
    def refresh(self, force=False):
        online, count, report = gw_health()
        self.online = online
        self.pkg_count = count
        state = "ON" if online else "OFF"
        if online:
            self.status_text = "gateway ONLINE %s:%s | %d pkgs" % (GW_HOST, GW_PORT, count)
        else:
            self.status_text = "gateway STOPPED (%s)" % report
        if force or self._last_logged != state:
            _log("status: " + self.status_text)
            self._last_logged = state
        self._apply_icon()

    def _apply_icon(self):
        if not self.hwnd:
            return
        icon = self.icon_ok if self.online else self.icon_bad
        nid = NOTIFYICONDATAW()
        nid.cbSize = ctypes.sizeof(NOTIFYICONDATAW)
        nid.hWnd = self.hwnd
        nid.uID = 1
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP
        nid.uCallbackMessage = WM_TRAY
        nid.hIcon = icon
        nid.szTip = self.status_text[:120]
        shell32.Shell_NotifyIconW(NIM_MODIFY, ctypes.byref(nid))

    def balloon(self, title, body):
        if not self.hwnd:
            return
        nid = NOTIFYICONDATAW()
        nid.cbSize = ctypes.sizeof(NOTIFYICONDATAW)
        nid.hWnd = self.hwnd
        nid.uID = 1
        nid.uFlags = NIF_INFO
        nid.szInfoTitle = title[:60]
        nid.szInfo = body[:250]
        nid.dwInfoFlags = NIIF_INFO
        shell32.Shell_NotifyIconW(NIM_MODIFY, ctypes.byref(nid))

    # -- 菜单
    def _menu(self):
        m = user32.CreatePopupMenu()
        user32.AppendMenuW(m, MF_STRING | MF_GRAYED, 0, "NexOS Developer Toolkit")
        user32.AppendMenuW(m, MF_SEPARATOR, 0, None)
        user32.AppendMenuW(m, MF_STRING | MF_GRAYED, 0, "status: " + self.status_text)
        user32.AppendMenuW(m, MF_SEPARATOR, 0, None)
        if self.online:
            user32.AppendMenuW(m, MF_STRING, ID_STOP, "Stop gateway")
            user32.AppendMenuW(m, MF_STRING | MF_GRAYED, ID_START, "Start gateway")
        else:
            user32.AppendMenuW(m, MF_STRING, ID_START, "Start gateway")
            user32.AppendMenuW(m, MF_STRING | MF_GRAYED, ID_STOP, "Stop gateway")
        user32.AppendMenuW(m, MF_STRING, ID_GUI, "Open GUI (WebView2)")
        user32.AppendMenuW(m, MF_STRING, ID_LOG, "Open log")
        user32.AppendMenuW(m, MF_STRING, ID_REFRESH, "Refresh status")
        user32.AppendMenuW(m, MF_SEPARATOR, 0, None)
        user32.AppendMenuW(m, MF_STRING, ID_EXIT, "Exit")
        return m

    def _show_menu(self):
        m = self._menu()
        user32.SetForegroundWindow(self.hwnd)
        pt = wt.POINT()
        user32.GetCursorPos(ctypes.byref(pt))
        cmd = user32.TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                    pt.x, pt.y, 0, self.hwnd, None)
        user32.PostMessageW(self.hwnd, 0x0000, 0, 0)
        user32.DestroyMenu(m)
        if cmd:
            self._command(cmd)

    def _command(self, cmd):
        if cmd == ID_START:
            self.gw_start()
        elif cmd == ID_STOP:
            self.gw_stop()
        elif cmd == ID_GUI:
            self.open_gui()
        elif cmd == ID_LOG:
            self.open_log()
        elif cmd == ID_REFRESH:
            self.refresh(force=True)
        elif cmd == ID_EXIT:
            self.quit()

    def _wndproc(self, hwnd, msg, wparam, lparam):
        try:
            if msg == WM_TRAY:
                ev = lparam & 0xFFFF
                if ev == WM_RBUTTONUP:
                    self._show_menu()
                elif ev == WM_LBUTTONDBLCLK:
                    self.open_gui()
                elif ev == WM_LBUTTONUP:
                    self.refresh(force=True)
                    self.balloon("NexOS", self.status_text)
                return 0
            if msg == WM_COMMAND:
                self._command(wparam & 0xFFFF)
                return 0
            if msg == WM_TIMER:
                self.refresh()
                return 0
            if msg == WM_DESTROY:
                user32.PostQuitMessage(0)
                return 0
        except Exception:
            _log("wndproc error: " + traceback.format_exc())
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def quit(self):
        try:
            nid = NOTIFYICONDATAW()
            nid.cbSize = ctypes.sizeof(NOTIFYICONDATAW)
            nid.hWnd = self.hwnd
            nid.uID = 1
            shell32.Shell_NotifyIconW(NIM_DELETE, ctypes.byref(nid))
        except Exception:
            pass
        if self.gw_proc and self.gw_proc.poll() is None:
            try:
                self.gw_proc.terminate()
            except Exception:
                pass
        user32.KillTimer(self.hwnd, 1)
        user32.DestroyWindow(self.hwnd)
        _log("tray exiting")

    def run(self):
        _log("=== nexos_tray.py starting ===")
        _log("TOOLS=" + TOOLS)
        _log("REPO_ROOT=" + REPO_ROOT)
        _log("GW_SCRIPT=" + str(GW_SCRIPT))
        _log("GUI_SCRIPT=" + str(GUI_SCRIPT))
        _log("python=" + sys.executable)

        _build_ico(ICON_GREEN, (34, 197, 94))
        _build_ico(ICON_RED, (220, 72, 72))
        self.icon_ok = user32.LoadImageW(None, ICON_GREEN, IMAGE_ICON, 0, 0, LR_LOADFROMFILE)
        self.icon_bad = user32.LoadImageW(None, ICON_RED, IMAGE_ICON, 0, 0, LR_LOADFROMFILE)
        _log("icons: ok=%s bad=%s" % (bool(self.icon_ok), bool(self.icon_bad)))

        wc = WNDCLASSW()
        wc.style = 0
        wc.lpfnWndProc = self._wndproc_cb
        wc.hInstance = self.hinstance
        wc.lpszClassName = self.class_name
        wc.hIcon = self.icon_ok
        atom = user32.RegisterClassW(ctypes.byref(wc))
        _log("RegisterClass atom=" + str(atom))
        if not atom:
            _log("RegisterClass failed err=" + str(ctypes.get_last_error()))
            return 2

        self.hwnd = user32.CreateWindowExW(0, self.class_name, "NexOS Tray", 0,
                                           0, 0, 0, 0, None, None,
                                           self.hinstance, None)
        _log("hwnd=" + str(self.hwnd))
        if not self.hwnd:
            _log("CreateWindow failed err=" + str(ctypes.get_last_error()))
            return 3

        nid = NOTIFYICONDATAW()
        nid.cbSize = ctypes.sizeof(NOTIFYICONDATAW)
        nid.hWnd = self.hwnd
        nid.uID = 1
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP
        nid.uCallbackMessage = WM_TRAY
        nid.hIcon = self.icon_bad
        nid.szTip = "NexOS - checking gateway..."
        added = shell32.Shell_NotifyIconW(NIM_ADD, ctypes.byref(nid))
        _log("Shell_NotifyIcon(NIM_ADD)=" + str(bool(added)))
        if not added:
            _log("tray icon add failed err=" + str(ctypes.get_last_error()))

        self.refresh(force=True)
        user32.SetTimer(self.hwnd, 1, 2000, None)

        msg = MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        _log("message loop ended")
        return 0

    # wndproc 回调需长期持有引用
    @property
    def _wndproc_cb(self):
        if self._wndproc_ref is None:
            self._wndproc_ref = WNDPROC(self._wndproc)
        return self._wndproc_ref


def main():
    try:
        with open(LOG_PATH, "w", encoding="utf-8") as f:
            f.write("")
    except Exception:
        pass

    if "--once" in sys.argv or "--selftest" in sys.argv:
        online, count, report = gw_health()
        print("TOOLS      : " + TOOLS)
        print("REPO_ROOT  : " + REPO_ROOT)
        print("GW_SCRIPT  : " + str(GW_SCRIPT))
        print("GUI_SCRIPT : " + str(GUI_SCRIPT))
        print("gateway    : %s (pkgs=%d, %s)" % ("ON" if online else "OFF", count, report))
        _build_ico(ICON_GREEN, (34, 197, 94))
        _build_ico(ICON_RED, (220, 72, 72))
        print("icon green : %s (%d bytes)" % (ICON_GREEN, os.path.getsize(ICON_GREEN)))
        print("icon red   : %s (%d bytes)" % (ICON_RED, os.path.getsize(ICON_RED)))
        return 0

    app = TrayApp()
    try:
        return app.run()
    except Exception:
        _log("FATAL: " + traceback.format_exc())
        return 1


if __name__ == "__main__":
    sys.exit(main())
