#!/usr/bin/env bash
echo '--- rel.text ---'
readelf -r /tmp/elf_test_driver.drv 2>/dev/null | head -30
