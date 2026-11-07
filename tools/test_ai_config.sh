#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
g++ -std=gnu++17 -O2 -I. -o /tmp/t_ai_config tools/test_ai_config.cpp ai_config.cpp 2>&1 | head -25
if [ $? -ne 0 ]; then echo BUILD_FAILED; exit 1; fi
/tmp/t_ai_config
