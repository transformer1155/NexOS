#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '=== __bss_end actual address ==='
nm build/kernel64.elf 2>/dev/null | grep -w '__bss_end\|__bss_start'
echo '=== margin vs HEAP_START 0x500000 ==='
python3 - <<'EOF'
import subprocess
out = subprocess.run(['nm','build/kernel64.elf'],capture_output=True,text=True).stdout
end = int([l.split()[0] for l in out.splitlines() if l.endswith(' __bss_end')][0],16)
hs = 0x500000
print('  __bss_end = 0x%X' % end)
print('  HEAP_START= 0x%X' % hs)
print('  margin    = %d bytes' % (hs-end))
EOF
