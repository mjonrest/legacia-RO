"""Tone down the Classic Aura pack pictures (owner 2026-11-17: too bright, but faint grey parts must stay).

usage: python3 dimpacks.py <folder with data/> <out folder> <scale> <gamma>   (owner 2026-11-17: 0.45 0.6 still too bright, now 0.225 0.6)
out = 255*scale*(in/255)^gamma per channel: highlights drop a lot (255 -> 115), dark parts barely (40 -> 38).
Pack 000 (blank) is skipped; pure magenta (see-through in BMPs) is kept.
"""
import sys, os, glob
from PIL import Image
src, out, c, g = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
lut = [min(255, int(round(255 * c * (v / 255) ** g))) for v in range(256)]
n = 0
for f in glob.glob(os.path.join(src, 'data/texture/effect/legacia_aura_pack/*/*')):
    rel = os.path.relpath(f, src)
    if '/000/' in rel:
        continue
    im = Image.open(f); im.load(); mode = im.mode
    im = im.convert('RGBA'); px = im.load(); w, h = im.size
    for y in range(h):
        for x in range(w):
            r, gg, b, a = px[x, y]
            if r >= 250 and gg <= 5 and b >= 250:
                continue
            px[x, y] = (lut[r], lut[gg], lut[b], a)
    dst = os.path.join(out, rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
    (im if mode == 'RGBA' else im.convert('RGB')).save(dst); n += 1
print(n, 'pictures; 40->%d 100->%d 180->%d 255->%d' % (lut[40], lut[100], lut[180], lut[255]))
