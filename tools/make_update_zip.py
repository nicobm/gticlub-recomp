"""Builds RacingClubPlus-Update-v<version>.zip: the installer folder without toolchain/ (the compiler the players
already have), for the 'Update' button of the installer (instalador.pyw). Upload it as an asset of the GitHub
release tagged v<version>, marked as latest (not prerelease).

    python tools/make_update_zip.py [output folder] [--completo previous-full-installer.zip]

--completo also builds RacingClubPlus-Installer-v<version>.zip: the same files plus the toolchain/ folder (compiler,
Python) copied from an earlier full installer zip, for people installing for the first time.

Contents: the files git tracks, as they are in the folder (minus docs/, NOTAS_NOCHE.md, build.bat, banner) + the installer's icons and
'Instalar Racing Club+.exe' from build-test/. Never any game file.
"""
import os, re, subprocess, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOP = 'Racing Club+ Installer/'
SKIP = re.compile(r'^(docs/|NOTAS_NOCHE\.md$|build\.bat$|assets/banner)')
EXTRA = {'assets/racing_club.ico': 'assets/racing_club.ico', 'assets/racing_club_logo.png': 'assets/racing_club_logo.png',
         'assets/racing_club_icon.png': 'assets/racing_club_icon.png', 'assets/racing_club_cars.png': 'assets/racing_club_cars.png',
         'Instalar Racing Club+.exe': 'build-test/Instalar Racing Club+.exe'}


def main():
    args = sys.argv[1:]
    full = None
    if '--completo' in args:
        i = args.index('--completo'); full = args[i + 1]; del args[i:i + 2]
    sys.argv[1:] = args
    ver = re.search(r"^VERSION = '([^']+)'", open(os.path.join(ROOT, 'instalador.pyw'), encoding='utf-8').read(), re.M).group(1)
    out = os.path.join(sys.argv[1] if len(sys.argv) > 1 else ROOT, 'RacingClubPlus-Update-v%s.zip' % ver)
    files = subprocess.run(['git', 'ls-files'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.splitlines()
    files = sorted(set(f for f in files + ['tools/make_update_zip.py'] if not SKIP.match(f) and os.path.isfile(os.path.join(ROOT, f))))
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in files:
            z.write(os.path.join(ROOT, f), TOP + f)
        for name, src in EXTRA.items():
            if os.path.exists(os.path.join(ROOT, src)): z.write(os.path.join(ROOT, src), TOP + name)
            else: print('aviso: falta', src)
    print('%s: %d ficheros, %.1f MB' % (out, len(files) + len(EXTRA), os.path.getsize(out) / 1e6))
    if full:
        out2 = os.path.join(os.path.dirname(out), 'RacingClubPlus-Installer-v%s.zip' % ver)
        n = 0
        with zipfile.ZipFile(out2, 'w', zipfile.ZIP_DEFLATED) as z2, zipfile.ZipFile(out) as z1, zipfile.ZipFile(full) as zf:
            for it in z1.infolist(): z2.writestr(it, z1.read(it.filename))
            for it in zf.infolist():
                if it.filename.startswith(TOP + 'toolchain/') and not it.is_dir(): z2.writestr(it, zf.read(it.filename)); n += 1
        if not n: print('aviso: el zip %s no tiene toolchain/' % full)
        print('%s: + %d ficheros de toolchain/, %.1f MB' % (out2, n, os.path.getsize(out2) / 1e6))


if __name__ == '__main__':
    main()
