import sys
d=open("build/img_serial.log","rb").read().decode("latin1")
markers=["[K1] kmain entered","[MFORMS] managed shell ready","[GUI] Entered GUI mode","fault","PANIC","triple","#PF","Exception"]
for m in markers:
    if m in d:
        print("FOUND:", m)
print("serial len:", len(d))
