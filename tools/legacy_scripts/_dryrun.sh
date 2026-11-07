#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '=== 1) AST / syntax ==='
python3 - <<'EOF'
import ast
ast.parse(open('tools/sfs_gen.py').read())
print('  AST OK')
EOF
echo
echo '=== 2) dry run: linux_root -> /tmp/test_linux.img (expect SUCCESS, note lines, no ERROR) ==='
python3 tools/sfs_gen.py linux_root /tmp/test_linux.img 2>&1 | tail -8
echo "  rc=${PIPESTATUS[0]}"
echo
echo '=== 3) dry run: sfs_files -> /tmp/test_sfs.img (expect SUCCESS, 12 notes, no ERROR) ==='
python3 tools/sfs_gen.py sfs_files /tmp/test_sfs.img 2>&1 | tail -6
echo "  rc=${PIPESTATUS[0]}"
echo
echo '=== 4) note-line counts per volume ==='
python3 tools/sfs_gen.py linux_root /tmp/t2.img 2>&1 | grep -c 'known long name' 
python3 tools/sfs_gen.py sfs_files /tmp/t3.img 2>&1 | grep -c 'known long name' 
echo '=== 5) any ERROR lines? (expect none) ==='
python3 tools/sfs_gen.py linux_root /tmp/t4.img 2>&1 | grep -c 'ERROR' 
python3 tools/sfs_gen.py sfs_files /tmp/t5.img 2>&1 | grep -c 'ERROR' 
rm -f /tmp/test_linux.img /tmp/test_sfs.img /tmp/t2.img /tmp/t3.img /tmp/t4.img /tmp/t5.img
