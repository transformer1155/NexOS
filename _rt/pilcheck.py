try:
    from PIL import Image, ImageFont, ImageDraw
    print("PIL_OK", Image.__name__)
    # find a CJK font
    import os
    cands = [
        "/mnt/c/Windows/Fonts/msyh.ttc",
        "/mnt/c/Windows/Fonts/simsun.ttc",
        "/mnt/c/Windows/Fonts/simhei.ttf",
        "/mnt/c/Windows/Fonts/msyh.ttf",
        "/mnt/d/MyOS/bootloader/sfs_files/msyh.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    ]
    for c in cands:
        if os.path.exists(c):
            print("FONT", c)
except Exception as e:
    print("PIL_FAIL", repr(e))
