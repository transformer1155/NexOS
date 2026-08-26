import re
data = open('build/sfs.img','rb').read()
names = re.findall(rb'[ -~]{3,40}\.mex', data)
print('mex files found in sfs.img:', sorted(set(n.decode() for n in names)))
print('shell.mex present:', b'shell.mex' in data)
