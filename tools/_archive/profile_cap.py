import importlib.util, os
PROJ=r"D:\MyOS\bootloader"
spec=importlib.util.spec_from_file_location("p",os.path.join(PROJ,"tools","ppm_to_png.py"))
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
w,h,px=m.read_ppm(os.path.join(PROJ,"build","edge_cap.ppm"))
print("size",w,h)
def get(x,y):
    i=(y*w+x)*3; return px[i],px[i+1],px[i+2]
def lum(c): return 0.299*c[0]+0.587*c[1]+0.114*c[2]
for y in [120,200,260,340,420,520]:
    line=[]
    for x in range(0,w,16):
        c=get(x,y); line.append(int(lum(c)))
    print("y=%3d:"%y, " ".join("%3d"%v for v in line))
from collections import Counter
cnt=Counter()
for i in range(0,len(px),3*37):
    cnt[px[i],px[i+1],px[i+2]]+=1
print("top colors:")
for col,n in cnt.most_common(8):
    print("   ",col,n)
