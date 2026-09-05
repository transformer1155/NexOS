#!/usr/bin/env bash
# =====================================================================
#  test_ai.sh  -  AI engine integration test (headless QEMU)
# ---------------------------------------------------------------------
#  Tests: ai init, ai info, generate, ai test, ai mode, agent pipeline
# =====================================================================
set -euo pipefail
cd "$(dirname "$0")"

SRC="${1:-build/os.img}"
IMG="build/test_ai.img"
OUT="build/ai_test.log"
D1="build/ai_d1.bin"; D2="build/ai_d2.bin"; D3="build/ai_d3.bin"
D4="build/ai_d4.bin"; D5="build/ai_d5.bin"; D6="build/ai_d6.bin"

rm -f "$D1" "$D2" "$D3" "$D4" "$D5" "$D6" "$OUT" "$IMG"
cp "$SRC" "$IMG"

# Type a string then Enter
type_line(){
  local s="$1"
  local delay="${2:-0.3}"
  for ((i=0;i<${#s};i++)); do
    local ch="${s:$i:1}"
    case "$ch" in
      ' ')   echo "sendkey spc" ;;
      '.')   echo "sendkey dot" ;;
      '/')   echo "sendkey slash" ;;
      '\\')  echo "sendkey backslash" ;;
      [A-Z]) echo "sendkey shift-${ch,,}" ;;
      *)     echo "sendkey $ch" ;;
    esac
    sleep 0.05
  done
  echo "sendkey ret"; sleep "$delay"
}

echo "==> AI Engine Integration Test"
echo "    Kernel: $(stat -c%s build/kernel.bin) bytes"

(
  sleep 3

  # ---- Phase 1: ai init ----
  type_line "ai init" 4.0
  echo "memsave 0xb8000 0x1000 $D1"; sleep 0.4

  # ---- Phase 2: ai info ----
  type_line "ai info" 0.5
  echo "memsave 0xb8000 0x1000 $D2"; sleep 0.4

  # ---- Phase 3: generate text ----
  type_line "generate The quick brown fox" 3.0
  echo "memsave 0xb8000 0x1000 $D3"; sleep 0.4

  # ---- Phase 4: transformer test ----
  type_line "ai test" 1.0
  echo "memsave 0xb8000 0x1000 $D4"; sleep 0.4

  # ---- Phase 5: agent pipeline ----
  type_line "agent init" 1.0
  type_line "agent run Create a greeting message" 8.0
  echo "memsave 0xb8000 0x1000 $D5"; sleep 0.4

  # ---- Phase 6: agent status ----
  type_line "agent status" 0.5
  echo "memsave 0xb8000 0x1000 $D6"; sleep 0.4

  echo "quit"
) | qemu-system-x86_64 -drive format=raw,file="$IMG" -m 64M -display none -no-reboot \
      -monitor stdio > "$OUT" 2>&1 || true

decode(){ python3 - "$1" <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
c=bytes(d[i] for i in range(0,len(d),2)).decode('latin-1')
print("\n".join(c[i:i+80].rstrip('\x00') for i in range(0,len(c),80) if c[i:i+80].strip()))
PY
}

echo; echo "===== Phase 1: ai init =====";          [ -s "$D1" ] && decode "$D1"
echo; echo "===== Phase 2: ai info =====";          [ -s "$D2" ] && decode "$D2"
echo; echo "===== Phase 3: generate =====";         [ -s "$D3" ] && decode "$D3"
echo; echo "===== Phase 4: ai test =====";          [ -s "$D4" ] && decode "$D4"
echo; echo "===== Phase 5: agent run =====";        [ -s "$D5" ] && decode "$D5"
echo; echo "===== Phase 6: agent status =====";     [ -s "$D6" ] && decode "$D6"
echo

echo "===== Assertions ====="
python3 - "$D1" "$D2" "$D3" "$D4" "$D5" "$D6" <<'PY'
import sys, os
def load(p):
    if not os.path.exists(p) or os.path.getsize(p) == 0:
        return ""
    d=open(p,'rb').read()
    c=bytes(d[i] for i in range(0,len(d),2)).decode('latin-1')
    return c
d1=load(sys.argv[1]); d2=load(sys.argv[2]); d3=load(sys.argv[3])
d4=load(sys.argv[4]); d5=load(sys.argv[5]); d6=load(sys.argv[6])
ok=True
def chk(c,m):
    global ok; print(("PASS" if c else "FAIL")+": "+m)
    if not c: ok=False

# Phase 1: ai init
chk('AI engine initialized' in d1,    "ai init initializes the AI engine")
chk('Markov' in d1,                    "ai init reports Markov mode")
chk('Corpus trained' in d1,            "ai init trains the corpus")

# Phase 2: ai info
chk('AI Engine Status' in d2,          "ai info shows engine status")
chk('Initialized: YES' in d2,          "ai info shows initialized=YES")
chk('GPT' in d2,                       "ai info shows GPT model info")

# Phase 3: generate
chk('Generating' in d3,                "generate command starts generation")
chk('quick brown fox' in d3,           "generate output contains the prompt")

# Phase 4: transformer test
chk('Transformer forward pass' in d4,  "ai test runs transformer test")

# Phase 5: agent pipeline
chk('Agent Pipeline' in d5 or 'agent' in d5.lower(), "agent run starts pipeline")
chk('Planner' in d5,                   "agent pipeline shows Planner agent")
chk('Actor' in d5,                     "agent pipeline shows Actor agent")
chk('Critic' in d5,                    "agent pipeline shows Critic agent")

# Phase 6: agent status
chk('Agent Framework' in d6,           "agent status shows framework info")

if ok:
    print("\n=== ALL AI TESTS PASSED ===")
else:
    print("\n=== SOME AI TESTS FAILED ===")
sys.exit(0 if ok else 1)
PY
