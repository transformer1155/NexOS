from PIL import Image
for src, dst in [('shot.ppm','shot_small.png'), ('shot_keys.ppm','shot_keys_small.png')]:
    Image.open(src).convert('RGB').resize((320,180)).save(dst)
    print('saved', dst)
