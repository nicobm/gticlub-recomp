# Racing Club+ como Flatpak

`build-gti.sh` empaqueta **Racing Club+ ya compilado** en un Flatpak que lo
ejecuta con **Wine**. No compila el juego, no usa RPCS3 y no necesita el
toolchain de mingw.

El paquete de Patreon incluye:

```
Racing Club+/Racing Club+.exe
Racing Club+/juego/EBOOT.elf
Racing Club+/juego/USRDIR/data
Racing Club+/juego/USRDIR/shadercache
Racing Club+/dev_flash/...
Racing Club+/dev_hdd0/...
Racing Club+/glcache/...
Racing Club+/gtiport.ini
```

El script extrae solo esa carpeta del zip, verifica el `EBOOT.elf` oficial
(SHA1 `e9faf331...`) y la empaqueta. `Racing Club+.exe` es el juego completo
(menú, carreras, opciones).

## Requisitos

- El zip del paquete de Patreon (`Racing Club+ Instalador.zip`) en XDG
  Downloads o Documents, o pasarle la ruta. El `.rar`/`.zip` del ROM es
  opcional; se acepta e ignora si el paquete ya trae el juego compilado.
- `flatpak` en el host.
- Toolbox con `flatpak-builder` (el script lo instala con `sudo dnf` si falta).
- Los runtimes de **usuario** de Flathub:
  - `org.freedesktop.Platform//26.08`
  - `org.freedesktop.Sdk//26.08` (solo para `flatpak-builder`)
  - `org.winehq.Wine//stable-26.08` (el Wine que ejecuta el juego)

El script los instala en el usuario si faltan; **no pide sudo**.

## Uso

```sh
cd ~/Proyectos/gti
bash build-gti.sh
```

Si el zip no está en Downloads/Documents:

```sh
bash build-gti.sh "/ruta/Racing Club+ Instalador.zip"
```

o una carpeta ya extraída:

```sh
bash build-gti.sh "/ruta/Racing Club+"
```

El script también acepta el `.rar`/`.zip` del ROM, en **cualquier orden**:

```sh
bash build-gti.sh "/ruta/Racing Club+ Instalador.zip" "/ruta/GTI Club...rar"
bash build-gti.sh "/ruta/GTI Club...rar" "/ruta/Racing Club+ Instalador.zip"
```

El ROM es **opcional**: el paquete de Patreon ya trae el juego compilado y los
datos. El script lo detecta, lo muestra y lo ignora si el paquete ya tiene
`Racing Club+/Racing Club+.exe`.

Si no pasás nada, busca en XDG Downloads/Documents:

- el zip `Racing Club+*.zip` (paquete de Patreon);
- el `.rar`/`.zip` del ROM (`NPEB00060` o `GTI Club`).

El script:

1. Clasifica los argumentos (paquete de Patreon / ROM) en cualquier orden.
2. Extrae `Racing Club+/` del zip a `prebuilt/` (ignorado por git) si hace
   falta.
3. Verifica `Racing Club+.exe`, `juego/EBOOT.elf` (SHA1 oficial), `data` y
   `shadercache`.
4. Comprueba/instala los runtimes de usuario de Flathub (sin sudo).
5. Construye el Flatpak con `flatpak-builder --user` dentro de Toolbox.
6. Lo instala en tu usuario desde el remoto local `gticlub-local`, separado
   de flathub, y lo deja desactivado.

Al terminar:

```sh
flatpak run io.github.nicobm.GtiClubRecomp
```

También aparece en el menú como **Racing Club+**.

## Primera ejecución

El lanzador copia el juego (~440 MB) a:

```
~/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club/game
```

y prepara el prefijo de Wine en:

```
~/.var/app/io.github.nicobm.GtiClubRecomp/data/wineprefix
```

Las partidas y ajustes se quedan en `racing-club/game`. Para borrarlos:

```sh
rm -rf ~/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club
```

## Permisos del sandbox

- `--share=network`: online.
- `--nofilesystem=home` y `--nofilesystem=host`: sin acceso a tu carpeta
  personal ni al resto del sistema.
- `--socket=wayland`, `--socket=fallback-x11`, `--socket=pulseaudio`,
  `--device=dri`, `--device=input`: pantalla, sonido, GPU y mandos.

## Si venís de la versión anterior

La versión anterior instalaba el port `gtiport` en el sistema. Ahora el juego
completo se instala en el **usuario**, sin sudo. Quitá la instalación de
sistema una vez para que `flatpak run` no elija la vieja:

```sh
flatpak uninstall --system io.github.nicobm.GtiClubRecomp
flatpak remote-delete --system --force gticlub-local
```

## Desinstalar

```sh
flatpak uninstall --user io.github.nicobm.GtiClubRecomp
flatpak remote-delete --user --force gticlub-local
rm -rf ~/.var/app/io.github.nicobm.GtiClubRecomp
```

## Notas

- No hace falta instalar `net.rpcs3.RPCS3`: el paquete de Patreon ya trae el
  `EBOOT.elf` oficial y el `Racing Club+.exe` compilado.
- No hace falta mingw, CMake, Ninja, SDL3 ni zlib: el juego ya está compilado.
- El `.rar`/`.zip` del ROM no se usa para este camino: el paquete de Patreon
  ya trae `juego/EBOOT.elf` y `juego/USRDIR`. Se acepta como argumento para
  que puedas pasarlo, pero no se extrae.
- Los mensajes `MESA/radv` de Vulkan son ruido; el juego usa OpenGL y el
  lanzador desactiva `winevulkan`.
- El Flatpak contiene datos del juego de tu paquete de Patreon. Es para uso
  personal: no redistribuyas el paquete ni `prebuilt/`.
