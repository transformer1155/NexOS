img = open("build/os.img", "rb").read()
k = open("build/kernel64.bin", "rb").read()
seg = img[2048 * 512:2048 * 512 + len(k)]
print("kernel64.bin len", len(k))
print("os.img match:", seg == k)
print("serial log tail:")
d = open("build/serial_dbg.log", "rb").read()
print(repr(d[-300:]))
