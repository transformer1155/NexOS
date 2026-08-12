#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_wsl.py - Run commands inside WSL from Windows and capture output.

Usage examples:
  python tools/run_wsl.py --cmd "cd ~/bootloader && make uefi -j4" --capture build/make.log
  python tools/run_wsl.py --distro Ubuntu-20.04 --cmd "bash ~/bootloader/tools/install_and_run_wsl.sh" --capture build/serial.log

Options:
  --distro DISTRO    WSL distribution name (optional)
  --cmd CMD          Command string to run inside WSL (required)
  --workdir DIR      Working directory inside WSL (optional)
  --capture FILE     Save combined stdout+stderr to this file (Windows path)
  --timeout SEC      Timeout seconds for the WSL command
  --env KEY=VAL      Extra environment variables passed to WSL (can repeat)

Note: This script invokes the `wsl` executable and should be run from
Windows PowerShell or CMD. The captured path is relative to the current
Windows working directory.
"""

import argparse
import shlex
import subprocess
import sys
import os

# shlex.quote may not exist on older Python; fall back to pipes.quote
try:
    _quote = shlex.quote
except AttributeError:
    try:
        import pipes
        _quote = pipes.quote
    except Exception:
        # worst-case: simple escape double quotes
        def _quote(s):
            return '"' + s.replace('"', '\\"') + '"'


def build_wsl_command(cmd, workdir=None, envs=None):
    # 如果指定 workdir，则在命令前加上 cd
    if workdir:
        # 使用 bash -lc，先 cd 到 workdir 再执行命令
        full = "cd %s && %s" % (shlex.quote(workdir), cmd)
    else:
        full = cmd
    # 将环境变量注入为前缀 export A=1 B=2 && <cmd>
    if envs:
        exports = ' '.join(("%s=%s" % (k, shlex.quote(v))) for k, v in envs.items())
        full = "export %s && %s" % (exports, full)
    return full


def run_wsl(distro, cmd, timeout=None):
    # 构造 wsl 调用：如果指定 distro 使用 `wsl -d <distro> -- <shell> -lc <cmd>`
    shell_cmd = ["bash", "-lc", cmd]
    if distro:
        call = ["wsl", "-d", distro, "--"] + shell_cmd
    else:
        call = ["wsl", "--"] + shell_cmd
    # 执行并捕获输出
    try:
        sys.stdout.write("Running: %s\n" % (' '.join(call)))
    except Exception:
        pass
    try:
        proc = subprocess.Popen(call, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except OSError:
        sys.stdout.write("ERROR: 'wsl' not found or failed to execute.\n")
        return 127, b"wsl-not-found"

    # Wait for process with manual timeout to support older Python
    import time
    if timeout is None:
        out = proc.communicate()[0]
        return proc.returncode, out
    else:
        start = time.time()
        while True:
            if proc.poll() is not None:
                out = proc.communicate()[0]
                return proc.returncode, out
            if (time.time() - start) > float(timeout):
                try:
                    proc.kill()
                except Exception:
                    pass
                return 124, ("TIMEOUT after %s seconds\n" % str(timeout)).encode('utf-8')
            time.sleep(0.1)


def parse_env_list(env_list):
    env = {}
    if not env_list:
        return env
    for s in env_list:
        if '=' in s:
            k, v = s.split('=', 1)
            env[k] = v
    return env


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--distro', help='WSL distribution name (optional)')
    p.add_argument('--cmd', required=True, help='Command string to run inside WSL (use quotes)')
    p.add_argument('--workdir', help='Working directory inside WSL (optional)')
    p.add_argument('--capture', help='Save combined stdout+stderr to this file (Windows path)')
    p.add_argument('--timeout', type=int, help='Timeout seconds for the WSL command')
    p.add_argument('--env', action='append', help='Extra env vars KEY=VAL (can repeat)')
    args = p.parse_args()

    envs = parse_env_list(args.env)
    wsl_cmd = build_wsl_command(args.cmd, workdir=args.workdir, envs=envs)

    rc, out = run_wsl(args.distro, wsl_cmd, timeout=args.timeout)
    # out is bytes
    try:
        text = out.decode('utf-8', errors='replace')
    except Exception:
        text = str(out)

    if args.capture:
        cap_path = args.capture
        d = os.path.dirname(cap_path)
        if d:
            try:
                os.makedirs(d)
            except OSError:
                pass
        # write bytes
        with open(cap_path, 'wb') as f:
            f.write(out)
        print("Output saved to: %s" % cap_path)

    # Print stdout to console
    sys.stdout.write(text)

    if rc != 0:
        print("WSL command exited with code %s" % str(rc))
    sys.exit(rc)

if __name__ == '__main__':
    main()
