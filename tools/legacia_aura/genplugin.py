# Textures for the auras legacia.dll draws (src/aura.cpp in tools/legacia-client).
#
#   python3 genplugin.py <gepard auras folder with 1/ 2/ 3/> <out folder>
#
# Writes <out>/plugins/auras/<part>/<nnn>.bmp: 32-bit BMPs, power-of-two sized,
# ready to be drawn additively (black adds nothing). Magenta colour keys and the
# dark grey backdrops some Gepard textures have become black; textures with an
# alpha channel are premultiplied, so their see-through parts become black too.
import os, sys, struct
from PIL import Image

SRC, OUT = sys.argv[1], sys.argv[2]
SIZE = {'1': 256, '2': 64, '3': 64}   # most texels per side, per part (sharper ground circles)


def pow2(n, cap):
    p = 16
    while p < n and p < cap:
        p *= 2
    return p


def convert(path, cap):
    src = Image.open(path)
    has_alpha = src.mode in ('RGBA', 'LA') or (src.mode == 'P' and 'transparency' in src.info)
    src = src.convert('RGBA')
    px = src.load()
    w, h = src.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if not has_alpha:
                a = 255
            if (r > 240 and g < 15 and b > 240) or max(r, g, b) <= 36:
                r = g = b = 0
            else:
                r, g, b = r * a // 255, g * a // 255, b * a // 255
            px[x, y] = (r, g, b, 255)
    s = max(w, h)
    sq = Image.new('RGBA', (s, s), (0, 0, 0, 255))
    sq.paste(src, ((s - w) // 2, (s - h) // 2))
    t = pow2(s, cap)
    return sq.resize((t, t), Image.LANCZOS)


def write_bmp32(im, path):
    w, h = im.size
    rows = []
    data = im.tobytes('raw', 'BGRA')
    for y in range(h - 1, -1, -1):          # bottom-up, like every plain BMP
        rows.append(data[y * w * 4:(y + 1) * w * 4])
    pixels = b''.join(rows)
    head = struct.pack('<2sIHHI', b'BM', 14 + 40 + len(pixels), 0, 0, 14 + 40)
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 32, 0, len(pixels), 2835, 2835, 0, 0)
    with open(path, 'wb') as f:
        f.write(head + info + pixels)


count = 0
for part in '123':
    out = os.path.join(OUT, 'plugins', 'auras', part)
    os.makedirs(out, exist_ok=True)
    for n, name in enumerate(sorted(os.listdir(os.path.join(SRC, part))), 1):
        im = convert(os.path.join(SRC, part, name), SIZE[part])
        write_bmp32(im, os.path.join(out, '%03d.bmp' % n))
        count += 1
print(count, 'textures')
