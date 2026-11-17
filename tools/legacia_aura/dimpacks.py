"""Dim the Classic Aura pack pictures (owner 2026-11-17: too bright).

usage: python3 dimpacks.py <folder with data/> <out folder> <factor, e.g. 0.6>
Pack 000 (blank) is skipped; pure magenta (see-through in BMPs) is kept.
"""
import sys, os, glob
from PIL import Image
src, out, k = sys.argv[1], sys.argv[2], float(sys.argv[3])
n = 0
for f in glob.glob(os.path.join(src, 'data/texture/effect/legacia_aura_pack/*/*')):
    rel = os.path.relpath(f, src)
    if '/000/' in rel:
        continue
    im = Image.open(f)
    im.load()
    mode = im.mode
    im = im.convert('RGBA')
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if r >= 250 and g <= 5 and b >= 250:
                continue  # magenta = see-through in the game's BMPs: keep exact
            px[x, y] = (int(r * k), int(g * k), int(b * k), a)
    dst = os.path.join(out, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    (im if mode == 'RGBA' else im.convert('RGB')).save(dst)
    n += 1
print(n, 'pictures')
