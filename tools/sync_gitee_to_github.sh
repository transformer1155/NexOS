#!/usr/bin/env bash
# ================================================================
# sync_gitee_to_github.sh
# ----------------------------------------------------------------
# Gitee (主仓库)  →  GitHub (镜像)  单向同步脚本
#
# 用法:
#   ./tools/sync_gitee_to_github.sh              # 正常同步
#   ./tools/sync_gitee_to_github.sh --dry-run    # 只打印将要做什么
#   GITEE_TOKEN=xxx ./tools/sync_gitee_to_github.sh
#   GITHUB_TOKEN=yyy ./tools/sync_gitee_to_github.sh
#
# 前提: 在 NexOS 仓库根目录运行。本地必须能同时访问
#       gitee.com 和 github.com (海外节点可能需要代理)。
# =================================================================
set -euo pipefail

# ---------- 仓库地址 (按需修改) --------------------------------
GITEE_REPO="https://gitee.com/transformer1155/NexOS.git"
GITHUB_REPO="https://github.com/transformer1155/NexOS.git"
GITEE_REMOTE="gitee"
GITHUB_REMOTE="origin"
# 主分支: 同时兼容 main / master, 自动检测
# ----------------------------------------------------------------

DRY_RUN=false
[[ "${1:-}" == "--dry-run" ]] && DRY_RUN=true

RED='\033[31m'; GREEN='\033[32m'; YELLOW='\033[33m'; CYAN='\033[36m'; BOLD='\033[1m'; RESET='\033[0m'
info()    { echo -e "${CYAN}➜${RESET} $*"; }
ok()      { echo -e "${GREEN} ✓${RESET} $*"; }
warn()    { echo -e "${YELLOW} ⚠${RESET} $*"; }
fail()    { echo -e "${RED} ✗${RESET} $*"; exit 1; }

run() {
    if $DRY_RUN; then
        echo -e "${YELLOW}  [dry-run]${RESET} $*"
    else
        eval "$@"
    fi
}

# ---------- 0. 仓库自检 ----------------------------------------
[[ -d .git ]] || fail "当前目录不是 git 仓库"
echo -e "${BOLD}NexOS Gitee → GitHub 同步脚本${RESET}"
echo "工作目录: $(pwd)"
$DRY_RUN && warn "DRY-RUN 模式, 不会真的执行任何 git 操作"
echo

# ---------- 1. 配置 remote --------------------------------------
info "检查 remote 配置..."

# origin → GitHub
if git remote get-url "$GITHUB_REMOTE" >/dev/null 2>&1; then
    CURRENT_ORIGIN=$(git remote get-url "$GITHUB_REMOTE")
    if [[ "$CURRENT_ORIGIN" != *"github.com"* ]]; then
        warn "$GITHUB_REMOTE 不是 GitHub (当前: $CURRENT_ORIGIN), 修正为 $GITHUB_REPO"
        run "git remote set-url $GITHUB_REMOTE $GITHUB_REPO"
    fi
    ok "$GITHUB_REMOTE → $(git remote get-url $GITHUB_REMOTE)"
else
    run "git remote add $GITHUB_REMOTE $GITHUB_REPO"
    ok "新增 $GITHUB_REMOTE → $GITHUB_REPO"
fi

# gitee → Gitee
if git remote get-url "$GITEE_REMOTE" >/dev/null 2>&1; then
    ok "$GITEE_REMOTE → $(git remote get-url $GITEE_REMOTE)"
else
    run "git remote add $GITEE_REMOTE $GITEE_REPO"
    ok "新增 $GITEE_REMOTE → $GITEE_REPO"
fi

# Token 注入 (可选, 避免交互式密码提示)
if [[ -n "${GITEE_TOKEN:-}" ]]; then
    GITEE_AUTH_URL="https://oauth2:${GITEE_TOKEN}@gitee.com/transformer1155/NexOS.git"
    run "git remote set-url $GITEE_REMOTE $GITEE_AUTH_URL"
    info "Gitee token 已注入 URL"
fi
if [[ -n "${GITHUB_TOKEN:-}" ]]; then
    GH_AUTH_URL="https://${GITHUB_TOKEN}@github.com/transformer1155/NexOS.git"
    run "git remote set-url $GITHUB_REMOTE $GH_AUTH_URL"
    info "GitHub token 已注入 URL"
fi

# ---------- 2. fetch 两边 --------------------------------------
info "fetch gitee ..."
run "git fetch $GITEE_REMOTE --prune" || fail "fetch gitee 失败 (检查网络或 token)"
info "fetch origin (GitHub) ..."
run "git fetch $GITHUB_REMOTE --prune" || fail "fetch github 失败"

# ---------- 3. 检测主分支 --------------------------------------
BRANCH=""
for cand in main master; do
    if git show-ref --verify --quiet "refs/remotes/$GITEE_REMOTE/$cand"; then
        BRANCH="$cand"
        break
    fi
done
[[ -z "$BRANCH" ]] && fail "gitee 上找不到 main 或 master 分支"
ok "主分支: $BRANCH"

# ---------- 4. 拉 gitee/main 到本地 ---------------------------
info "将 $GITEE_REMOTE/$BRANCH 合入本地 ..."

LOCAL_HEAD=$(git rev-parse HEAD 2>/dev/null)
GITEE_HEAD=$(git rev-parse "$GITEE_REMOTE/$BRANCH" 2>/dev/null)
GH_HEAD=$(git rev-parse "$GITHUB_REMOTE/$BRANCH" 2>/dev/null || echo "$GITEE_HEAD")

info "  gitee HEAD:  ${GITEE_HEAD:0:7}  $(git log -1 --oneline "$GITEE_REMOTE/$BRANCH" | cut -d' ' -f2-)"
info "  github HEAD: ${GH_HEAD:0:7}  $(git log -1 --oneline "$GITHUB_REMOTE/$BRANCH" | cut -d' ' -f2-)"
info "  本地 HEAD:   ${LOCAL_HEAD:0:7}  $(git log -1 --oneline HEAD | cut -d' ' -f2-)"

# 确认本地工作树干净
if ! $DRY_RUN && [[ -n "$(git status --porcelain)" ]]; then
    warn "本地有未提交改动, 建议先 commit 或 stash"
    git status --short | head -5
    read -rp "  继续? [y/N] " ans
    [[ "$ans" == "y" || "$ans" == "Y" ]] || exit 0
fi

# 本地 checkout 到主分支
run "git checkout $BRANCH 2>/dev/null || git checkout -b $BRANCH $GITEE_REMOTE/$BRANCH"

# 快进合入 gitee
RUN_NOW=$(eval "echo git merge --ff-only $GITEE_REMOTE/$BRANCH")
if $DRY_RUN; then
    echo -e "${YELLOW}  [dry-run]${RESET} $RUN_NOW"
else
    git merge --ff-only "$GITEE_REMOTE/$BRANCH" || {
        fail "本地落后于 gitee 且无法 fast-forward (有本地分叉)。请手动 rebase 后重试。"
    }
fi
ok "本地已与 gitee 对齐"

# ---------- 5. push 到 GitHub ----------------------------------
info "push 到 $GITHUB_REMOTE/$BRANCH ..."
RUN_PUSH=$(eval "echo git push $GITHUB_REMOTE $BRANCH")
if $DRY_RUN; then
    echo -e "${YELLOW}  [dry-run]${RESET} $RUN_PUSH"
else
    git push "$GITHUB_REMOTE" "$BRANCH" || fail "push GitHub 失败 (检查 token 或权限)"
fi
ok "push 完成"

# ---------- 6. 可选: 同步所有其他分支和 tag --------------------
if [[ "${ALL:-}" == "1" ]]; then
    info "同步所有分支 + tags ..."
    run "git push --mirror $GITHUB_REMOTE"
    run "git push $GITHUB_REMOTE --tags"
fi

# ---------- 7. 恢复 remote URL (清 token) -----------------------
if [[ -n "${GITEE_TOKEN:-}" ]] && ! $DRY_RUN; then
    git remote set-url "$GITEE_REMOTE" "$GITEE_REPO"
    info "已从 gitee URL 移除 token"
fi
if [[ -n "${GITHUB_TOKEN:-}" ]] && ! $DRY_RUN; then
    git remote set-url "$GITHUB_REMOTE" "$GITHUB_REPO"
    info "已从 github URL 移除 token"
fi

# ---------- 8. 最终报告 ----------------------------------------
GITEE_AFTER=$(git rev-parse "$GITEE_REMOTE/$BRANCH" 2>/dev/null)
GH_AFTER=$(git rev-parse "$GITHUB_REMOTE/$BRANCH" 2>/dev/null || echo "(fetch 后确认)")

echo
echo -e "${BOLD}━━━ 同步完成 ━━━${RESET}"
echo -e "  gitee/$BRANCH   →  ${GREEN}${GITEE_AFTER:0:7}${RESET}"
echo -e "  github/$BRANCH  →  ${GREEN}${GH_AFTER:0:7}${RESET}"

if [[ "${GITEE_AFTER:0:7}" == "${GH_AFTER:0:7}" || $DRY_RUN ]]; then
    ok "两端一致 ✓"
else
    warn "两端 commit hash 不同 (可能需要再 fetch origin 确认)"
fi

echo
echo "用法总结:"
echo "  日常:   ./tools/sync_gitee_to_github.sh"
echo "  带 token: GITEE_TOKEN=xxx GITHUB_TOKEN=yyy ./tools/sync_gitee_to_github.sh"
echo "  全量:   ALL=1 ./tools/sync_gitee_to_github.sh"
echo "  演练:   ./tools/sync_gitee_to_github.sh --dry-run"
