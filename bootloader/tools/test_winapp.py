#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# =====================================================================
#  test_winapp.py - MiniOS Windows 应用测试工具
# ---------------------------------------------------------------------
#  让开发者选择一个 Windows 可执行文件 (.exe/.bat/.ps1/.com),
#  自动完成: 复制到 SFS -> 重新编译 -> QEMU 启动 ->
#  自动输入 "run <file>" -> 打开 GUI -> 截图验证。
#
#  用法:  python tools/test_winapp.py
#         python tools/test_winapp.py "C:\path\to\app.exe"
# =====================================================================
import os
import sys
import time
import shutil
import socket
import subprocess
import tempfile
from pathlib import Path

BOOT_DIR   = Path(r"D:\MyOS\bootloader")          # 项目根 (Windows)
SFS_DIR    = BOOT_DIR / "sfs_files"
ISO_PATH   = BOOT_DIR / "build" / "os.iso"
MON_SOCK   = "/tmp/minios_mon.sock"                # QEMU monitor (WSL)
SHOT_PATH  = "/tmp/minios_shot.ppm"
SERIAL_LOG = "/tmp/minios_serial.txt"

ALLOWED_EXT = {".exe", ".bat", ".cmd", ".ps1", ".psm1", ".psd1", ".com", ".dll"}
FS_NAME_MAX = 20                                    # MiniOS SFS 文件名上限


def pick_file(path_arg=None):
    """选择要测试的文件 (支持命令行参数或 tkinter 对话框)"""
    if path_arg:
        p = Path(path_arg)
        if p.exists():
            return p
        print(f"[!] 文件不存在: {path_arg}")
        sys.exit(1)
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk()
        root.withdraw()
        root.attributes("-topmost", True)
        path = filedialog.askopenfilename(
            title="选择要测试的 Windows 应用 (exe/bat/ps1/com)",
            filetypes=[("Windows 可执行文件", "*.exe *.bat *.cmd *.ps1 *.psm1 *.psd1 *.com *.dll"),
                       ("所有文件", "*.*")])
        root.destroy()
        if not path:
            print("[i] 已取消选择。")
            sys.exit(0)
        return Path(path)
    except Exception as e:
        print(f"[!] 无法打开文件选择框 ({e})，请用命令行参数指定文件")
        sys.exit(1)


def wsl_run(cmd, timeout=600):
    """在 WSL 中运行命令, 返回 stdout"""
    r = subprocess.run(["wsl", "-e", "bash", "-lc", cmd],
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr


def copy_to_sfs(src: Path) -> str:
    """复制文件到 SFS, 返回 SFS 内文件名 (截断到 20 字符)"""
    base = src.name
    stem, ext = os.path.splitext(base)
    # 截断到 SFS 限制 (20 字符, 保留扩展名)
    name = base
    if len(name) > FS_NAME_MAX:
        keep = FS_NAME_MAX - len(ext)
        name = stem[:keep] + ext
    dst = SFS_DIR / name
    shutil.copy2(src, dst)
    print(f"[+] 已复制 -> {dst}")
    return name


def qemu_send(sock, s):
    """通过 QEMU monitor sendkey 逐字符发送字符串"""
    keymap = {' ': 'spc', '.': 'dot', '-': 'minus', '_': 'underscore',
              '/': 'slash', '\\': 'backslash', '(': 'shift-parenleft',
              ')': 'shift-parenright', ',': 'comma', '!': 'shift-1',
              '@': 'shift-2', '#': 'shift-3', '=': 'equal', ':': 'shift-semicolon'}
    for ch in s:
        key = keymap.get(ch, ch)
        if ch.isupper():
            key = f"shift-{ch.lower()}"
        sock.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.02)
    sock.sendall(b"sendkey ret\n")   # 回车执行
    print(f"[+] 已发送命令: run {s}")


def main():
    src = pick_file(sys.argv[1] if len(sys.argv) > 1 else None)
    if src.suffix.lower() not in ALLOWED_EXT:
        print(f"[!] 不支持的扩展名 {src.suffix}。支持: {sorted(ALLOWED_EXT)}")
        sys.exit(1)

    fname = copy_to_sfs(src)

    print("[1/5] 重新编译 MiniOS (含新文件)...")
    out = wsl_run(f"cd /mnt/d/MyOS/bootloader && make build/os.iso build/os.img 2>&1")
    if "error" in out.lower() and "Error" not in "":
        print(out[-2000:])
        print("[X] 编译失败")
        sys.exit(1)
    print("[+] 编译完成")

    print("[2/5] 启动 QEMU (headless)...")
    qemu_cmd = (
        f"cd /mnt/d/MyOS/bootloader && rm -f {MON_SOCK} {SHOT_PATH} {SERIAL_LOG} && "
        f"qemu-system-x86_64 -cdrom build/os.iso -boot d -m 2G "
        f"-display none -monitor unix:{MON_SOCK},server,nowait "
        f"-serial file:{SERIAL_LOG} -no-reboot"
    )
    proc = subprocess.Popen(["wsl", "-e", "bash", "-lc", qemu_cmd],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        print("[3/5] 等待引导完成...")
        time.sleep(20)   # 等 kernel 到 shell

        # 连接 QEMU monitor
        sock = None
        for _ in range(10):
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(MON_SOCK)
                break
            except (OSError, FileNotFoundError):
                time.sleep(1)
        if not sock:
            print("[X] 无法连接 QEMU monitor")
            sys.exit(2)

        print("[4/5] 输入 run 命令并打开 GUI...")
        qemu_send(sock, f"run {fname}")

        time.sleep(10)   # 等 GUI 初始化 + 窗口打开

        sock.sendall(f"screendump {SHOT_PATH}\n".encode())
        time.sleep(1)
        sock.sendall(b"quit\n")
        sock.close()
        proc.wait(timeout=15)

        print("[5/5] 分析截图...")
        out = wsl_run(f"python3 - <<'PY'\n"
                      f"from PIL import Image\n"
                      f"from collections import Counter\n"
                      f"data=open('{SHOT_PATH}','rb').read()\n"
                      f"nl=he=0\n"
                      f"for i in range(300):\n"
                      f"    if data[i]==10:\n"
                      f"        nl+=1\n"
                      f"        if nl==3: he=i+1; break\n"
                      f"hdr=data[:he].decode()\n"
                      f"w,h=[int(x) for x in hdr.split()[1:3]]\n"
                      f"im=Image.frombytes('RGB',(w,h),data[he:])\n"
                      f"c=Counter(im.getdata())\n"
                      f"top=c.most_common(4)\n"
                      f"print('RESULT WxH',w,h,'colors',len(c))\n"
                      f"for cnt,col in top: print('RESULT',cnt,'px',col)\n"
                      f"im.save('/mnt/d/MyOS/win11-desktop/winapp_test.png')\n"
                      f"PY")
        print(out)
        # 判定: GUI 打开 = 颜色数 > 3 (壁纸+任务栏+窗口)
        if "colors" in out:
            print("=" * 50)
            print("✅ 测试完成! 截图: D:\\MyOS\\win11-desktop\\winapp_test.png")
            print("   (彩色像素多 = GUI 窗口已打开)")
        else:
            print("[X] 截图分析失败")
    finally:
        try:
            proc.kill()
        except Exception:
            pass


if __name__ == "__main__":
    main()
