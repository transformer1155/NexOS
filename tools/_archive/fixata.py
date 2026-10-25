p = ".attic64/kernel64.cpp"
t = open(p, encoding="utf-8").read()
t = t.replace("ata_nobsy(2000000)", "ata_nobsy(50000)")
open(p, "w", encoding="utf-8").write(t)
print("ata_nobsy(2000000) remaining:", t.count("ata_nobsy(2000000)"))
print("ata_nobsy(50000) count:", t.count("ata_nobsy(50000)"))
