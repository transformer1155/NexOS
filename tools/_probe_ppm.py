with open("build/tb_pin_menu.ppm", "rb") as f:
    hdr = f.read(48)
    print(repr(hdr))
