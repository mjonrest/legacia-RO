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

def make_slices(path, prefix, n, size=(16, 64)):
    """Cut a strip texture into n vertical slices so it wraps once around a ring."""
    src = Image.open(path).convert('RGBA')
    im = Image.new('RGB', src.size, (0, 0, 0)); im.paste(src, mask=src.getchannel('A'))
    px = im.load()
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            r, g, b = px[x, y]
            if (r > 240 and g < 15 and b > 240) or max(r, g, b) <= 36:
                px[x, y] = (0, 0, 0)
    im = im.resize((size[0] * n, size[1]), Image.LANCZOS)
    names = []
    for k in range(n):
        name = '%s_%02d.bmp' % (prefix, k)
        im.crop((k * size[0], 0, (k + 1) * size[0], size[1])).save(os.path.join(EFF, name), 'BMP')
        names.append(name)
    return names

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

# All parts are anchored at the feet (hatEffectPos -11). Screen y grows downwards.
# Matched to a recording of the Gepard aura (2026-11-05).
SQUASH = 0.62      # floor ellipse: screen height / width
LIFT = 0.80        # vertical world units -> screen pixels

def project(x, y, z):
    """world point (x right, y up, z towards camera) -> screen offset"""
    return (x, z * SQUASH - y * LIFT)

# ---- part 1: ground circle spinning flat on the floor ----
G_R, G_SPIN, G_STEP = 62.0, 6 * FPS, 6
def ground_states(alpha=230.0):
    st = []
    for f in range(0, G_SPIN + 1, G_STEP):
        th = 2 * math.pi * f / G_SPIN
        pts = []
        for cxs, cys in ((-1, -1), (1, -1), (1, 1), (-1, 1)):      # TL TR BR BL
            x = cxs * G_R * math.cos(th) - cys * G_R * math.sin(th)
            z = cxs * G_R * math.sin(th) + cys * G_R * math.cos(th)
            pts.append(project(x, 0, z))
        pos, xy = quad(320.0, 320.0, pts)
        st.append((f, pos, xy, (255.0, 255.0, 255.0, alpha)))
    return st, G_SPIN

# ---- part 2: tilted rings around the body; the strip runs along each ring ----
R_PERIOD, R_STEP, R_SEGS, R_BAND, R_CENTER = 4 * FPS, 20, 12, 12.0, 30.0
# (base radius, radius pulse, tilt about x (deg), tilt swing, tilt about z (deg), spin turns per loop, phase)
RINGS = ((56.0, 12.0, 0.0, 16.0, 0.0, 1, 0.0), (46.0, 10.0, -6.0, 12.0, 28.0, -1, 0.5))
def rot(p, ax, az):
    x, y, z = p
    y, z = y * math.cos(ax) - z * math.sin(ax), y * math.sin(ax) + z * math.cos(ax)
    x, y = x * math.cos(az) - y * math.sin(az), x * math.sin(az) + y * math.cos(az)
    return x, y, z
def ring_layers(slices):
    ls = []
    for r0, rp, tx, ts, tz, turns, ph in RINGS:
        for k in range(R_SEGS):
            st = []
            for f in range(0, R_PERIOD + 1, R_STEP):
                t = f / R_PERIOD
                r = r0 + rp * math.sin(2 * math.pi * (t + ph))
                ax = math.radians(tx + ts * math.sin(2 * math.pi * (t + ph)))
                az = math.radians(tz)
                a0 = 2 * math.pi * (k / R_SEGS + turns * t)
                a1 = a0 + 2 * math.pi / R_SEGS
                pts = []
                for rr, aa in ((r + R_BAND, a0), (r + R_BAND, a1), (r - R_BAND, a1), (r - R_BAND, a0)):  # TL TR BR BL
                    x, y, z = rot((rr * math.cos(aa), 0.0, rr * math.sin(aa)), ax, az)
                    pts.append(project(x, y + R_CENTER, z))
                pos, xy = quad(320.0, 320.0, pts)
                st.append((f, pos, xy, (255.0, 255.0, 255.0, 210.0)))
            ls.append(layer(slices[k], st))
    return ls, R_PERIOD

# ---- part 3: small sprites rising from the floor around the player ----
S_PERIOD, S_COUNT, S_LIFE, S_AREA, S_RISE = 4 * FPS, 10, 150, 66.0, 60.0
def sprite_layers(tex, size, seed):
    import random
    rnd = random.Random(seed)
    ls = []
    for k in range(S_COUNT):
        a = rnd.uniform(0, 2 * math.pi); d = S_AREA * math.sqrt(rnd.uniform(0.05, 1.0))
        x, z = d * math.cos(a), d * math.sin(a)
        rise = S_RISE * rnd.uniform(0.7, 1.2); drift = rnd.uniform(-10, 10)
        sz = size * rnd.uniform(0.75, 1.15)
        start = (k * S_PERIOD // S_COUNT + rnd.randint(0, 8)) % S_PERIOD
        def at(u):   # u = 0..1 through the life
            sx, sy = project(x + drift * u, rise * u, z)
            alpha = 255.0 * min(1.0, u / 0.2) * (1.0 - max(0.0, (u - 0.55) / 0.45))
            h = sz * (1.0 - 0.3 * u)
            pos, xy = quad(320.0 + sx, 320.0 + sy, ((-h, -h), (h, -h), (h, h), (-h, h)))
            return pos, xy, (255.0, 255.0, 255.0, alpha)
        marks = [0.0, 0.2, 0.55, 1.0]
        samples = [(start + u * S_LIFE, u) for u in marks]
        if start + S_LIFE > S_PERIOD:          # life crosses the loop end: split it
            ub = (S_PERIOD - start) / S_LIFE
            samples = [(fr, u) for fr, u in samples if fr < S_PERIOD] + [(S_PERIOD, ub)] + \
                      [(0, ub)] + [(fr - S_PERIOD, u) for fr, u in samples if fr > S_PERIOD]
        samples.sort(key=lambda s: (s[0], s[1] if s[0] == 0 else 0))
        st, last = [], -1
        for fr, u in samples:
            fr = int(round(fr))
            if fr <= last: fr = last + 1
            if fr > S_PERIOD: fr = S_PERIOD
            st.append((fr, *at(u))); last = fr
        ls.append(layer(tex, st))
    return ls, S_PERIOD

# Gepard aura parts: 1 = ground (1000+n), 2 = rings (1300+n), 3 = rising sprites (1600+n)
auras = []
for n, f in enumerate(sorted(os.listdir(os.path.join(SRC, '1'))), 1):
    tex = 'g%03d.bmp' % n; make_texture(os.path.join(SRC, '1', f), tex)
    st, mk = ground_states()
    write_str('g%03d.str' % n, mk, [layer(tex, st)])
    auras.append(dict(id=1000 + n, part=1, num=n, kind='ground', file='g%03d.str' % n, src='1/' + f))
for n, f in enumerate(sorted(os.listdir(os.path.join(SRC, '2'))), 1):
    slices = make_slices(os.path.join(SRC, '2', f), 'p%03d' % n, R_SEGS)
    ls, mk = ring_layers(slices)
    write_str('p%03d.str' % n, mk, ls)
    auras.append(dict(id=1300 + n, part=2, num=n, kind='rings', file='p%03d.str' % n, src='2/' + f))
for n, f in enumerate(sorted(os.listdir(os.path.join(SRC, '3'))), 1):
    p = os.path.join(SRC, '3', f); w = Image.open(p).size[0]
    tex = 'b%03d.bmp' % n; make_texture(p, tex)
    ls, mk = sprite_layers(tex, 8.0 if w <= 50 else 12.0, n)
    write_str('b%03d.str' % n, mk, ls)
    auras.append(dict(id=1600 + n, part=3, num=n, kind='sprites', file='b%03d.str' % n, src='3/' + f))
json.dump(auras, open(os.path.join(OUT, 'auras.json'), 'w'), indent=1)
print(len(auras), 'auras')
