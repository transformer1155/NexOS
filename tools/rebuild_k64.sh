#!/bin/bash
# Incremental rebuild for kernel-side .cpp edits (kernel64.cpp / osapi.cpp).
# The shared folder has coarse mtime granularity, so touch the source and
# delete the object explicitly -- otherwise make silently links a stale
# object and the fix appears to be missing in the VM.
# Runs detached: the SSH session can die without killing make.
cd /media/sf_NexOS/NexOS || exit 1
LOG=/media/sf_NexOS/NexOS/build/build_out.txt
date > "$LOG"
# L4 (like L12) touches the kernel API, the icall registry AND the managed
# toolkit.  Touch every changed source: the shared folder has coarse mtime,
# and a stale object would silently ship the previous build.
touch osapi.h osapi.cpp mforms.cpp \
      csharp/NexOS.Forms/Forms.cs csharp/apps/Shell/Shell.cs
rm -f build/osapi64.o build/mforms64.o \
      build/kernel64.elf build/kernel64.bin \
      sfs_files/shell.mex build/sfs.img build/os_v2.img
setsid bash -c "cd /media/sf_NexOS/NexOS; make build/os_v2.img MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf >> '$LOG' 2>&1; echo MAKE_EXIT=\$? >> '$LOG'" </dev/null >/dev/null 2>&1 &
echo "BUILD_PID=$!" >> "$LOG"
