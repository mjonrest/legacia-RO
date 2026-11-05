# Build Legacia aura effects (STR + textures) from Gepard aura textures.
import os, sys, math, struct, json
from PIL import Image

SRC, OUT = sys.argv[1], sys.argv[2]
FOLDER = 'efst_legacia_aura'
EFF = os.path.join(OUT, 'data', 'texture', 'effect', FOLDER)
os.makedirs(EFF, exist_ok=True)
FPS = 60
SRC_BLEND, DST_BLEND = 5, 7        # same blend as official effects (additive look on black)
UV = (0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 1.0)
ZERO8 = (0.0,) * 8

def pow2(n):
    p = 32
    while p < n and p < 128: p *= 2
    return p

def make_texture(path, name):
    src = Image.open(path).convert('RGBA')
    im = Image.new('RGB', src.size, (0, 0, 0)); im.paste(src, mask=src.getchannel('A'))   # alpha -> black
    px = im.load()
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            r, g, b = px[x, y]
            if (r > 240 and g < 15 and b > 240) or max(r, g, b) <= 36:
                px[x, y] = (0, 0, 0)   # magenta key / dark grey backdrop -> black (black is invisible)
    w, h = im.size; s = max(w, h)
    sq = Image.new('RGB', (s, s), (0, 0, 0)); sq.paste(im, ((s - w) // 2, (s - h) // 2))
    t = pow2(s)
    sq = sq.resize((t, t), Image.LANCZOS)
    sq.save(os.path.join(EFF, name), 'BMP')
    return t

def key(frame, typ, pos, xy, color, angle=0.0, uv=UV):
    return struct.pack('<iI2f8f8ffIff4f3I', frame, typ, *pos, *uv, *xy,
                       0.0, 0, 0.0, angle, *color, SRC_BLEND if typ == 0 else 0,
                       DST_BLEND if typ == 0 else 0, 0)

def layer(texname, states):
    """states: list of (frame, pos, xy, color). Emits keyframe + morph pairs."""
    out = [struct.pack('<i', 1), texname.encode('latin-1').ljust(128, b'\0')]
    keys = []
    for i, (f, pos, xy, col) in enumerate(states):
        keys.append(key(f, 0, pos, xy, col))
        if i + 1 < len(states):
            f2, pos2, xy2, col2 = states[i + 1]; n = f2 - f
            d = lambda a, b: tuple((bb - aa) / n for aa, bb in zip(a, b))
            keys.append(key(f, 1, d(pos, pos2), d(xy, xy2), d(col, col2), uv=ZERO8))
    out.append(struct.pack('<i', len(keys))); out += keys
    return b''.join(out)

def write_str(name, maxkey, layers):
    data = b'STRM' + struct.pack('<4I', 0x94, FPS, maxkey, len(layers)) + b'\0' * 16 + b''.join(layers)
    open(os.path.join(EFF, name), 'wb').write(data)

def quad(cx, cy, pts):
    xs = tuple(p[0] for p in pts); ys = tuple(p[1] for p in pts)
    return (cx, cy), xs + ys

# ---- ground circle: flat ellipse spinning on the floor ----
G_R, G_SQUASH, G_SPIN, G_STEP = 52.0, 0.62, 6 * FPS, 6
def ground_states(alpha=230.0):
    st = []
    for f in range(0, G_SPIN + 1, G_STEP):
        th = 2 * math.pi * f / G_SPIN
        pts = []
        for cxs, cys in ((-1, -1), (1, -1), (1, 1), (-1, 1)):      # TL TR BR BL
            x = cxs * G_R * math.cos(th) - cys * G_R * math.sin(th)
            z = cxs * G_R * math.sin(th) + cys * G_R * math.cos(th)
            pts.append((x, z * G_SQUASH))
        pos, xy = quad(320.0, 320.0, pts)
        st.append((f, pos, xy, (255.0, 255.0, 255.0, alpha)))
    return st, G_SPIN

# ---- body glow: billboard that breathes ----
B_SIZE, B_PERIOD, B_LIFT = 58.0, 2 * FPS, 40.0   # all auras are anchored at the feet
def glow_states():
    st = []
    for f, sc, a in ((0, 1.0, 200.0), (B_PERIOD // 2, 1.1, 255.0), (B_PERIOD, 1.0, 200.0)):
        h = B_SIZE * sc
        pos, xy = quad(320.0, 320.0 - B_LIFT, ((-h, -h), (h, -h), (h, h), (-h, h)))
        st.append((f, pos, xy, (255.0, 255.0, 255.0, a)))
    return st, B_PERIOD

# ---- small particles: 4 copies orbiting the body ----
P_R, P_SIZE, P_PERIOD, P_STEP = 42.0, 11.0, 4 * FPS, 6
def particle_layers(tex):
    ls = []
    for k in range(4):
        st = []
        for f in range(0, P_PERIOD + 1, P_STEP):
            th = 2 * math.pi * (f / P_PERIOD + k / 4)
            cx = 320 + P_R * math.cos(th); cy = 320 - 45 + P_R * 0.35 * math.sin(th) - 18 * math.sin(2 * th + k)
            pos, xy = quad(cx, cy, ((-P_SIZE, -P_SIZE), (P_SIZE, -P_SIZE), (P_SIZE, P_SIZE), (-P_SIZE, P_SIZE)))
            st.append((f, pos, xy, (255.0, 255.0, 255.0, 235.0)))
        ls.append(layer(tex, st))
    return ls, P_PERIOD

# ---- pillar: the texture wraps around the body like a spinning cylinder ----
C_R, C_H, C_SEGS, C_PERIOD, C_STEP = 30.0, 96.0, 10, 4 * FPS, 10
def pillar_layers(tex):
    ls = []
    for k in range(C_SEGS):
        st = []
        for f in range(0, C_PERIOD + 1, C_STEP):
            a0 = 2 * math.pi * (k / C_SEGS + f / C_PERIOD)
            a1 = a0 + 2 * math.pi / C_SEGS
            x0, x1 = C_R * math.sin(a0), C_R * math.sin(a1)
            y0, y1 = C_R * G_SQUASH * math.cos(a0), C_R * G_SQUASH * math.cos(a1)
            pts = ((x0, y0 - C_H), (x1, y1 - C_H), (x1, y1), (x0, y0))   # TL TR BR BL, bottom on the floor
            pos, xy = quad(320.0, 320.0, pts)
            st.append((f, pos, xy, (255.0, 255.0, 255.0, 190.0)))
        ls.append(layer(tex, st))
    return ls, C_PERIOD

# Gepard aura parts: 1 = ground (1000+n), 2 = pillar (1300+n), 3 = glow (1600+n)
auras = []
for n, f in enumerate(sorted(os.listdir(os.path.join(SRC, '1'))), 1):
    tex = 'g%03d.bmp' % n; make_texture(os.path.join(SRC, '1', f), tex)
    st, mk = ground_states()
    write_str('g%03d.str' % n, mk, [layer(tex, st)])
    auras.append(dict(id=1000 + n, part=1, num=n, kind='ground', file='g%03d.str' % n, src='1/' + f))
for n, f in enumerate(sorted(os.listdir(os.path.join(SRC, '2'))), 1):
    tex = 'p%03d.bmp' % n; make_texture(os.path.join(SRC, '2', f), tex)
    ls, mk = pillar_layers(tex)
    write_str('p%03d.str' % n, mk, ls)
    auras.append(dict(id=1300 + n, part=2, num=n, kind='pillar', file='p%03d.str' % n, src='2/' + f))
for n, f in enumerate(sorted(os.listdir(os.path.join(SRC, '3'))), 1):
    p = os.path.join(SRC, '3', f); w = Image.open(p).size[0]
    tex = 'b%03d.bmp' % n; make_texture(p, tex)
    if w <= 50:
        ls, mk = particle_layers(tex); kind = 'particle'
    else:
        st, mk = glow_states(); ls = [layer(tex, st)]; kind = 'glow'
    write_str('b%03d.str' % n, mk, ls)
    auras.append(dict(id=1600 + n, part=3, num=n, kind=kind, file='b%03d.str' % n, src='3/' + f))
json.dump(auras, open(os.path.join(OUT, 'auras.json'), 'w'), indent=1)
print(len(auras), 'auras')
