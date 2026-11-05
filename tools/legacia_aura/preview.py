# Render Legacia aura STR files the way roBrowser does (keyframe + morph, additive blend).
import struct, sys, os, json
import numpy as np
from PIL import Image, ImageDraw

def load(path):
    d = open(path, 'rb').read(); o = 4
    ver, fps, mk, ln = struct.unpack_from('<4I', d, o); o += 32
    layers = []
    for _ in range(ln):
        tc, = struct.unpack_from('<i', d, o); o += 4
        names = []
        for _ in range(tc): names.append(d[o:o+128].split(b'\0')[0].decode()); o += 128
        kc, = struct.unpack_from('<i', d, o); o += 4
        keys = []
        for _ in range(kc):
            v = struct.unpack_from('<iI2f8f8ffIff4f3I', d, o); o += struct.calcsize('<iI2f8f8ffIff4f3I')
            keys.append(dict(frame=v[0], type=v[1], pos=np.array(v[2:4]), xy=np.array(v[12:20]), color=np.array(v[24:28])))
        layers.append((names, keys))
    return fps, mk, layers

def state(keys, t):
    fr = to = None
    for i, k in enumerate(keys):
        if k['frame'] <= t:
            if k['type'] == 0: fr = i
            else: to = i
    if fr is None: return None
    a = keys[fr]
    if to == fr + 1:
        b = keys[to]; dt = t - a['frame']
        return a['pos'] + b['pos'] * dt, a['xy'] + b['xy'] * dt, a['color'] + b['color'] * dt
    return a['pos'], a['xy'], a['color']

def draw(canvas, tex, pos, xy, color, ox, oy, scale):
    # quad corners TL TR BR BL
    P = [(ox + (pos[0] - 320 + xy[i]) * scale, oy + (pos[1] - 320 + xy[4 + i]) * scale) for i in range(4)]
    tw, th = tex.size
    # affine dest->src from TL, TR, BL
    import numpy.linalg as la
    A = np.array([[P[0][0], P[0][1], 1], [P[1][0], P[1][1], 1], [P[3][0], P[3][1], 1]])
    sx = la.solve(A, [0, tw, 0]); sy = la.solve(A, [0, 0, th])
    H, W = canvas.shape[:2]
    warped = np.asarray(Image.fromarray(tex).transform((W, H), Image.AFFINE, (*sx, *sy), Image.BILINEAR), dtype=np.float32) if False else \
        np.asarray(tex.transform((W, H), Image.AFFINE, (*sx, *sy), Image.BILINEAR), dtype=np.float32)
    canvas += warped * (color[:3] / 255.0) * (color[3] / 255.0)

def render(path, t, W=200, H=200, scale=1.0, oy=None, char=True, ground=True):
    fps, mk, layers = load(path)
    base = os.path.dirname(path)
    canvas = np.zeros((H, W, 3), np.float32); canvas[:] = (58, 66, 54)
    ox = W / 2; oy = oy if oy is not None else H * 0.62
    for names, keys in layers:
        s = state(keys, t % mk)
        if s is None: continue
        tex = Image.open(os.path.join(base, names[0])).convert('RGB')
        draw(canvas, tex, *s, ox, oy, scale)
    im = Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8))
    if char:  # simple stand-in character so size can be judged
        d = ImageDraw.Draw(im); cx, cy = ox, oy
        d.ellipse((cx - 9 * scale, cy - 78 * scale, cx + 9 * scale, cy - 60 * scale), fill=(235, 205, 170))
        d.rounded_rectangle((cx - 12 * scale, cy - 60 * scale, cx + 12 * scale, cy - 20 * scale), 4, fill=(70, 90, 160))
        d.rectangle((cx - 10 * scale, cy - 20 * scale, cx - 3 * scale, cy), fill=(60, 50, 40))
        d.rectangle((cx + 3 * scale, cy - 20 * scale, cx + 10 * scale, cy), fill=(60, 50, 40))
    return im

if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'gif':
        path, out = sys.argv[2], sys.argv[3]
        fps, mk, _ = load(path)
        frames = [render(path, t) for t in range(0, mk, 4)]
        frames[0].save(out, save_all=True, append_images=frames[1:], duration=int(4000 / fps), loop=0)
