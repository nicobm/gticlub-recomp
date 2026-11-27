"""Finds the PlayStation button icons in the Sumo Tool screen atlases: small square sprites whose colourful pixels are
blue (cross), red (circle), green (triangle) or pink (square). Prints screen, file, texture, rect and the guess."""
import sys, os, struct, colorsys
from PIL import Image
import gcmtex, sumotex

def sprites(dat):
    d = open(dat, 'rb').read()
    for tag, o, size in sumotex.chunks(d):
        if tag != b'PTEX': continue
        c = d[o:o + size]
        nspr, ntex = struct.unpack('>II', c[0x10:0x18])
        # texture structs: their chunk offsets, to map a sprite's pointer to its texture index
        texs = sumotex.textures(dat)
        out = []
        for k in range(nspr):
            r = c[0x18 + 0x28 * k:0x18 + 0x28 * (k + 1)]
            ptr = struct.unpack('>I', r[0:4])[0]
            xs = struct.unpack('>8f', r[8:40])
            out.append((ptr, xs[0], xs[1], xs[4], xs[5]))
        # pair table (struct pointer, offset) right before the structs: texture index from the pointer
        return out, c
    return [], b''

def classify(im):
    n = {'cross': 0, 'circle': 0, 'triangle': 0, 'square': 0}
    tot = 0
    for p in im.getdata():
        r, g, b, a = p
        if a < 128: continue
        tot += 1
        h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        if s < 0.45 or v < 0.35: continue
        h *= 360
        if 200 <= h <= 250: n['cross'] += 1
        elif h <= 20 or h >= 345: n['circle'] += 1
        elif 90 <= h <= 170: n['triangle'] += 1
        elif 280 <= h < 345: n['square'] += 1
    best = max(n, key=n.get)
    return (best if n[best] >= max(6, tot * 0.06) else None), n, tot

root = sys.argv[1]
for scr in sorted(os.listdir(root)):
    for f in sorted(os.listdir(os.path.join(root, scr))):
        if not f.endswith('.dat'): continue
        dat = os.path.join(root, scr, f)
        texs = sumotex.textures(dat)
        if not texs: continue
        gpu = open(dat[:-4] + '.gpu', 'rb').read()
        imgs = [gcmtex.decode(dict(t, format=t['format'] & ~0x40, data=gpu[t['gpu_offset']:])) if (t['format'] & ~0x60 | 0x80) in (0x85, 0x86, 0x87, 0x88) else None for t in texs]
        spr, c = sprites(dat)
        ptrs = sorted({p for p, *_ in spr})
        for p, x0, y0, x1, y1 in spr:
            w, h = x1 - x0, y1 - y0
            if not (8 <= w <= 48 and 8 <= h <= 48 and 0.7 <= w / h <= 1.4): continue
            for ti, im in enumerate(imgs):
                if im is None or x1 > im.width or y1 > im.height: continue
                kind, n, tot = classify(im.crop((int(x0), int(y0), int(x1), int(y1))))
                if kind: print(scr, f, 'tex', ti, 'ptr', hex(p), 'rect', int(x0), int(y0), int(w), int(h), kind, n)
