#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '=== 1. JSON validity + id ==='
for f in sfs_files/coop-bugfix.json sfs_files/coop-invalid.json; do
  python3 -c "import json,sys; d=json.load(open('$f')); print('  $f  OK id='+d['id']+' phases='+str(len(d['phases']))+' roles='+str(len(d['roles'])))"
done
echo
echo '=== 2. sfs_gen.py 可执行性（--help / 空跑）==='
python3 -c "import ast; ast.parse(open('tools/sfs_gen.py').read()); print('  AST OK')"
echo
echo '=== 3. KNOWN_TRUNCATED 与实际长名集合比较（集合，不是计数）==='
python3 - <<'EOF'
import os, re
src='sfs_files'
actual=set()
for root,dirs,names in os.walk(src):
    for n in names:
        rel=os.path.relpath(os.path.join(root,n), src).replace(os.sep,'/')
        if len(rel)>=20: actual.add(rel)
txt=open('tools/sfs_gen.py').read()
m=re.search(r'KNOWN_TRUNCATED = \{(.*?)\}', txt, re.S)
allow=set(re.findall(r"'([^']+)'", m.group(1))) if m else set()
print('  actual  >=20 :', len(actual))
print('  allowlist    :', len(allow))
print('  in actual not allowed :', sorted(actual-allow) or 'none')
print('  in allow not actual   :', sorted(allow-actual) or 'none')
print('  SETS IDENTICAL' if actual==allow else '  SETS DIFFER')
EOF
