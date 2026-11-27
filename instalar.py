"""Racing Club+ : builds the PC version of the game from the user's own copy. No game file is part of this project.

usage: python instalar.py --pkg <game.pkg> <update 1.02.pkg> [<car pack.pkg>] --rap <game .rap> [--out <folder>]
       python instalar.py --juego <extracted NPEB00060 folder> --rap <game .rap> [--out <folder>]

Needs: the EUROPEAN version NPEB00060 of GTI Club+ Rally Cote d'Azur, update 1.02 and (optionally) the car pack, as
the .pkg files bought on PSN, and the game's licence (.rap). No emulator and nothing from the PS3 system software:
the packages are extracted and the executable decrypted here (tools/ps3crypt.py), the music decoder is built in.
Needs on this PC: Python 3 with capstone 5.0.x, and the llvm-mingw + CMake + Ninja toolchain (see LEEME_INSTALADOR.md).

Steps: extract the packages and check the version, decrypt the executable, translate the game code and the SPU
programs to C++, compile, and put the result in the output folder (executable + a copy of the game data).
"""
import argparse, hashlib, os, re, shutil, struct, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tools'))
import ps3crypt

ROOT = os.path.dirname(os.path.abspath(__file__))
TITLE_ID, APP_VER = 'NPEB00060', '01.02'
ELF_SHA1 = 'e9faf331b52770164be0c19f427c10aef747776d'     # decrypted EBOOT of NPEB00060 1.02
EBOOT_SHA1 = 'c68e065fb373c69c18195182fc0cd79635cead39'   # the same, encrypted (USRDIR/EBOOT.BIN)
SPU_SPECS = ['ba3680:0xa200', 'baea80:0x14c00', '0xbd0b00:0xd560:0x3000', '0xbce880:0x2000:0x2f080']


def fail(msg):
    print('\nERROR: ' + msg)
    sys.exit(1)


def step(n, text):
    print('\n[%d/7] %s' % (n, text), flush=True)


def run(cmd, env=None, cwd=ROOT, quiet=False):
    r = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE if quiet else None, stderr=subprocess.STDOUT if quiet else None)
    if r.returncode != 0:
        if quiet and r.stdout: print(r.stdout.decode('utf-8', 'replace')[-3000:])
        fail('ha fallado: ' + ' '.join(str(c) for c in cmd))


def sfo(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\0PSF': fail('PARAM.SFO no valido: ' + path)
    keys, data, n = struct.unpack_from('<III', d, 8)
    out = {}
    for i in range(n):
        ko, fmt, ln, mx, do = struct.unpack_from('<HHIII', d, 20 + 16 * i)
        k = d[keys + ko: d.index(b'\0', keys + ko)].decode()
        v = d[data + do: data + do + ln]
        out[k] = struct.unpack('<I', v[:4])[0] if fmt == 0x0404 else v.rstrip(b'\0').decode('utf-8', 'replace')
    return out


def compile_game(a, tc, elf):
    """Steps 3-6: the game's code translated to C++ and compiled with this version's runtime. Returns the build folder."""
    py, re_dir = sys.executable, os.path.dirname(elf)
    step(3, 'Analizando el codigo del juego')
    run([py, os.path.join('tools', 'prepare_re.py'), elf, re_dir])
    run([py, os.path.join('tools', 'jumptables.py')], quiet=True)

    step(4, 'Traduciendo el codigo del juego a C++ (unos minutos)')
    full = os.path.join(ROOT, 'src', 'recomp', 'full')
    os.makedirs(full, exist_ok=True)
    run([py, os.path.join('tools', 'ppc2cpp.py'), os.path.join('src', 'recomp', 'full', 'game.cpp'), '10000-bb3000'], env=dict(os.environ, SPLIT='48'), quiet=True)

    step(5, 'Traduciendo los programas de sonido y geometria')
    specs = list(SPU_SPECS)
    run([py, os.path.join('tools', 'spu2cpp.py'), elf, os.path.join('src', 'recomp', 'spu_gen')] + specs, quiet=True)

    step(6, 'Compilando (varios minutos; usa %d hilos)' % a.jobs)
    env = dict(os.environ)
    env['PATH'] = os.path.join(tc, 'bin') + os.pathsep + env.get('PATH', '')
    build = os.path.join(ROOT, 'build-inst')
    cmake = os.path.join(tc, 'bin', 'cmake.exe')   # full path: Windows looks the program up in our own PATH, not env's
    if not os.path.exists(cmake): fail('falta cmake.exe en ' + os.path.join(tc, 'bin'))
    tool = lambda n: os.path.join(tc, 'bin', n).replace('\\', '/')
    run([cmake, '-S', ROOT, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=' + tool('clang.exe'), '-DCMAKE_CXX_COMPILER=' + tool('clang++.exe'), '-DCMAKE_MAKE_PROGRAM=' + tool('ninja.exe'),
         '-DCMAKE_PREFIX_PATH=' + os.path.join(tc, 'deps').replace('\\', '/')], env=env, quiet=True)
    run([cmake, '--build', build, '--target', 'recomp_boot', '-j', str(a.jobs)], env=env)

    return build


def find_toolchain(a):
    tc = a.toolchain or os.path.join(ROOT, 'toolchain')
    if not os.path.isdir(os.path.join(tc, 'bin')): tc = os.path.join(os.path.expanduser('~'), '.local', 'share', 'retcomm', 'toolchains', 'cmake-clang-v1', 'latest')
    if not os.path.exists(os.path.join(tc, 'bin', 'clang++.exe')): fail('no encuentro el compilador (carpeta toolchain/ o --toolchain). Mira LEEME_INSTALADOR.md')
    try:
        sys.path[:] = [p for p in sys.path if os.path.abspath(p or '.') != os.path.join(ROOT, 'tools')]
        import capstone
    except ImportError:
        fail('falta el modulo capstone de Python:  python -m pip install capstone==5.0.7')
    return tc


def clean_intermediate():
    # the decrypted executable and the translated code stay out of the installer's folder, so it can be shared;
    # never in a development checkout (.git), where re/ also holds analysis files that cannot be regenerated
    if not os.path.exists(os.path.join(ROOT, '.git')):
        for d in ('re', os.path.join('src', 'recomp', 'full'), os.path.join('src', 'recomp', 'spu_gen'), 'build-inst', 'paquetes'):
            shutil.rmtree(os.path.join(ROOT, d), ignore_errors=True)


def update(a, t0):
    """An installed game rebuilt with this version: the decrypted executable the first install left in
    <folder>/juego/EBOOT.elf is used again, the game data and gtiport.ini (options, language) stay as they are."""
    out = os.path.abspath(a.actualizar)
    step(1, 'Comprobando el juego instalado en ' + out)
    src = os.path.join(out, 'juego', 'EBOOT.elf')
    if not os.path.exists(src): fail('en %s no hay un juego instalado (falta juego\\EBOOT.elf). Instalalo primero.' % out)
    if hashlib.sha1(open(src, 'rb').read()).hexdigest() != ELF_SHA1: fail('el juego de %s no es el de %s %s' % (out, TITLE_ID, APP_VER))
    print('  juego correcto')
    tc = find_toolchain(a)
    step(2, 'Preparando el ejecutable del juego')
    re_dir = os.path.join(ROOT, 're')
    os.makedirs(re_dir, exist_ok=True)
    elf = os.path.join(re_dir, 'EBOOT.elf')
    if os.path.abspath(src) != os.path.abspath(elf): shutil.copyfile(src, elf)
    build = compile_game(a, tc, elf)
    step(7, 'Cambiando el ejecutable del juego')
    exe = os.path.join(out, 'Racing Club+.exe')
    for k in range(30):   # the game may still be closing
        try:
            shutil.copyfile(os.path.join(build, 'recomp_boot.exe'), exe); break
        except PermissionError:
            if k == 29: fail('no se puede cambiar %s: cierra el juego y vuelve a intentarlo' % exe)
            time.sleep(1)
    if not a.keep: clean_intermediate()
    print('\nActualizado en %d min. Para jugar: "%s"' % ((time.time() - t0) / 60, exe))


def main():
    ap = argparse.ArgumentParser(description='Racing Club+ para PC a partir de tu copia del juego (NPEB00060, 1.02)')
    ap.add_argument('--pkg', nargs='*', default=[], help='paquetes .pkg en orden: juego, actualizacion 1.02, pack de coches (opcional)')
    ap.add_argument('--juego', help='carpeta del juego ya extraido de los .pkg: NPEB00060 con USRDIR dentro')
    ap.add_argument('--rap', nargs='*', default=[], help='licencia .rap del juego (descifra el ejecutable)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'Racing Club+'), help='carpeta de salida')
    ap.add_argument('--toolchain', default=os.environ.get('GTI_TOOLCHAIN', ''), help='carpeta del compilador (bin/, deps/)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--no-copy', action='store_true', help='no copiar los datos del juego a la salida')
    ap.add_argument('--keep', action='store_true', help='conservar los ficheros intermedios (re/, codigo traducido, build-inst/) para recompilar mas rapido')
    ap.add_argument('--actualizar', metavar='CARPETA', help='actualizar un juego ya instalado en CARPETA: recompila con esta version sin pedir otra vez los paquetes')
    ap.add_argument('--idioma', type=int, choices=range(1, 6), help='idioma del juego: 1 English, 2 Francais, 3 Espanol, 4 Deutsch, 5 Italiano')
    a = ap.parse_args()
    t0 = time.time()
    if a.actualizar: return update(a, t0)

    step(1, 'Comprobando el juego (hace falta la version europea %s con la actualizacion %s)' % (TITLE_ID, APP_VER))
    if a.pkg:   # base game, update 1.02 on top, car pack: extracted here (the update replaces files of the game)
        work = os.path.join(ROOT, 'paquetes')
        shutil.rmtree(work, ignore_errors=True)
        for pk in a.pkg:
            pk = os.path.abspath(pk)
            if not os.path.exists(pk): fail('no encuentro ' + pk)
            try:
                p = ps3crypt.Pkg(pk)
            except (ValueError, OSError) as e:
                fail('%s no es un paquete de PS3 valido (%s)' % (os.path.basename(pk), e))
            if TITLE_ID not in p.content_id: fail('%s no es de %s (version europea): %s' % (os.path.basename(pk), TITLE_ID, p.content_id))
            print('  extrayendo %s (%d ficheros)' % (os.path.basename(pk), p.file_count), flush=True)
            game = p.extract(work)
    elif a.juego:   # the game extracted from its .pkg files: base game + update 1.02 + optional car pack
        game = os.path.abspath(a.juego)
        if os.path.basename(game).upper() == 'USRDIR': game = os.path.dirname(game)
        if not os.path.exists(os.path.join(game, 'USRDIR', 'EBOOT.BIN')):
            fail('en %s no esta el juego extraido (hace falta la carpeta %s con USRDIR\\EBOOT.BIN dentro)' % (game, TITLE_ID))
    else:
        fail('indica los paquetes del juego (--pkg) o la carpeta del juego extraido (--juego)')
    if os.path.exists(os.path.join(game, 'PARAM.SFO')):
        info = sfo(os.path.join(game, 'PARAM.SFO'))
        ver = info.get('APP_VER')
        print('  %s  %s  version %s' % (info.get('TITLE_ID', '?'), info.get('TITLE', '?').replace('\n', ' '), ver or '?'))
        if info.get('TITLE_ID') != TITLE_ID: fail('el juego no es %s (version europea). Otras regiones no sirven.' % TITLE_ID)
        if ver and ver != APP_VER: fail('hace falta la actualizacion %s del juego (encontrada: %s).' % (APP_VER, ver))
    usrdir = os.path.join(game, 'USRDIR')
    # the executable itself tells the exact version (an extracted base game keeps the PARAM.SFO without APP_VER)
    if hashlib.sha1(open(os.path.join(usrdir, 'EBOOT.BIN'), 'rb').read()).hexdigest() != EBOOT_SHA1:
        fail('el EBOOT.BIN no es el de %s con la actualizacion %s. Si has extraido los .pkg, extrae tambien la actualizacion encima del juego.' % (TITLE_ID, APP_VER))
    print('  ejecutable del juego: version %s correcta' % APP_VER)
    if not os.path.isdir(os.path.join(usrdir, 'data', 'cars', 'pack_modern_1')): print('  aviso: no veo el pack de coches (DLC); el juego funciona sin el')
    tc = find_toolchain(a)

    step(2, 'Descifrando el ejecutable del juego')
    re_dir = os.path.join(ROOT, 're')
    os.makedirs(re_dir, exist_ok=True)
    elf = os.path.join(re_dir, 'EBOOT.elf')
    if not (os.path.exists(elf) and hashlib.sha1(open(elf, 'rb').read()).hexdigest() == ELF_SHA1):
        raps = sorted([r for r in a.rap if r], key=lambda r: 'CARPACK' in os.path.basename(r).upper())   # the game's licence first
        if not raps and a.pkg:   # not given: the one bought with the game, next to its .pkg
            d = os.path.dirname(os.path.abspath(a.pkg[0]))
            for base in (d, os.path.dirname(d)):
                for root, dirs, files in os.walk(base):
                    if root[len(base):].count(os.sep) >= 2: dirs[:] = []
                    raps += [os.path.join(root, f) for f in files if f.lower().endswith('.rap') and 'NPEB00060_00-GTICLUB1' in f.upper()]
                if raps: break
        if not raps: fail('falta la licencia .rap del juego (EP0101-NPEB00060_00-GTICLUB...rap)')
        data, err = None, ''
        for r in raps:
            try:
                data = ps3crypt.self_to_elf(os.path.join(usrdir, 'EBOOT.BIN'), open(r, 'rb').read())
                if hashlib.sha1(data).hexdigest() == ELF_SHA1: break
            except (ValueError, OSError, Exception) as e:
                err = str(e); data = None
        if data is None: fail('no se ha podido descifrar el juego con la licencia indicada: %s. Hace falta la licencia .rap del juego.' % err)
        open(elf, 'wb').write(data)
    if hashlib.sha1(open(elf, 'rb').read()).hexdigest() != ELF_SHA1:
        fail('el ejecutable descifrado no es el de %s %s; esta version no esta soportada' % (TITLE_ID, APP_VER))
    print('  ejecutable correcto')

    build = compile_game(a, tc, elf)

    step(7, 'Preparando la carpeta del juego')
    out = os.path.abspath(a.out)
    os.makedirs(os.path.join(out, 'juego'), exist_ok=True)
    shutil.copyfile(os.path.join(build, 'recomp_boot.exe'), os.path.join(out, 'Racing Club+.exe'))
    shutil.copyfile(elf, os.path.join(out, 'juego', 'EBOOT.elf'))
    if not a.no_copy:
        print('  copiando los datos del juego...', flush=True)
        shutil.copytree(usrdir, os.path.join(out, 'juego', 'USRDIR'), dirs_exist_ok=True, ignore=shutil.ignore_patterns('EBOOT.BIN'))
        if os.path.isdir(os.path.join(game, 'TROPDIR')):   # the trophy list (names, descriptions, icons)
            shutil.copytree(os.path.join(game, 'TROPDIR'), os.path.join(out, 'juego', 'TROPDIR'), dirs_exist_ok=True)
    if not a.keep: clean_intermediate()
    if a.idioma:   # the game's language, in the settings file the game reads next to its executable
        ini = os.path.join(out, 'gtiport.ini')
        lines = [l for l in (open(ini, encoding='utf-8').read().splitlines() if os.path.exists(ini) else []) if not l.startswith('idioma=')]
        open(ini, 'w', encoding='utf-8').write('\n'.join(lines + ['idioma=%d' % a.idioma]) + '\n')
    print('\nHecho en %d min. Para jugar: "%s"' % ((time.time() - t0) / 60, os.path.join(out, 'Racing Club+.exe')))
    print('Esa carpeta contiene tu copia del juego: es para tu uso, no la repartas.')


if __name__ == '__main__':
    main()
