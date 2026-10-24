#!/usr/bin/env bash
cd /mnt/e/NexOS/NexOS/drivers || exit 1
gcc -O1 -w -I. -o /tmp/drvtest _host_test.c driver_loader.c sample_driver.c 2>&1
echo "build_rc=$?"
/tmp/drvtest
