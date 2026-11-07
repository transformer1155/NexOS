#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
bash /media/sf_NexOS/webemu/tools/_selftest_export_assert.sh
echo "rc=$?"
