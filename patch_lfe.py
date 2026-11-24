#!/usr/bin/env python3
"""Baja el canal LFE de la mezcla 8->2 de Racing Club+ (parche binario).

El recomp suma el canal 3 (LFE/subwoofer) a ambos parlantes con ganancia 0.5.
En una salida estéreo eso deja el bajo muy fuerte y satura cuando suenan
muchos motores a la vez. Este script cambia esa ganancia en el .exe ya
compilado, sin recompilar:

    python3 patch_lfe.py "Racing Club+.exe"
    python3 patch_lfe.py --lfe 0.0 ".../Racing Club+.exe"
    python3 patch_lfe.py --dry-run ".../Racing Club+.exe"

El valor por defecto es 0.0 (LFE fuera). Se guarda un .lfe-backup la primera
vez. El parche es para la build del paquete de Patreon (Racing Club+.exe de
93.537.280 bytes); si la build cambia, el script avisa y no toca nada.
"""
import argparse
import os
import struct
import sys

# mulss xmm3,xmm8 (LFE * 0.5) + mulss xmm2,xmm9 (* 0.707) + addss xmm2,xmm3
CODE = bytes.fromhex("f3410f59d8" "f3410f59d1" "f30f58d3")
MOVSS_XMM8 = bytes.fromhex("f3440f1005")   # movss xmm8,[rip+disp32]


def pe_sections(data):
    if data[:2] != b"MZ":
        raise SystemExit("no es un .exe (falta MZ)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("no es un PE válido")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    image_base = struct.unpack_from("<Q" if magic == 0x20B else "<I", data, opt + (0x18 if magic == 0x20B else 0x1C))[0]
    secs = []
    sh = opt + opt_size
    for i in range(nsec):
        name = data[sh:sh + 8].rstrip(b"\0").decode("latin1")
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, sh + 8)
        secs.append((name, va, vsize, raw, rawsize))
        sh += 40
    return image_base, secs


def va_to_off(secs, va, image_base):
    rva = va - image_base
    for _name, sva, vsize, raw, rawsize in secs:
        if sva <= rva < sva + max(vsize, rawsize):
            off = raw + (rva - sva)
            return off if off < raw + rawsize else None
    return None


def main():
    ap = argparse.ArgumentParser(description="Baja/ajusta el LFE del .exe de Racing Club+")
    ap.add_argument("exe", help="Racing Club+.exe")
    ap.add_argument("--lfe", type=float, default=0.0,
                    help="ganancia del LFE en la mezcla estéreo (0..1; 0 = omitir, 0.5 = original)")
    ap.add_argument("--dry-run", action="store_true", help="solo mostrar qué haría")
    a = ap.parse_args()
    if not 0.0 <= a.lfe <= 1.0:
        raise SystemExit("--lfe debe estar entre 0 y 1")

    data = bytearray(open(a.exe, "rb").read())
    image_base, secs = pe_sections(data)
    text = next((s for s in secs if s[0] == ".text"), None)
    if not text:
        raise SystemExit("no encuentro la sección .text")

    # La secuencia está en snd::port_thread; se busca en .text.
    tname, tva, tvsize, traw, trawsize = text
    hay = data[traw:traw + trawsize]
    pos = hay.find(CODE)
    if pos < 0:
        raise SystemExit("no encuentro la mezcla 8->2 (build distinta?); no se modifica nada")
    code_va = image_base + tva + pos

    # El multiplicador 0.5 está cargado en xmm8 justo antes; su referencia
    # RIP-relative da la dirección de la constante en .rdata.
    start = max(0, pos - 0x2000)
    mov = hay.rfind(MOVSS_XMM8, start, pos)
    if mov < 0:
        raise SystemExit("no encuentro la carga de la constante 0.5; no se modifica nada")
    mov_va = image_base + tva + mov
    disp = struct.unpack_from("<i", hay, mov + 5)[0]
    const_va = mov_va + 9 + disp
    const_off = va_to_off(secs, const_va, image_base)
    if const_off is None:
        raise SystemExit("la constante 0.5 no cae en ninguna sección; no se modifica nada")

    old = struct.unpack_from("<f", data, const_off)[0]
    if not 0.0 <= old <= 1.0:
        raise SystemExit("la constante encontrada no parece una ganancia (es %r); no se modifica nada" % old)

    print("exe:             %s" % a.exe)
    print("mezcla en:       %#x (archivo %#x)" % (code_va, traw + pos))
    print("constante LFE:   %#x (archivo %#x) = %r" % (const_va, const_off, old))
    print("LFE nuevo:       %r" % a.lfe)
    if abs(old - a.lfe) < 1e-6:
        print("ya está en ese valor; no hay nada que cambiar.")
        return
    if a.dry_run:
        print("dry-run: no se escribe nada.")
        return

    backup = a.exe + ".lfe-backup"
    if not os.path.exists(backup):
        with open(backup, "wb") as f:
            f.write(data)
        print("backup:          %s" % backup)
    struct.pack_into("<f", data, const_off, a.lfe)
    with open(a.exe, "wb") as f:
        f.write(data)
    print("listo. Arrancá el juego; para volver al original, restaurá el backup.")


if __name__ == "__main__":
    main()
