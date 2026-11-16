"""Custom aura packs -> client files for legacia.dll.

usage: python3 genpacks.py <extracted packs folder> <out folder>

Each pack folder holds effect/aurafloat.tga, auraring.bmp and usually
freezing_circle.bmp, whitelight.tga (the classic "custom aura" pictures for the
official level 99 aura). Packs are numbered by name (case-insensitive) and
written to <out>/data/texture/effect/legacia_aura_pack/NNN/, plus
<out>/packs.txt (number<TAB>name), which the Aura Master NPC list follows.
Pack n is worn with set_aura_pack(n); the server sends it as hat effect 2000+n.
"""
import os, shutil, sys

FILES = ['aurafloat.tga', 'auraring.bmp', 'freezing_circle.bmp', 'whitelight.tga']

src, out = sys.argv[1], sys.argv[2]
packs = sorted((d for d in os.listdir(src) if os.path.isdir(os.path.join(src, d))), key=str.lower)
lines = []
n = 0
for name in packs:
    found = {}
    for root, _, files in os.walk(os.path.join(src, name)):
        for f in files:
            low = f.lower()
            # prefer the copy inside effect\ (some packs keep an alternate next to it)
            if low in FILES and (low not in found or os.path.basename(root).lower() == 'effect'):
                found[low] = os.path.join(root, f)
    if 'auraring.bmp' not in found and 'aurafloat.tga' not in found:
        print('skipped (no aura pictures):', name)
        continue
    n += 1
    dst = os.path.join(out, 'data', 'texture', 'effect', 'legacia_aura_pack', '%03d' % n)
    os.makedirs(dst, exist_ok=True)
    for low, path in found.items():
        shutil.copyfile(path, os.path.join(dst, low))
    lines.append('%d\t%s\t%s' % (n, name, ' '.join(f for f in FILES if f in found)))
with open(os.path.join(out, 'packs.txt'), 'w', encoding='utf-8') as fh:
    fh.write('\n'.join(lines) + '\n')
print(n, 'packs')
