<p align="center"><img src="assets/banner_racing_club.png" alt="Racing Club+" width="100%"></p>

<p align="center"><a href="https://www.patreon.com/ilesraider/posts/racing-club-175397772"><b>❤️ Patreon: el apoyo se destina al mantenimiento del servidor online. Descarga allí el instalador / Patreon support goes to the upkeep of the online server. Download the installer there</b></a></p>

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
  repositorio (`build-game-flatpak.sh`). Empaqueta el `Racing Club+.exe` ya
  compilado.
- **Audio:** el mixer MultiStream corre recompilado en un hilo aparte. En
  una VM/virgl el límite suele ser el renderizador, no el audio: si los
  frames tardan, el coche y el audio avanzan a tirones (ver
  [Rendimiento](#rendimiento)).
- **Gráficos:** solo Wayland (Wine usa su driver Wayland nativo; el Flatpak
  no expone X11). El lanzador elige el renderizador: **OpenGL** en una VM con
  `virtio-gpu`/virgl y **Vulkan** en una GPU real (con vuelta automática a
  OpenGL si Vulkan no está disponible).

## Camino rápido (Flatpak, recomendado)

Requisitos:

- Tu paquete de Patreon: `Racing Club+ Instalador.zip`.
- `flatpak` y `flatpak-builder` en el host, o Toolbox: si faltan, el script
  entra (o crea) el contenedor `gticlub-recomp` y los instala dentro con
  `sudo dnf`. Sin Toolbox solo imprime el comando del gestor de paquetes.
- Runtimes de usuario: `org.freedesktop.Platform//26.08`,
  `org.freedesktop.Sdk//26.08` y `org.winehq.Wine//stable-26.08`. El script
  los instala en el usuario si faltan.

```sh
cd ~/Proyectos/gti
bash build-game-flatpak.sh
flatpak run --user io.github.nicobm.GtiClubRecomp
```

Si el zip no está en `~/Descargas` o `~/Documentos`, pasá la ruta:

```sh
bash build-game-flatpak.sh "/ruta/Racing Club+ Instalador.zip"
```

El `.rar`/`.zip` del ROM es opcional y se puede pasar en cualquier orden:

```sh
bash build-game-flatpak.sh "/ruta/Racing Club+ Instalador.zip" "/ruta/GTI Club....rar"
```

El script:

1. Clasifica los argumentos (paquete de Patreon / ROM).
2. Extrae `Racing Club+/` a `prebuilt/` (ignorado por git).
3. Verifica `Racing Club+.exe`, `juego/EBOOT.elf` (SHA1 oficial), `data` y
   `shadercache`.
4. Comprueba/instala los runtimes necesarios (Platform, SDK y Wine) sin sudo.
5. Construye el Flatpak con `flatpak-builder --user` y lo exporta a `repo/`.
6. Lo instala en el usuario desde el remoto local `gticlub-local`, separado
   de Flathub, y lo deja desactivado.

`flatpak-builder` puede separar símbolos de depuración en
`io.github.nicobm.GtiClubRecomp.Debug`. No hace falta para jugar: el script no
lo instala y quita el que haya dejado un build anterior. Usá `--debug` si lo
querés para `gdb`.

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

- El port puede dibujar con **OpenGL 4.5**, DirectX 11 o **Vulkan 1.3**. El
  lanzador elige solo, según la GPU:
  - **virtio-gpu/virgl** (VM): **OpenGL**;
  - **GPU real**: **Vulkan** (si no está disponible, el juego vuelve solo a
    OpenGL).
- **Solo Wayland:** el manifiesto no expone X11 ni XWayland y Wine usa su
  driver Wayland nativo. Si no hay sesión Wayland, el lanzador avisa y no
  arranca.
- En una **VM con virtio-gpu/virgl**, además de forzar OpenGL aplica
  `MESA_GL_VERSION_OVERRIDE=4.5` (virgl anuncia 4.3 y el juego pide 4.5
  core) y apaga los ICDs de Vulkan: la VM no tiene Vulkan salvo que se
  habilite Venus en QEMU.
- **Elegir renderizador a mano:** `--env=RENDERER=gl`, `vulkan` o `d3d11`
  (el lanzador no lo pisa; tiene prioridad sobre la opción del menú F1).
- **Software** (lento, pero funciona): `--env=RACING_CLUB_SOFTWARE_GL=1`.
- **Forzar virtio/virgl** si `/sys` no es legible: `--env=RACING_CLUB_VIRTIO=1`.
- **Caché de shaders:** `glcache/` se borra una sola vez (el del autor es de
  otra GPU). Si se borrara en cada arranque, todo iría lento. Forzar borrado:
  `--env=RACING_CLUB_RESET_GLCACHE=1`. Conservar el del autor:
  `--env=RACING_CLUB_KEEP_GLCACHE=1`.

## Rendimiento

El juego avanza su lógica **por imagen**. Si una imagen tarda 50-90 ms, el
coche acelera/frena solo y el audio tironea con él. No es un problema de CPU
(o de RAM): en una VM con `virtio-gpu`/virgl el cuello suele ser el
**renderizador**.

Cómo medirlo:

```sh
flatpak run --user \
  --env=SPUS=4 \
  --env=FPSLOG=1 \
  --env=SWAPLOG=1 \
  io.github.nicobm.GtiClubRecomp
```

y después:

```sh
grep -E '\[fps\]|\[pace\]|\[hitch\]|\[margin\]|\[slowdraw\]|\[rsxprof\]|\[rsx\] surface' \
  ~/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club/game/recomp_log.txt | tail -100
```

Señales de que el renderizador es el límite:

- `[fps]` por debajo de 60 de forma sostenida.
- `[margin] ... the RSX finished up to X ms after it` con X > 16.7 ms.
- `[pace]` con muchas imágenes de más de 25 ms.
- `[hitch] 67 ms without a flip` repetido (4 frames de 60 Hz perdidos).

### Probar 30 fps estables

Si el renderizador no llega a 60, forzar 30 Hz suele dar una experiencia
mucho más pareja: el paso de física se escala (`60/30`) y el coche mantiene
la velocidad correcta.

```sh
flatpak run --user \
  --env=GAMEHZ=30 \
  --env=SPUS=4 \
  io.github.nicobm.GtiClubRecomp
```

No es 60 fps, pero deja de ir “rápido-lento-rápido”. Si con 30 Hz va parejo,
el límite es el renderizador.

### Palancas para acercarse a 60

- **Calidad del juego:** en el menú del juego buscá opciones de calidad,
  sombras y texturas (`SelectGame_Quality_Icon`, `m_uiShadowResolution`,
  `m_uiTextureQualityBias`). Bajarlas reduce mucho el trabajo de virgl.
- **Pantalla:** probá `pantalla_completa=1` (borderless) o `2`, y `vsync=0`
  en `gtiport.ini`.
- **Pacing:** `VBLANKTIMER=1` (timer de 60 Hz en vez del vblank del host),
  `REFRESCO=0` (no cambiar el modo de pantalla), `FLIPNOW=1` (presentar sin
  esperar vblank; puede rasgar).
- **Perfilado:** `RSXPROF=1` imprime `[rsxprof]` (ms/s de uploads, draws,
  esperas del ring). `[slowdraw]` avisa cuando un frame sube muchas texturas,
  construye streams de vértices o compila programas.
- **Superficies:** `[rsx] surface ...` muestra los tamaños reales. Si ves
  `2560x1440`/`5120x1440` para una salida de 1280x720, el juego está
  renderizando con MSAA; forzar 1x o bajar la resolución interna requiere
  recompilar el port (no hay opción en `gtiport.ini`).
- **GPU real:** para 60 fps sostenidos en esta escena, lo más efectivo es
  ejecutar fuera de virgl (GPU passthrough o una máquina con GPU real). El
  autor lo corre a 60 en una GPU nativa; virgl agrega mucha latencia por
  llamada.

## Audio

El juego mezcla con **MultiStream**: una tarea SPU recompilada produce bloques
de 256 muestras que el hilo de `cellAudio` copia al ring y el port los manda a
SDL (48 kHz estéreo float).

Variables útiles:

| Variable | Efecto |
|---|---|
| `SPUS=3` o `SPUS=4` | workers de geometría (el lanzador usa `4`; el juego solo necesita 3) |
| `SDL_AUDIO_DEVICE_SAMPLE_FRAMES=1024` / `2048` | buffer del dispositivo (el lanzador usa `1024`) |
| `SNDWAV=1` | escribe `snd_out.f32` (PCM crudo) para diagnosticar |
| `SNDPROF=1` | hotspots del intérprete de SPU cada 10 s |
| `SNDTRACE=1` | traza detallada de la tarea de audio |

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

- El Flatpak es **solo Wayland**: hace falta una sesión Wayland (ya no
  incluye X11 ni XWayland).
- En una VM, probá `--env=RACING_CLUB_VIRTIO=1`.
- Si el host no expone 3D, usá software: `--env=RACING_CLUB_SOFTWARE_GL=1`.

### `MESA: error: vdrm_device_connect failed` / `radv/amdgpu`

Es ruido de Vulkan (`radv`/Venus enumeran el virtio-gpu y fallan). En modo
virtio/virgl el lanzador ya apaga esos ICDs (`VK_DRIVER_FILES=/nonexistent`)
y el juego usa OpenGL. Si aparece en una GPU real, es una GPU sin Vulkan 1.3:
el juego vuelve solo a OpenGL.

### El audio se corta o satura

En esta build el audio suele ser un síntoma del renderizador: si los frames
tardan, el mixer también se atrasa. Para confirmarlo, mirá sobre todo
`[pace]`/`[margin]` (ver [Rendimiento](#rendimiento)).

1. Probá `--env=SPUS=3` y `--env=SDL_AUDIO_DEVICE_SAMPLE_FRAMES=1024`.
2. Mirá las líneas `[snd]` del log:

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
| `build-game-flatpak.sh` | Empaquetado e instalación del Flatpak. |
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
