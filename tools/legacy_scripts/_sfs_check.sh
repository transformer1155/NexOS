#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '=== are my templates inside build/sfs.img? ==='
for s in bugfix-end-to-end _invalid-probe cooperation; do
  n=$(grep -c -a "$s" build/sfs.img 2>/dev/null || echo 0)
  echo "  '$s' occurrences in sfs.img: $n"
done
echo '=== does sfs.img exist / when ==='
ls -l build/sfs.img sfs_files/ai/cooperation/*.json 2>&1 | awk '{print "  "$5" "$6" "$7" "$8" "$9}'
echo '=== is sfs.img newer than the templates? (stale => not repacked) ==='
if [ build/sfs.img -nt sfs_files/ai/cooperation/bugfix-end-to-end.json ]; then echo '  sfs.img NEWER than template'; else echo '  sfs.img OLDER than template  <= would need repack'; fi
