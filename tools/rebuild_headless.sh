#!/bin/bash
# V2.1 headless incremental build (Task 1/2/3 verification) + model externalisation.
#
# Only .attic64/kernel64.cpp, .attic64/entry64.asm and Makefile changed, so
# NO C# / mex / sfs rebuild is needed.
#
# The system image no longer carries the GGUF weights (they moved to the DATA
# disk), so this build is ~30 MB and writes nothing big through the shared
# folder -- the 400 MB write used to wedge the Linux build VM.
#
# Usage (from the build VM):
#   bash tools/rebuild_headless.sh          # stage 3 (threads), lazy model
#   bash tools/rebuild_headless.sh 2        # stage 2 (timer IRQ only)
#   bash tools/rebuild_headless.sh 3 0      # stage 3, load the model at boot
#   bash tools/rebuild_headless.sh 0        # legacy GUI escape hatch
#
# The second argument is NEX_MODEL_LAZY: 1 = boot only prints the descriptor,
# 0 = load the weights and run one inference during boot (that is how the
# externalised data-disk read path is regression-tested).
cd /media/sf_NexOS/NexOS || exit 1
LOG=/media/sf_NexOS/NexOS/build/build_out.txt
STAGE="${1:-3}"
LAZY="${2:-1}"
date > "$LOG"
echo "HEADLESS_STAGE=$STAGE NEX_MODEL_LAZY=$LAZY MODEL=external" >> "$LOG"
# The shared folder has coarse mtime granularity: touch + delete objects or
# make will silently link the previous stage's kernel.
#
# Only the kernel changed here (no C# edit), but the shared folder's coarse
# timestamps make make think every mex app is out of date and rebuild all 19
# of them -- roughly 20 minutes of dotnet work per build.  Touch the packed
# apps and the SFS image so they are newer than the C# sources and the
# rebuild is skipped.  Delete build/sfs.img only when a C# file really changed.
touch .attic64/kernel64.cpp .attic64/entry64.asm
rm -f build/kernel64.o build/entry64.o \
      build/kernel64.elf build/kernel64.bin build/os_v2.img
touch build/sfs_files/*.mex build/sfs.img 2>/dev/null || true
setsid bash -c "cd /media/sf_NexOS/NexOS; make build/os_v2.img HEADLESS_STAGE=$STAGE NEX_MODEL_LAZY=$LAZY >> '$LOG' 2>&1; echo MAKE_EXIT=\$? >> '$LOG'" </dev/null >/dev/null 2>&1 &
echo "BUILD_PID=$!" >> "$LOG"
