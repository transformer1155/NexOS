#!/usr/bin/env bash
# =====================================================================
#  MiniOS Win11 Desktop - 命令行工具
#  用法: ./run.sh <command> [args]
#
#  Commands:
#    build      编译 (cmake 优先, 回退 make)         [默认]
#    run        编译 + 运行图形界面
#    test       无头自测: 渲染 6 帧并保存 shot_*.bmp
#    install    安装依赖 (libsdl2-dev cmake, 需要 sudo)
#    clean      清理 build 产物
#    shots      查看已生成的截图
#    help       显示帮助
#
#  例:  ./run.sh test          # 截图验证
#       ./run.sh run           # 打开桌面
# =====================================================================
set -u
cd "$(dirname "$0")"

BIN=build/win11desktop
WANT="none"

usage() {
    sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

# ---------- 依赖检查 ----------
have() { command -v "$1" >/dev/null 2>&1; }

check_deps() {
    local missing=""
    have cmake || missing="$missing cmake"
    have pkg-config || missing="$missing pkg-config"
    if ! pkg-config --exists sdl2 2>/dev/null; then
        missing="$missing libsdl2-dev"
    fi
    if [ -n "$missing" ]; then
        echo "[!] 缺少依赖:$missing"
        echo "    安装: sudo apt install -y cmake pkg-config libsdl2-dev"
        echo "    或运行: ./run.sh install"
        return 1
    fi
    return 0
}

# ---------- 命令实现 ----------
do_install() {
    echo "[*] 安装依赖 (sudo apt)..."
    sudo apt-get update
    sudo apt-get install -y cmake pkg-config libsdl2-dev g++
}

do_build() {
    check_deps || exit 1
    if [ -f build/CMakeCache.txt ]; then
        echo "[*] cmake 增量构建..."
        cmake --build build -j
    else
        echo "[*] cmake 配置 + 构建..."
        cmake -B build -DCMAKE_BUILD_TYPE=Release || exit 1
        cmake --build build -j
    fi
    if [ -x "$BIN" ]; then
        echo "[OK] 构建完成: $BIN"
    else
        echo "[!] 二进制未生成: $BIN"
        exit 1
    fi
}

do_run() {
    do_build || exit 1
    echo "[*] 启动 Win11 桌面..."
    exec "./$BIN"
}

do_test() {
    do_build || exit 1
    echo "[*] 无头自测 (SDL_VIDEODRIVER=dummy)..."
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./$BIN --selftest
    echo "---"
    ls -la shot_*.bmp 2>/dev/null || echo "[!] 未生成截图"
}

do_clean() {
    rm -rf build shot_*.bmp
    echo "[OK] 已清理"
}

do_shots() {
    ls -la shot_*.bmp 2>/dev/null || echo "[!] 暂无截图, 先运行 ./run.sh test"
}

# ---------- 入口 ----------
cmd="${1:-build}"
case "$cmd" in
    build)  do_build ;;
    run)    do_run ;;
    test)   do_test ;;
    install) do_install ;;
    clean)  do_clean ;;
    shots)  do_shots ;;
    help|-h|--help) usage ;;
    *)
        echo "[!] 未知命令: $cmd"
        usage
        ;;
esac
