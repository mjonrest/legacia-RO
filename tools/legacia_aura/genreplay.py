#!/usr/bin/env python3
"""Cut a recorded Gepard ring out of a video for legacia.dll to play back.

  ffmpeg -i ring.mp4 -vf fps=30 frames/f%03d.png
  python3 genreplay.py frames OUT/plugins/auras/replay/006 FOOTX FOOTY START PERIOD

frames/f001.png must show the character without the aura (the "floor").
FOOTX/FOOTY: the character's feet in the video (pixels). START/PERIOD: one
loop of the effect, in 30 fps frames (Gepard rings repeat every 173 frames).
Every 2nd frame is kept (15 fps) as a 256x288 32-bit BMP holding only the
light the aura adds; info.txt tells the plugin how to place it. The video's
scale is 2.49 pixels per aura unit (character about 95 units tall).
"""
import glob, os, struct, sys
import numpy as np
from PIL import Image

W, H, UP, PPU = 256, 288, 232, 2.49

def bmp32(path, arr):
    h, w = arr.shape[:2]
    px = np.dstack([arr[..., 2], arr[..., 1], arr[..., 0], arr.max(2)]).astype(np.uint8)[::-1]
    data = px.tobytes()
    hdr = b'BM' + struct.pack('<IHHI', 54 + len(data), 0, 0, 54) + \
        struct.pack('<IiiHHIIiiII', 40, w, h, 1, 32, 0, len(data), 2835, 2835, 0, 0)
    open(path, 'wb').write(hdr + data)

src, out = sys.argv[1], sys.argv[2]
fx, fy, s0, period = (int(a) for a in sys.argv[3:7])
fs = sorted(glob.glob(os.path.join(src, 'f*.png')))
bg = np.asarray(Image.open(fs[0]).convert('RGB'), dtype=float)
os.makedirs(out, exist_ok=True)
n = 0
for j in range(0, period - 1, 2):
    a = np.asarray(Image.open(fs[s0 + j]).convert('RGB'), dtype=float)
    x = np.clip(a - bg, 0, 255)
    x[x.sum(2) < 30] = 0  # video noise
    frame = np.zeros((H, W, 3))
    y0, x0 = fy - UP, fx - W // 2
    for yy in range(H):
        sy = y0 + yy
        if 0 <= sy < x.shape[0]:
            lo, hi = max(0, x0), min(x.shape[1], x0 + W)
            frame[yy, lo - x0:hi - x0] = x[sy, lo:hi]
    bmp32(os.path.join(out, '%03d.bmp' % n), frame)
    n += 1
open(os.path.join(out, 'info.txt'), 'w').write('%d %d %d %d %d %d %.3f\n' % (n, 15, W, H, W // 2, UP, PPU))
print(n, 'frames')
