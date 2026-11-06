# Renders aura_host.exe output (polygons the aura code gave the client) to PNG
# frames and a GIF: additive textured strips, drawn in the client's order
# (sorted by the first two corners' rhw), characters as grey placeholders.
#   python3 render_faces.py faces.txt <folder with tex_*.raw> out_prefix
import sys, struct
import numpy as np
from PIL import Image, ImageDraw

faces_path, texdir, out = sys.argv[1], sys.argv[2], sys.argv[3]
W, H = 1024, 768
texs = {}
def tex(i):
    if i not in texs:
        b = open('%s/tex_%03d.raw' % (texdir, i), 'rb').read()
        w, h = struct.unpack('<ii', b[:8])
        a = np.frombuffer(b[8:], dtype='<u4').reshape(h, w)
        rgb = np.stack([(a >> 16) & 255, (a >> 8) & 255, a & 255], -1).astype(np.float32)
        texs[i] = rgb
    return texs[i]

frames, cur = [], None
for line in open(faces_path):
    p = line.split()
    if p[0] == 'frame':
        cur = {'faces': [], 'actors': []}; frames.append(cur)
    elif p[0] == 'actor':
        cur['actors'].append((float(p[2]), float(p[3]), float(p[4])))
    elif p[0] == 'face':
        t = int(p[1]); vals = list(map(float, [x if i % 7 != 4 else str(int(x, 16)) for i, x in enumerate(p[4:])]))
        vs = [vals[i * 7:(i + 1) * 7] for i in range(4)]
        cur['faces'].append((t, vs))

yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
def tri(img, t, a, b, c):
    (x0, y0, u0, v0, g0), (x1, y1, u1, v1, g1), (x2, y2, u2, v2, g2) = a, b, c
    d = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if abs(d) < 1e-6: return
    minx, maxx = int(max(0, min(x0, x1, x2))), int(min(W - 1, max(x0, x1, x2)) + 1)
    miny, maxy = int(max(0, min(y0, y1, y2))), int(min(H - 1, max(y0, y1, y2)) + 1)
    if minx >= maxx or miny >= maxy: return
    X, Y = xx[miny:maxy, minx:maxx], yy[miny:maxy, minx:maxx]
    l1 = ((X - x0) * (y2 - y0) - (x2 - x0) * (Y - y0)) / d
    l2 = ((x1 - x0) * (Y - y0) - (X - x0) * (y1 - y0)) / d
    l0 = 1 - l1 - l2
    m = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
    if not m.any(): return
    tx = tex(t); th, tw = tx.shape[:2]
    u = np.clip(l0 * u0 + l1 * u1 + l2 * u2, 0, 0.9999); v = np.clip(l0 * v0 + l1 * v1 + l2 * v2, 0, 0.9999)
    g = l0 * g0 + l1 * g1 + l2 * g2
    col = tx[(v * th).astype(int), (u * tw).astype(int)] * (g[..., None] / 255.0)
    img[miny:maxy, minx:maxx][m] += col[m]

gif = []
for fi, fr in enumerate(frames):
    img = np.zeros((H, W, 3), np.float32); img[:] = (60, 70, 62)
    items = [(f[1][0][3] + f[1][1][3], 'f', f) for f in fr['faces']] + [(2 * a[2], 'a', a) for a in fr['actors']]
    items.sort(key=lambda it: it[0])
    for _, kind, it in items:
        if kind == 'a':
            x, y, rhw = it; s = rhw * 1100   # screen pixels per world unit there
            pil = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)); d = ImageDraw.Draw(pil)
            d.ellipse([x - 4 * s, y - 22 * s, x + 4 * s, y - 15 * s], fill=(200, 180, 160))
            d.rectangle([x - 4 * s, y - 15 * s, x + 4 * s, y], fill=(90, 90, 140))
            img = np.asarray(pil).astype(np.float32)
        else:
            t, vs = it
            P = [(v[0], v[1], v[5], v[6], (int(v[4]) >> 16) & 255) for v in vs]
            tri(img, t, P[0], P[1], P[2]); tri(img, t, P[2], P[1], P[3])
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    if fi == 0: im.save(out + '.png')
    gif.append(im.crop((262, 184, 762, 534)))
sheet = Image.new('RGB', (4 * 500, 2 * 350))
for k in range(8):
    sheet.paste(gif[k * len(gif) // 8], ((k % 4) * 500, (k // 4) * 350))
sheet.save(out + '_sheet.png')
gif[0].save(out + '.gif', save_all=True, append_images=gif[1:], duration=50, loop=0)
print(len(frames), 'frames')
