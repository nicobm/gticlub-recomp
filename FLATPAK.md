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

- `--share=network`: online.
- `--filesystem=home`: el Flatpak puede ver tu carpeta personal
  (`~/Documentos`, `~/Descargas`, etc.). Es para que un file browser o el
  instalador puedan elegir archivos; el juego prebuilt no lo necesita.
- `--nofilesystem=host`: sin acceso al resto del sistema (`/`, `/usr`, otros
  usuarios, etc.).
- `--socket=wayland`, `--socket=fallback-x11`, `--socket=pulseaudio`,
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
- Los mensajes `MESA/radv` de Vulkan son ruido; el juego usa OpenGL y el
  lanzador desactiva `winevulkan`.
- El lanzador usa **OpenGL por software** por defecto (`LIBGL_ALWAYS_SOFTWARE=1`)
  porque en algunas AMD/Mesa el GL por hardware del Flatpak falla. Es lento;
  para probar hardware:
  ```sh
  flatpak run --user --env=RACING_CLUB_SOFTWARE_GL=0 \
      io.github.nicobm.GtiClubRecomp
  ```
  No uses `flatpak override`: cambia la configuración para todas las
  ejecuciones. El `--env` de una sola corrida es suficiente.
- El `glcache` del paquete es de la GPU del autor; el lanzador lo borra **una
  sola vez** y el juego regenera los shaders para tu GPU. Si se borrara en
  cada arranque, se perdería esa caché y todo iría lento. Para forzar el
  borrado: `--env=RACING_CLUB_RESET_GLCACHE=1`. Para conservar incluso el del
  autor: `--env=RACING_CLUB_KEEP_GLCACHE=1`.
- El juego es un `.exe` de Windows: SDL crea el contexto con **WGL**, que Wine
  traduce al driver X11 o al de Wayland. Con el driver Wayland el hardware
  puede fallar con `No matching GL pixel format available`; el lanzador
  prefiere **X11** si el Flatpak lo expone. Para forzar Wayland:
  `--env=RACING_CLUB_VIDEO=wayland`.
- Si el GL por hardware (radeonsi) deja la ventana en blanco o negra, probá
  **zink** (OpenGL sobre Vulkan), que usa otra ruta del driver:
  ```sh
  flatpak run --user \
      --env=RACING_CLUB_SOFTWARE_GL=0 \
      --env=MESA_LOADER_DRIVER_OVERRIDE=zink \
      --env=GALLIUM_DRIVER=zink \
      io.github.nicobm.GtiClubRecomp
  ```
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
- El render por hardware en algunas AMD/Mesa puede seguir mostrando blanco
  aunque el *present* sea por shader: es el renderer RSX del juego, no el
  Flatpak. En ese caso el modo software muestra los logos (lento) hasta que
  se parchee `rsx_gl` y se recompile el juego.
- El Flatpak contiene datos del juego de tu paquete de Patreon. Es para uso
  personal: no redistribuyas el paquete ni `prebuilt/`.
