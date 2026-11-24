<p align="center"><img src="assets/banner_racing_club.png" alt="Racing Club+" width="100%"></p>

<p align="center"><a href="https://www.patreon.com/ilesraider/posts/racing-club-175397772"><b>❤️ Apoya el proyecto y descarga el instalador en Patreon / Support the project and download the installer on Patreon</b></a></p>

# GTI Club+ para PC (recompilación nativa)

Recompilación para PC de *GTI Club + Rally Côte d'Azur* (PS3, `NPEB00060`):
el código de la PS3 se traduce a C++ y se ejecuta de forma nativa, con un
renderizador **OpenGL** propio para la RSX y audio emulado en hilos del host.
No usa RPCS3 para jugar.

**El repositorio no incluye ningún archivo del juego.** Necesitás tu propia
copia (`EBOOT.BIN`/`EBOOT.elf` y `USRDIR/data`) o el paquete ya compilado que
el autor publica en Patreon. No redistribuyas `prebuilt/`, `re/`, `capture/`,
`src/recomp/full/` ni los datos extraídos del juego.

## Estado

- **Offline:** jugable (menú, selección de coche, carreras, repeticiones).
- **Online/LAN:** no implementado en esta build. El juego muestra el menú de
  red/PSN, pero el port no tiene pila de red: `socket()` falla y
  `sceNpManagerGetStatus` responde “offline”. Cambiar una IP no conecta nada.
- **Flatpak + Wine:** es la vía recomendada para Linux y la que usa este
  repositorio (`build-gti.sh`). Empaqueta el `Racing Club+.exe` ya compilado.
- **Audio:** el mixer MultiStream corre recompilado en un hilo aparte. El
  canal LFE se omite por defecto desde la revisión de audio (ver
  [Audio](#audio)); hay un parche binario para la build ya instalada.
- **Gráficos:** OpenGL 4.5. En Wayland usa el driver Wayland de Wine y X11
  solo como fallback. En una VM con `virtio-gpu`/virgl el lanzador detecta el
  dispositivo y ajusta Mesa automáticamente.

## Camino rápido (Flatpak, recomendado)

Requisitos:

- Tu paquete de Patreon: `Racing Club+ Instalador.zip`.
- `flatpak` en el host.
- Un Toolbox de Fedora con `flatpak-builder` (el script lo instala con
  `sudo dnf` si falta).
- Runtimes de usuario: `org.freedesktop.Platform//26.08`,
  `org.freedesktop.Sdk//26.08` y `org.winehq.Wine//stable-26.08`.

```sh
cd ~/Proyectos/gti
bash build-gti.sh
flatpak run --user io.github.nicobm.GtiClubRecomp
```

Si el zip no está en `~/Descargas` o `~/Documentos`, pasá la ruta:

```sh
bash build-gti.sh "/ruta/Racing Club+ Instalador.zip"
```

El `.rar`/`.zip` del ROM es opcional y se puede pasar en cualquier orden:

```sh
bash build-gti.sh "/ruta/Racing Club+ Instalador.zip" "/ruta/GTI Club....rar"
```

El script:

1. Clasifica los argumentos (paquete de Patreon / ROM).
2. Extrae `Racing Club+/` a `prebuilt/` (ignorado por git).
3. Verifica `Racing Club+.exe`, `juego/EBOOT.elf` (SHA1 oficial), `data` y
   `shadercache`.
4. Instala/verifica los runtimes de usuario sin sudo.
5. Construye el Flatpak con `flatpak-builder --user` dentro de Toolbox.
6. Lo instala en el usuario desde el remoto local `gticlub-local`.

La primera ejecución copia el juego (~440 MB) a:

```text
~/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club/game
```

y crea el prefijo de Wine en:

```text
~/.var/app/io.github.nicobm.GtiClubRecomp/data/wineprefix
```

Más detalles de permisos, actualización y desinstalación: **[FLATPAK.md](FLATPAK.md)**.

## Compilar desde el código

El juego completo (`Racing Club+.exe`) se compila como el target
`recomp_boot`. Para generarlo hacen falta los fuentes recompilados del
`EBOOT.elf`, que **no están en el repositorio** porque son datos derivados del
juego (`src/recomp/full/`, `src/recomp/gen/`, `src/recomp/spu_gen/`).

La vía oficial es el instalador de Patreon:

- Windows: `Instalar Racing Club+.exe` (o `python instalar.py --rpcs3 ...`),
  con el toolchain incluido en el paquete.
- El instalador descifra el `EBOOT.BIN`, genera el código recompilado, compila
  y deja `Racing Club+.exe` + `juego/` + `dev_flash/`.
- `--keep` conserva `re/`, `src/recomp/full/` y `build-inst/` para recompilar
  más rápido.

Instrucciones del instalador: **[LEEME_INSTALADOR.md](LEEME_INSTALADOR.md)**.

### Targets de CMake

| Target | Qué es |
|---|---|
| `recomp_boot` | El juego completo. Se copia a `Racing Club+.exe`. |
| `gtiport` | Port nativo / visor de modelos (el “Hito 1”). |
| `ep3tool` | Validador de paquetes `.ep3`. |
| `spu_test`, `spu_diff` | Pruebas del intérprete/recompilador de SPU. |
| `recomp_frame`, `recomp_probe`, `recomp_fmt` | Herramientas de depuración del recomp. |
| `instalador` | Lanzador gráfico del instalador de Windows. |

En Windows con el toolchain del paquete:

```bat
build.bat
```

o, si ya tenés los fuentes generados:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
  -DCMAKE_PREFIX_PATH="%USERPROFILE%\.local\share\retcomm\toolchains\cmake-clang-v1\latest\deps"
cmake --build build --target recomp_boot
```

## Ajustes del juego (`gtiport.ini`)

El archivo se lee junto al ejecutable. El script lo deja en **ventana
720p/60** para el primer arranque:

```ini
pantalla_completa=0
resolucion_interna=1
imagenes_por_segundo=60
pantalla_120hz=0
idioma=3
contador_fps=1
vsync=1
sonido=1
antialiasing=1
nitidez=1
modo_directo=0
filtrado_anisotropo=0
```

| Clave | Valores | Notas |
|---|---|---|
| `pantalla_completa` | 0 ventana, 1 borderless, 2 full screen | |
| `resolucion_interna` | 1 = 720p, 2, 3 | múltiplos de 1280x720 |
| `imagenes_por_segundo` | 60, 72, 120, 144 | 60 es lo compatible |
| `pantalla_120hz` | 0/1 | cambia un monitor no múltiplo de 60 a 120 Hz |
| `idioma` | 1..5 | inglés, francés, español, alemán, italiano |
| `contador_fps` | 0/1 | fps en el título de la ventana |
| `vsync` | 0/1 | swap interval |
| `sonido` | 0/1 | |
| `antialiasing` | 0/1 | FXAA del *present* |
| `nitidez` | 0..3 | sharpen del *present* |
| `modo_directo` | 0/1 | streaming: 60 imágenes y pantalla a 120 Hz |
| `filtrado_anisotropo` | 0..4 | 4 = 16x |

**Importante:** con `antialiasing=0` y `nitidez=0` el juego usa
`glBlitFramebuffer` directo, que en algunas AMD/Mesa deja la ventana en
blanco. Dejá al menos uno en 1 para forzar el *present* por shader. El
lanzador lo corrige solo si encuentra ambos en 0.

Después del primer arranque se cambia desde el menú **F1** del juego.

## Gráficos

- El juego pide **OpenGL 4.5 core**; no usa Vulkan.
- **Wayland** es la vía principal. El lanzador quita `DISPLAY` si hay
  `WAYLAND_DISPLAY`, porque Wine prefiere X11 cuando ve ambos.
- **X11** queda como fallback (`--socket=fallback-x11`).
- En una **VM con virtio-gpu/virgl**, el lanzador detecta
  `/sys/class/drm/card0/device/uevent` y aplica:
  - `MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu`
  - `MESA_GL_VERSION_OVERRIDE=4.5`
  - `MESA_GLSL_VERSION_OVERRIDE=450`
- **Software** (lento, pero funciona): `--env=RACING_CLUB_SOFTWARE_GL=1`.
- **Forzar virtio** si `/sys` no es legible: `--env=RACING_CLUB_VIRTIO=1`.
- **Vulkan:** el juego no lo usa; el lanzador deja el loader sin ICDs para
  que `radv` no llene la terminal con `vdrm_device_connect failed`. Para
  volver a habilitarlo: `--env=RACING_CLUB_VULKAN=1`.
- **Caché de shaders:** `glcache/` se borra una sola vez (el del autor es de
  otra GPU). Si se borrara en cada arranque, todo iría lento. Forzar borrado:
  `--env=RACING_CLUB_RESET_GLCACHE=1`. Conservar el del autor:
  `--env=RACING_CLUB_KEEP_GLCACHE=1`.

## Audio

El juego mezcla con **MultiStream**: una tarea SPU recompilada produce bloques
de 256 muestras que el hilo de `cellAudio` copia al ring y el port los manda a
SDL (48 kHz estéreo float).

Variables útiles:

| Variable | Efecto |
|---|---|
| `SPUS=3` o `SPUS=4` | workers de geometría (el lanzador usa `4`; el juego solo necesita 3) |
| `SDL_AUDIO_DEVICE_SAMPLE_FRAMES=1024` / `2048` | buffer del dispositivo (el lanzador usa `1024`) |
| `SNDLFE=0.0` | omite el canal LFE en la mezcla estéreo (por defecto) |
| `SNDGAIN=0.8` | baja la mezcla general antes del limitador |
| `SNDNOLIMIT=1` | desactiva el limitador suave |
| `SNDWAV=1` | escribe `snd_out.f32` (PCM crudo) para diagnosticar |
| `SNDPROF=1` | hotspots del intérprete de SPU cada 10 s |
| `SNDTRACE=1` | traza detallada de la tarea de audio |

### Arreglar el bajo saturado en una build ya instalada

El recomp sumaba el canal 3 (LFE) a ambos parlantes con ganancia 0.5. En
estéreo eso deja el bajo enorme y satura con muchos motores. El código
(`src/recomp/lv2.cpp`) ya lo omite, pero para la build de Patreon que ya está
instalada hay un parche binario que no requiere recompilar:

```sh
python3 patch_lfe.py \
  "$HOME/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club/game/Racing Club+.exe"
```

- Por defecto pone `SNDLFE=0.0` (LFE fuera).
- Guarda `Racing Club+.exe.lfe-backup` la primera vez.
- Si la build no es la esperada, no toca nada.
- Para dejar algo de LFE: `python3 patch_lfe.py --lfe 0.15 ".../Racing Club+.exe"`.
- Para ver qué haría: `--dry-run`.

En este repositorio local, `prebuilt/Racing Club+.exe` ya está parcheado. Si
volvés a extraer el paquete de Patreon, corré `patch_lfe.py` otra vez sobre el
exe extraído (o sobre el instalado) antes de jugar.

## Controles

| Tecla / mando | Acción |
|---|---|
| Enter | START |
| X / Espacio | Cruz |
| Esc | Círculo |
| Flechas | menús |
| A / D | volante |
| W / S | acelerar / frenar |
| F1 | opciones del port (vídeo/audio) |

El mando se detecta por SDL3 (DualShock, Xbox, etc.).

## Solución de problemas

### La ventana queda blanca

- Asegurate de que `gtiport.ini` tenga `antialiasing=1` o `nitidez>=1`.
- Borrá el ini del usuario para que se copie el nuevo:

```sh
rm -f ~/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club/game/gtiport.ini
```

### No abre ventana / `No matching GL pixel format available`

- En Wayland, el lanzador ya prefiere Wayland.
- En una VM, probá `--env=RACING_CLUB_VIRTIO=1`.
- Si el host no expone 3D, usá software: `--env=RACING_CLUB_SOFTWARE_GL=1`.

### `MESA: error: vdrm_device_connect failed` / `radv/amdgpu`

Es ruido de Vulkan (`radv` enumera el virtio-gpu y falla). El juego usa
OpenGL. La build normal ya desactiva esos ICDs; si ves el ruido, actualizá el
Flatpak o poné `--env=MESA_DEBUG=silent`.

### El audio se corta o satura

1. Probá `--env=SPUS=3` y `--env=SDL_AUDIO_DEVICE_SAMPLE_FRAMES=1024`.
2. Aplicá el parche de LFE de arriba.
3. Mirá las líneas `[snd]` del log:

```sh
grep '\[snd\]' \
  ~/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club/game/recomp_log.txt | tail -40
```

### Online / LAN

No está implementado en esta build. El menú puede aparecer, pero no conecta.
Ver [Estado](#estado).

## Estructura del repositorio

| Ruta | Contenido |
|---|---|
| `src/formats/` | Lectores `.ep3`, Edge, texturas RSX/GCM. |
| `src/game/` | Física, vehículos, colisiones del port nativo. |
| `src/render/` | Renderizador OpenGL del port nativo. |
| `src/recomp/` | Capa de sistema PS3 (LV2), SPU, RSX OpenGL, audio, settings. |
| `src/tools/` | `recomp_boot`, `ep3tool`, pruebas y herramientas. |
| `flatpak/` | Lanzador, `.desktop`, icono y metainfo del Flatpak. |
| `tools/*.py` | Ingeniería inversa, exportadores y utilidades. |
| `build-gti.sh` | Empaquetado e instalación del Flatpak. |
| `patch_lfe.py` | Parche binario del LFE para la build compilada. |
| `prebuilt/` | Juego extraído del paquete de Patreon (ignorado por git). |
| `docs/` | Notas de físicas, imports de PS3 y formato de UI. |

## Documentación

- [FLATPAK.md](FLATPAK.md): instalación, permisos, actualización, desinstalación.
- [LEEME_INSTALADOR.md](LEEME_INSTALADOR.md): instalador de Patreon/Windows.
- [NOTAS_NOCHE.md](NOTAS_NOCHE.md): ingeniería inversa, RSX, SPU, rendimiento.
- [docs/fase2_fisicas.md](docs/fase2_fisicas.md): modelo de físicas.
- [docs/fase_recomp_imports.md](docs/fase_recomp_imports.md): imports de PS3.
- [docs/formato_ui.md](docs/formato_ui.md): formato de la UI.

## Créditos y legal

- Recompilación y port: **ilesraider** y colaboradores. Apoyá el proyecto en
  [Patreon](https://www.patreon.com/ilesraider/posts/racing-club-175397772).
- *GTI Club + Rally Côte d'Azur* es de Konami / Sumo Digital. Este proyecto no
  está afiliado ni patrocinado por ellos.
- No se distribuye ningún archivo del juego. Usá tu propia copia legal.
- `prebuilt/`, `re/`, `capture/`, `reference/`, `src/recomp/full/`,
  `src/recomp/gen/` y `src/recomp/spu_gen/` están ignorados por git y no deben
  subirse.
