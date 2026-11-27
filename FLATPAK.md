# Racing Club+ como Flatpak

`build-game-flatpak.sh` empaqueta **Racing Club+ ya compilado** en un Flatpak
que lo ejecuta con **Wine**. No compila el juego, no usa RPCS3 y no necesita
el toolchain de mingw.

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
- `flatpak` y `flatpak-builder` en el host. En Fedora Atomic el script entra
  (o crea) el contenedor Toolbox `gticlub-recomp` y los instala dentro con
  `sudo dnf`; usá `GTI_TOOLBOX=otro-nombre` para elegir otro contenedor. Sin
  Toolbox (Debian, Arch, Alpine, Fedora normal, ...) no toca el host: imprime
  el comando del gestor de paquetes detectado.
- Los runtimes de **usuario** de Flathub:
  - `org.freedesktop.Platform//26.08`
  - `org.freedesktop.Sdk//26.08` (solo para `flatpak-builder`)
  - `org.winehq.Wine//stable-26.08` (el Wine que ejecuta el juego)

El script los instala en el usuario si faltan; **no pide sudo**.

## Uso

```sh
cd ~/Proyectos/gti
bash build-game-flatpak.sh
```

Si el zip no está en Downloads/Documents:

```sh
bash build-game-flatpak.sh "/ruta/Racing Club+ Instalador.zip"
```

o una carpeta ya extraída:

```sh
bash build-game-flatpak.sh "/ruta/Racing Club+"
```

El script también acepta el `.rar`/`.zip` del ROM, en **cualquier orden**:

```sh
bash build-game-flatpak.sh "/ruta/Racing Club+ Instalador.zip" "/ruta/GTI Club...rar"
bash build-game-flatpak.sh "/ruta/GTI Club...rar" "/ruta/Racing Club+ Instalador.zip"
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
5. Construye el Flatpak con `flatpak-builder --user` y lo exporta a `repo/`
   (ignorado por git).
6. Lo instala en tu usuario desde el remoto local `gticlub-local`, separado
   de Flathub, y lo deja desactivado.

El remoto local queda desactivado después de instalar: como la app se
construye localmente, `flatpak update` no tiene nada que descargar. Para
actualizarla, volvé a ejecutar `build-game-flatpak.sh`.

`flatpak-builder` puede separar los símbolos de depuración en
`io.github.nicobm.GtiClubRecomp.Debug`. No hace falta para jugar: el script no
lo instala y quita el que haya dejado un build anterior. Si lo querés para
`gdb`, ejecutá:

```sh
bash build-game-flatpak.sh --debug
```

Al terminar:

```sh
flatpak run --user io.github.nicobm.GtiClubRecomp
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

El `gtiport.ini` del paquete viene en 4K/fullscreen/72 Hz. El script lo ajusta
a **ventana 720p/60** para el primer arranque; después se cambia desde el menú
**F1** del juego. Si ya tenías un `gtiport.ini` en el directorio de datos y no
quieres esperar, borralo para que se copie el nuevo:

```sh
rm -f ~/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club/game/gtiport.ini
```

Las partidas y ajustes se quedan en `racing-club/game`. Para borrarlos:

```sh
rm -rf ~/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club
```

## Permisos del sandbox

- `--share=network`: red (el port todavía no implementa la pila de red; el
  menú online/LAN del juego no conecta).
- `--filesystem=home`: el Flatpak puede ver tu carpeta personal
  (`~/Documentos`, `~/Descargas`, etc.). Es para que un file browser o el
  instalador puedan elegir archivos; el juego prebuilt no lo necesita.
- `--nofilesystem=host`: sin acceso al resto del sistema (`/`, `/usr`, otros
  usuarios, etc.).
- `--socket=wayland`, `--socket=pulseaudio`,
  `--device=dri`, `--device=input`: pantalla, sonido, GPU y mandos.

Si no querés exponer todo el home, se puede cambiar `--filesystem=home` por
`--filesystem=xdg-documents` y `--filesystem=xdg-download`.

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
- El juego usa **OpenGL**, no Vulkan. Wine lo mantiene desactivado con
  `WINEDLLOVERRIDES` (`winevulkan=d;vulkan-1=d`) para que `radv` no llene la
  terminal con `vdrm_device_connect failed`.
- El lanzador usa **OpenGL por hardware** por defecto. En una VM con
  `virtio-gpu`/virgl detecta el dispositivo y aplica
  `MESA_GL_VERSION_OVERRIDE=4.5` (virgl anuncia 4.3 y el juego pide 4.5
  core). Es la única variable de Mesa necesaria. Para forzar software:
  ```sh
  flatpak run --user --env=RACING_CLUB_SOFTWARE_GL=1 \
      io.github.nicobm.GtiClubRecomp
  ```
  No uses `flatpak override`: cambia la configuración para todas las
  ejecuciones. Los `--env` de una sola corrida son suficientes.
- El `glcache` del paquete es de la GPU del autor; el lanzador lo borra **una
  sola vez** y el juego regenera los shaders para tu GPU. Si se borrara en
  cada arranque, se perdería esa caché y todo iría lento. Para forzar el
  borrado: `--env=RACING_CLUB_RESET_GLCACHE=1`. Para conservar incluso el del
  autor: `--env=RACING_CLUB_KEEP_GLCACHE=1`.
- El juego es un `.exe` de Windows: SDL crea el contexto con **WGL**, que Wine
  traduce con su driver **Wayland** nativo. El manifiesto solo expone
  `--socket=wayland`: sin X11 ni XWayland, Wine no puede caer en X11. Si no
  hay sesión Wayland, el lanzador avisa y no arranca.
- El modo virtio/virgl se activa solo si
  `/sys/class/drm/card0/device/uevent` contiene `virtio`; si el sandbox no
  puede leerlo, forzalo con `--env=RACING_CLUB_VIRTIO=1`. Si la VM no tiene
  3D (sin virgl), usá `--env=RACING_CLUB_SOFTWARE_GL=1`: funciona con
  llvmpipe, pero es lento.
- El script ajusta `gtiport.ini` a 720p/60 con `antialiasing=1` y `nitidez=1`.
  Eso fuerza el *present* por shader; con ambos en 0 el juego usa
  `glBlitFramebuffer`, que en AMD/Mesa deja la ventana en blanco. El
  antialiasing anisotrópico queda en 0 para aliviar el software. Después se
  cambia desde el menú **F1**.
- Si después de actualizar el Flatpak seguís viendo todo blanco, borrá el ini
  de tu carpeta de usuario y volvé a arrancar para que se copie el nuevo:
  ```sh
  rm -f ~/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club/game/gtiport.ini
  ```
- Si el host no expone 3D al invitado (por ejemplo, virtio-gpu sin virgl),
  el modo software muestra los logos pero es lento. El rendimiento por
  hardware depende de que el host de la VM tenga virgl o passthrough.
- El Flatpak contiene datos del juego de tu paquete de Patreon. Es para uso
  personal: no redistribuyas el paquete ni `prebuilt/`.
