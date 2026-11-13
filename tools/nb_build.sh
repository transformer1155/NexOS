#!/bin/bash
# Compile the OS image inside the Linux build VM (shared folder mount).
#
# WHY DETACHED: `vm-run` opens an SSH session whose output is truncated and
# whose disconnect kills the foreground process.  A full `make all` takes
# ~20 min (19 dotnet builds), so it MUST run detached with the result
# written to a log file that a second script polls.
#
# Usage (Windows host):
#     & "C:/windows/vm-run.bat" push tools/nb_build.sh /tmp/nb_build.sh
#     & "C:/windows/vm-run.bat" "bash /tmp/nb_build.sh"
#     & "C:/windows/vm-run.bat" "bash /tmp/nb_poll.sh"
#
# Env knobs (read by nb_poll.sh):
#     NB_MODEL   GGUF to embed (default build/qwen2-0_5b-instruct-q4_k_m.gguf)
#     NB_LOG     log file path        (default /tmp/mk.log)
cd /media/sf_NexOS/NexOS || { echo "NO REPO"; exit 1; }
# Shared-folder mtime is unreliable: touch changed sources so make rebuilds
# the corresponding objects instead of skipping them.
touch .attic64/entry64.asm .attic64/kernel64.cpp nexos_compat.cpp \
      nexos_compat_asm.cpp osapi.cpp win32.cpp mforms.cpp nexos_compat.h osapi.h
LOG=${NB_LOG:-/tmp/mk.log}
MODEL=${NB_MODEL:-build/qwen2-0_5b-instruct-q4_k_m.gguf}
setsid bash -c "cd /media/sf_NexOS/NexOS; make -j1 all MODEL_GGUF=$MODEL > $LOG 2>&1; echo \"RC=\$?\" >> $LOG" \
    < /dev/null > /dev/null 2>&1 &
disown
echo "launched: log=$LOG model=$MODEL"
