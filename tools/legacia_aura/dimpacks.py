"""Tone down (and optionally sharpen) the Classic Aura pack pictures.

usage: python3 dimpacks.py <folder with data/> <out folder> <scale> <gamma> [sharp]
Owner 2026-11-17: 0.225 0.6 sharp. out = 255*scale*(in/255)^gamma per channel after an
unsharp mask (radius 1.2, 180%). Pack 000 is skipped; pure magenta (see-through) is kept.
"""
import sys, os, glob
from PIL import Image, ImageFilter
src, out, c, g = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
sharp = len(sys.argv) > 5 and sys.argv[5] == 'sharp'
lut = [min(255, int(round(255 * c * (v / 255) ** g))) for v in range(256)]
n = 0
for f in glob.glob(os.path.join(src, 'data/texture/effect/legacia_aura_pack/*/*')):
    rel = os.path.relpath(f, src)
    if '/000/' in rel:
        continue
    im = Image.open(f); im.load(); mode = im.mode
    im = im.convert('RGBA'); w, h = im.size
    px = im.load()
    key = set()
    for y in range(h):
        for x in range(w):
            r, gg, b, a = px[x, y]
            if r >= 250 and gg <= 5 and b >= 250:
                key.add((x, y)); px[x, y] = (0, 0, 0, a)  # black adds nothing: no pink fringes when sharpening
    if sharp:
        rgb = im.convert('RGB').filter(ImageFilter.UnsharpMask(radius=1.2, percent=180, threshold=2))
        rgb.putalpha(im.getchannel('A')); im = rgb; px = im.load()
    for y in range(h):
        for x in range(w):
            r, gg, b, a = px[x, y]
            px[x, y] = (255, 0, 255, a) if (x, y) in key else (lut[r], lut[gg], lut[b], a)
    dst = os.path.join(out, rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
    (im if mode == 'RGBA' else im.convert('RGB')).save(dst); n += 1
print(n, 'pictures')
