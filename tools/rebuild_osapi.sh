#!/bin/sh
# Robust rebuild: force-recompile touched sources, build the full kernel
# image with the embedded model, and log to the shared folder so the host
# can poll without depending on a long-lived SSH session.
cd /media/sf_NexOS/NexOS || exit 1
LOG=/media/sf_NexOS/NexOS/build/build_out.txt
date > "$LOG"
touch osapi.cpp osapi.h mforms.cpp clr.cpp clr.h .attic64/entry64.asm \
      .attic64/kernel64.cpp nexos_compat.cpp nexos_compat.h \
      csharp/NexOS.Forms/Forms.cs csharp/apps/Shell/Shell.cs Makefile 2>/dev/null
rm -f build/osapi64.o build/mforms64.o build/clr64.o build/entry64.o \
      build/kernel64.o build/nexos_compat64.o
setsid bash -c "cd /media/sf_NexOS/NexOS; make build/os_v2.img MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf >> '$LOG' 2>&1; echo MAKE_EXIT=\$? >> '$LOG'" </dev/null >/dev/null 2>&1 &
echo "BUILD_PID=$!" >> "$LOG"
