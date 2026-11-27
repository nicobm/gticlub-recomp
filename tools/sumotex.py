"""Textures of the Sumo Tool screens (data/sumotool/ps3/<screen>/<screen>[_lang].dat + .gpu).

The .dat is a chunk file (tag, size); PTEX holds a header (sprites count at +0x10, textures count at +0x14), the
sprite rectangles, then per texture a (struct pointer, offset in the .gpu) pair, then CellGcmTexture structs of
0x18 bytes: format u8, mips u8, dimension u8, cube u8, remap u32, width u16, height u16, depth u16, location u8,
pad u8, pitch u32, offset u32.
"""
import struct, sys, os

def chunks(d):
    o = 0
    while o + 8 <= len(d):
        tag = d[o:o + 4]; size = struct.unpack('>I', d[o + 4:o + 8])[0]
        if size < 8: break
        yield tag, o, size
        o += size

def textures(dat_path):
    d = open(dat_path, 'rb').read()
    out = []
    for tag, o, size in chunks(d):
        if tag != b'PTEX': continue
        c = d[o:o + size]
        nspr, ntex = struct.unpack('>II', c[0x10:0x18])
        # the structs: the last ntex * 0x18 bytes that look like CellGcmTexture, found by scanning for the pair table
        for fs in range(0x18, len(c) - 0x18 * ntex + 1, 4):
            ok = True
            for k in range(ntex):
                s = fs + 0x18 * k
                fmt, mips, dim = c[s], c[s + 1], c[s + 2]
                w, h = struct.unpack('>HH', c[s + 8:s + 12])
                if not (fmt & 0x80 and 1 <= mips <= 13 and dim == 2 and 0 < w <= 4096 and 0 < h <= 4096): ok = False; break
            if not ok or fs < 8 * ntex: continue
            for k in range(ntex):
                s = fs + 0x18 * k
                fmt, mips = c[s], c[s + 1]
                remap = struct.unpack('>I', c[s + 4:s + 8])[0]
                w, h = struct.unpack('>HH', c[s + 8:s + 12])
                pitch = struct.unpack('>I', c[s + 16:s + 20])[0]
                off = struct.unpack('>I', c[fs - 8 * (ntex - k) + 4:fs - 8 * (ntex - k) + 8])[0]
                out.append(dict(format=fmt, mips=mips, remap=remap, width=w, height=h, pitch=pitch, gpu_offset=off))
            break
    return out

if __name__ == '__main__':
    root = sys.argv[1]
    for scr in sorted(os.listdir(root)):
        for f in sorted(os.listdir(os.path.join(root, scr))):
            if not f.endswith('.dat') or any(f.endswith(x) for x in ('_en.dat', '_fr.dat', '_ge.dat', '_it.dat')): continue
            for t in textures(os.path.join(root, scr, f)):
                print(scr, f, hex(t['format']), t['width'], t['height'], hex(t['gpu_offset']), hex(t['remap']))
