"""Contact sheet of Sumo Tool screen textures: sumotex_sheet.py <screen dir> <out.png> [size] [names...]"""
import sys, os
from PIL import Image
import gcmtex, sumotex

root, outp = sys.argv[1], sys.argv[2]
size = int(sys.argv[3]) if len(sys.argv) > 3 else 384
want = sys.argv[4:]
tiles = []
for f in sorted(os.listdir(root)):
    if not f.endswith('.dat') or (want and not any(w == f[:-4] for w in want)): continue
    gpu = open(os.path.join(root, f[:-4] + '.gpu'), 'rb').read()
    for i, t in enumerate(sumotex.textures(os.path.join(root, f))):
        fmt = t['format'] & ~0x60 | 0x80
        if fmt not in (0x85, 0x86, 0x87, 0x88): print('skip', f, hex(t['format'])); continue
        t2 = dict(t, format=t['format'] & ~0x40, data=gpu[t['gpu_offset']:])
        im = gcmtex.decode(t2)
        bg = Image.new('RGBA', im.size, (70, 40, 90, 255)); bg.alpha_composite(im)
        sc = size / max(im.size)
        tiles.append((f'{f}#{i}', bg.convert('RGB').resize((max(1, int(im.width * sc)), max(1, int(im.height * sc))))))
        print(len(tiles) - 1, f, i, im.size)
cols = 4
out = Image.new('RGB', (cols * (size + 4), ((len(tiles) + cols - 1) // cols) * (size + 4)), (20, 20, 20))
for k, (n, t) in enumerate(tiles): out.paste(t, ((k % cols) * (size + 4), (k // cols) * (size + 4)))
out.save(outp)
