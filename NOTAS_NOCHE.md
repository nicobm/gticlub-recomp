# Trabajo nocturno (11/11/2026) — físicas comparadas con el original

## Versión para probar
- `build\gtiport_noche.exe`. `build\gtiport.exe` no se tocó porque estaba abierto.

## Método
1. **Repetición por ventanas.** Se cortaron 330 trozos de 1 s de las grabaciones reales del juego original:
   - tu sesión libre;
   - mis pruebas automáticas.
2. **Comparación.** El coche del port se coloca en el mismo estado (velocidad, giro y marcha) y recibe los mismos mandos fotograma a fotograma. Después se compara su velocidad y su giro con los del original.
3. **Ajuste automático.** Un optimizador ajusta las constantes del port para minimizar ese error.

Herramientas:
- `tools/replay_extract.py`: extrae las ventanas de las grabaciones.
- `tools/replay_fit.py`: compara y optimiza.
- `tools/accel_curve.py`: curva de aceleración.
- `tools/accel_cmp.py` y `tools/labsum.py`: pruebas puntuales.

## Resultado (Mini clásico; los 9 coches tienen físicas casi idénticas en el original)

| Prueba | Original | Port |
|---|---|---|
| 0 → 40 / 60 / 80 km/h | 0,65 / 1,15 / 1,74 s | 0,70 / 1,00 / 1,70 s |
| 0 → 100 / 120 / 140 km/h | 2,37 / 3,66 / 4,94 s | 2,30 / 3,40 / 5,10 s |
| 0 → 160 / 170 km/h | 7,31 / 8,70 s | 6,90 / 8,80 s |
| Frenada 63 → 14 km/h | 1,75 s | 1,80 s |
| Marcha atrás máxima | 48 km/h | 48 km/h |
| Giro a tope a 75 km/h (°/s a +0,1 / 0,3 / 0,5 / 0,8 s) | 21 / 90 / 108 / 100 | 44 / 84 / 102 / 109 |

**Error medio en las 330 ventanas.** De 7,7 se pasó a 4,5:
- velocidad: 1,7 m/s;
- deslizamiento lateral: 0,9 m/s;
- giro: 19 °/s.

## Hallazgos del original incorporados
- Sin resistencia del aire al levantar el pie: el coche mantiene la velocidad.
- El cambio de marcha dura lo que dice el XML (`Gear_Clutch_Timer` = 0,15 s).
- La fuerza por marcha no sigue a una caja de cambios real. Se ha ajustado con la curva de aceleración medida.
- El agarre lateral es mucho mayor: el original corrige los derrapes muy rápido.
- La marcha atrás llega a unos 48 km/h.

## Pendiente
- Al empezar a girar, el port reacciona algo antes que el original.
- Al frenar o ir marcha atrás girando, el port gira de más.
- Freno de mano: la velocidad ya coincide, pero el port gira de más.
- Traducción exacta del código original (vía larga).

## Segunda parte de la noche (tras tu grabación)

### Saltos ("gravedad lunar")
- **Medido en el original:** en el aire el coche cae a unos **2 g**. Cada fotograma pierde 0,297 m/s de velocidad vertical.
- **En el port:** ahora la gravedad es 2× y los muelles se han reforzado en proporción para que la altura de marcha no cambie.
- **Efecto en el ajuste:** con gravedad doble el ajuste general mejora (4,28 frente a 4,52), lo que confirma que el original funciona así.
- **Aceleración:** se reajustó la curva y la punta coincide (0-160 km/h en 7,3 s frente a 7,31 s; 0-170 km/h en 8,6 s frente a 8,7 s).

### Choques
- **Comparación sobre el circuito real.** Las coordenadas del original coinciden al milímetro con las del port, así que las ventanas se pueden repetir sobre el circuito de verdad, con muros, bordillos y saltos.
- **No hay malla de colisión separada.** El original choca contra la geometría visible, igual que el port.
- **Ajuste.** Se ajustaron rebote, rozamiento y giro de los choques con 174 momentos de choque reales. De media, tras un choque el original conserva el 69 % de la velocidad; el port estaba en el 80 % y ahora pierde algo más.
- **Diferencias que quedan.** Los choques son caóticos: unos centímetros cambian el resultado. Algunos golpes contra bordillos aún no se comportan igual.

### Cámaras
- **Interior:** ya no sale negra con ningún coche. Ahora es una vista limpia desde los ojos del conductor.
- **Cinematográfica:** es el cuarto modo de la tecla V. Usa las 26 cámaras de repetición del propio juego (`replaycameras.ep3`). La cámara más cercana sigue al coche con zoom tipo televisión.
- **Stick derecho del mando:** gira la cámara alrededor del coche. A tope, mira hacia atrás.

### Ejecutables
- `build\gtiport.exe` y `build\gtiport_noche.exe` son la misma versión, la última.

## Física original recompilada (11/11, tarde)

### Resultado
- **Bit a bit igual que el emulador durante 24 fotogramas seguidos.** Es toda la grabación disponible.
- Solo difieren unas palabras temporales que escribe código ajeno a la física (+0x270…+0x28c) y dos punteros de pila.

### Cómo funciona el original
- **`World::update` (0x31bab8)** es el objeto mundo físico.
  - Vtable 0xbfbaa0; en snap2 está en 0x31782da0.
  - 801 cuerpos; el coche es el último.
  - Antes y después de cada fotograma llama a los oyentes de +0x100.
  - Llama a **0x31b780**, el programador de paso fijo:
    - acumulador en +0xa0;
    - paso de 1/60 s en +0x9c, multiplicado por la escala de tiempo global;
    - el tiempo del fotograma está en `*(*(*(toc+0x25f4))+0xcc)+0xbc`.
  - En cada subpaso llama a los oyentes (vtable+0x10 y +0x14) y a **0x31b108**, el paso del mundo.
- **0x31b108**, para cada cuerpo activo:
  1. vtable+0x3c;
  2. guarda el historial (`memcpy` +0xf0 → +0x180, 0x50 bytes);
  3. actualiza (0x3367c0);
  4. confirma (0x313a70);
  5. predice (vtable+0x6c; en el coche es 0x332808 → 0x315b88, que escribe +0xf0 = pos + v·dt);
  6. vtable+0x38.
  - Después: colisiones (0x311c20, 0x31ac50), solver de contactos (0x316df8, 5 iteraciones), una segunda predicción por cuerpo, 0x312dd0 y vtable+0x50; al final 0x317538.
- **Motor y caja de cambios: 0x26b078 (motor = car+0xb4).**
  - Calcula rpm (+0x1ac) y marcha (+0x1a4), con cambio automático.
  - **No influye en la dinámica:** la velocidad sale idéntica con o sin esta llamada. Solo alimenta el sonido y el marcador.
- **Entradas crudas del jugador:**

  | Campo | Mando |
  |---|---|
  | car+0x410 | acelerador |
  | car+0x418 | freno |
  | car+0x420 | dirección (+ = izquierda) |
  | car+0x41c | freno de mano |

  El juego las suaviza por su cuenta.

### Arreglos en el recompilador (`tools/ppc2cpp.py`)
- **Funciones por grafo de control.** Antes se cortaban de forma lineal hasta el siguiente inicio. Los datos desensamblados como `bla` creaban inicios falsos; por ejemplo, la colisión 0x9cf898 quedaba partida en 0x9d0398. El resultado eran pilas sin restaurar y fallos a los ~60 fotogramas. Ahora cada función incluye todo lo alcanzable desde ella; un salto a otro inicio real se trata como llamada de cola.
- **Tablas `switch`.** Un `bctr` dentro de la función hace un `switch` sobre los inicios de bloque.
- **Instrucciones nuevas.**
  - `dcbt`, `dcbst`, `dcbf`, `icbi`, `sync`, `lwsync`, `eieio` e `isync` son no-op.
  - `dcbz` pone a cero 128 bytes.
- **Desensamblado de 0x10000–0x20000.** Falta en `ppu_dis.pkl`; ahí está la salida del proceso (0x10278). No hace falta para la física.
- **Base de tiempo determinista (`g_timebaseFixed`).** Antes el código leía el reloj del PC (`mftb`) y cada ejecución salía distinta.

### Port
- **Activación.** `gtiport.exe --orig ..\capture\snap2.snap`, o `build\conducir_fisica_original.bat`.
- **Arranque.** Parte de la instantánea: el Abarth a 45 km/h en la bajada de Côte d'Azur.
- **Teclas.**
  - O cambia entre la física original y la del port.
  - Retroceso vuelve a la instantánea.
- **Rendimiento.** Unos 0,5 ms por fotograma.
- **Código.**
  - `src/game/orig_physics.{h,cpp}`: localiza solos el mundo y el coche en la instantánea.
  - CMake enlaza `recomp_full` con `gtiport` (`GTI_ORIG_PHYSICS`).
- **Herramienta de pruebas: `recomp_probe ... world <W> 31bab8 N`.**
  - `INPUT=thr,brk,steer,hb`.
  - `TBFIXED=100`.
  - `SHOWDIFF` muestra las diferencias.
  - `SHADOW` muestra la cadena de funciones si hay un fallo.
  - `CHECKNV` comprueba los registros no volátiles.
  - `ENGFN2=fn,off` y `DUMPRANGE=lo,hi[,ptr]`.

### Pendiente
- **Comparación con mandos cambiantes.** La instantánea solo trae 0,7 s grabados con mandos fijos. Falta grabar en RPCS3 una instantánea con más fotogramas.
- **Elegir coche o punto de salida.** Hace falta una instantánea por coche o crear el coche con el código original.
- **Ruedas.** La compresión de la suspensión es fija; falta leerla de los objetos rueda (car+0x2c8…).

## Datos del original aplicados al port (11/11, tarde 2)

### Ruedas (objetos rueda, car+0x2c8 …, orden FL FR RL RR)
| Offset | Dato |
|---|---|
| +0x04 | radio |
| +0x0c | ángulo de rodadura acumulado (rad) |
| +0x10 | entrada de dirección |
| +0x18 | ángulo de dirección (rad, máx. 22,5°) |
| +0x24 | velocidad angular |
| +0x50 | punto de contacto con el suelo, en el mundo |

- El centro de la rueda es el contacto más el radio.
- El port dibuja las ruedas con estos datos: giro, rodadura y recorrido de suspensión.

### Origen del coche
- La matriz con la que el juego dibuja la carrocería tiene como traslación el +0x50 de la física.
- Ese origen está a nivel de la calzada y coincide con el origen del modelo 3D.

### Cámaras (`data/cars/effects/cameras.ep3`)
- Cercana `Camera_Chase`: (0; 1,288; −7,062).
- Lejana `Camera_ChaseHigh`: (0; 2,1015; −8,2353).
- Ambas apuntan al localizador `ChaseCam` (0; 2,16; 0). De ahí salen los 7° hacia arriba de la cercana y los 0,4° de la lejana.
- FOV vertical de 55,79°.
- Retrovisor `RearView`: (0,008; 1,255; −1,763), mirando atrás, FOV 67,4°.
- Validado con las capturas PS3 `reference/ps3_t174` (lejana) y `ps3_t198` (cercana).
- Retraso de seguimiento medido: ~0,6–1,5 m a 100–120 km/h; en el port, suavizado de 25/s. La dinámica exacta de la cámara original está sin recompilar.

### Coche y salida
- **Elección del modelo.** El port lo elige comparando los *hardpoints* de cada modelo con los contactos de rueda de la física original. snap2 es `fiatabarth` (pack original).
- **Salida.** Si la instantánea no está parada, `OrigPhysics::place` pone el coche en `Start_Lower_0`. Escribe las filas de orientación (+0x20, +0x60, +0x100, +0x190), el origen +0x50, el centro de masas (+0x90, +0xf0, +0x180) y la velocidad y velocidad angular a 0.
- **Diferencias entre coches.** Los XML de los 9 coches difieren en muy poco: CoM, Force_Offset, AirDrag, marchas, TC_Torque_Mul, Pacejka_Lat, amortiguación, freno y DefaultAfterTouch. La geometría sale del modelo.
- **Fidelidad por coche.** Para que cada coche sea idéntico, se toma una instantánea del original por coche en la parrilla: `tools/rpcs3_snapcars.py`, que escribe en `capture/cars/`.
  - El primer intento se paró porque la ventana del juego perdió el foco.
  - Hace falta dejar el PC ~10 minutos sin tocar.
  - Con `--drive` graba además una conducción con mandos variables para comparar fotograma a fotograma.
- **Coche activo.** El port elige el cuerpo con la vtable del coche dentro de la lista del mundo físico vivo (+0xc4).

## Instantáneas por coche y verificación larga (11/11, tarde 3)

### Instantánea de la salida real
- `capture/cars/orig0.snap` es el Fiat A112 Abarth parado en la salida real del Free Run, en lo alto de la cuesta (−12,82; 87,52; −209,56). `Start_Lower_0` no es esa salida.
- En la parrilla, el centro de las ruedas del original queda a 0,266–0,269 m; el buje del modelo está a 0,268 m.

### Verificación larga
- **Datos:** `capture/cars/orig0_drive.bin`, una conducción grabada en RPCS3 justo después de la instantánea, de 9 s. Incluye acelerón, volantazos, choque, freno, freno de mano con giro, marcha atrás y otra vez acelerón.
- **Comando:** `recomp_probe ... replay 31782da0 capture/cars/orig0_drive.bin` (con `TBFIXED=100`).
- **Resultado:** se reproducen los 539 pasos. 522 muestras coinciden bit a bit, con el objeto coche entero idéntico. Las 4 restantes son muestras aisladas tomadas a mitad de paso, y la simulación sigue exacta después.
- **Claves para alinear:**
  - El reloj del coche (+0x374) avanza 1/60 por paso; da los pasos exactos entre muestras y los pasos de espera entre la instantánea y la grabación (114).
  - Los fotogramas largos se reproducen como una sola llamada a `World::update` con dt = n/60.
  - Cuando el mando cambia entre dos muestras, se prueban todos los pasos posibles del cambio (con memoria guardada y restaurada).
- **Entrada nueva:** +0x424 es el botón de freno (1 al frenar). El port ya la escribe.
- **El motor (0x26b078) no influye en la dinámica:** el resultado es el mismo llamándolo o no.

### Captura del resto de coches (`tools/rpcs3_snapcars.py`)
- **Secuencia de menús comprobada en pantalla:**
  1. Return.
  2. X (aviso de guardado), esperar 8 s.
  3. X (aviso de PSN).
  4. X (un jugador).
  5. Right ×3 (Free Run).
  6. X, esperar 7 s.
  7. **D** (stick izquierdo; la cruceta no cambia de coche) ×idx.
  8. X (coche).
  9. X (cambio automático), esperar 20 s.
- **Coche vivo:** `tools/live_car.py`, el coche que está en la lista de cuerpos del mundo físico vivo.
- **Coches pendientes:**
  - orig1–4 (orden: Abarth, Lancia Delta, Golf GTI…) y dlc0–3.
  - El último intento falló: el stick se pulsaba demasiado pronto (ahora hay una espera de 7 s) y luego se perdió el foco porque el usuario volvió al PC.
- **Port:** `--orig <carpeta>` usa todas las `.snap` (N/B), y `find_data_dir` ignora el argumento de `--orig`.

## Recompilación completa del juego (vía 2), estado al 12/11

### Arranque nativo
- **Comando:** `build-test\recomp_boot.exe re\EBOOT.elf ..\NPEB00060\USRDIR [--seconds N] [--trace] [--chain] [--watch EA] [--pad "start:15,cross:20"]`.
  - Variable de entorno `SAMPLE=1`: guarda la cadena de llamadas de cada hilo cada 0,5 s.
- **Capa de sistema:** `src/recomp/lv2.cpp`. Cubre:
  - hilos reales y TLS;
  - lwmutex/lwcond, mutex, cond, semáforos, colas y puertos de eventos, *event flags* y temporizadores;
  - ficheros y directorios (`/app_home` → USRDIR);
  - la familia `printf`;
  - Edge Zlib (inflado en el PC);
  - GCM con un procesador de órdenes RSX sin dibujo;
  - mando virtual en modo presión (sensores a 512);
  - salida de vídeo 1280×720, idioma inglés y «sin partida guardada».
- **Recompilador (`tools/ppc2cpp.py`):**
  - funciones por grafo de control con `goto` a la entrada;
  - tablas `switch` exactas (`tools/jumptables.py` → `re/jt_map.txt`);
  - `lwarx`/`stwcx.` atómicos;
  - `dcbz`.
- **Pruebas:**
  - `recomp_fmt.exe`: el `snprintf` del juego, correcto.
  - `spu_test.exe`: el intérprete SPU con la tarea Edge Zlib.

### Qué hace ya el juego
1. Inicializa GCM, SPURS, audio y 7 hilos.
2. Carga 376 ficheros: 370 shaders de `shadercache`, la región, las pinturas y las interfaces SumoTool `global` y `popups`.
3. Entra en su bucle principal: unos 24 cambios de cuadro por segundo, sin dibujar.

### El protocolo de órdenes RSX de este juego
- Dos anillos de órdenes alternos, en las posiciones IO 0x213e980 y 0x253e980.
- Los trabajos SPU de geometría (Edge) rellenan huecos de 32 bytes que empiezan con un salto a sí mismo. Mientras no se ejecuten, la RSX mínima salta el hueco tras 3 ms.
- **Al final de cada cuadro:**
  - el cuadro escribe una etiqueta con el semáforo 0x1d6c/0x1d70 (desplazamiento 0x4c0);
  - el cambio de cuadro (0xFEAC) avisa al hilo de render por su cola de eventos (puerto en +0x2dc);
  - la etiqueta 0x81 hace de semáforo para el manejador de vblank.

### Dónde está parado
- En la secuencia de introducción (SumoTool `INTRO_SEQUENCE`: avisos ESRB, pack de coches, «PRESS START»).
- No pasa a cargar `Data\Intro\*.rep` ni el escenario, aunque se pulsen START y X.
- Hace falta ver la imagen para saber qué espera.

### Siguiente: RSX real
- Los shaders son microcódigo RSX compilado (ficheros `CGB` en `shadercache`).
- Hace falta un traductor de programas de vértices y de fragmentos a GLSL, como RPCS3, además del estado NV4097: superficies, vértices, texturas, mezcla y dibujo.

## Recompilación: avances del 12/11 (tarde)
- **Desbloqueo de la intro:**
  - `cellSaveDataAutoLoad2` y `AutoSave2` están implementadas de verdad: se llama a `funcStat` y `funcFile` del juego mediante `call_nested`. Las partidas se guardan en `<hdd0>/home/00000001/savedata/`.
  - Con eso el juego carga `intro_sequence`, Côte d'Azur, los objetos del recorrido y `carselect`: 740 ficheros.
- **`sys_time_get_current_time` (145):** ahora escribe los segundos y nanosegundos.
- **SPURS JobChain en el PC (namespace `jobs`, en `lv2.cpp`):**
  - Un hilo recorre la lista de órdenes: GUARD `ea|0xf`, JOB, SYNC `2`, NEXT `ea|3`, END `0x7f`.
  - Ejecuta cada binario en el intérprete SPU.
  - **Contexto JM2:** `ioBuffer` en +0, `cacheBuffer[4]` en +4, `oBuffer` en +0x1c, `scratch` en +0x20, `dmaTag` en +0x24 y `eaJobDescriptor` en +0x28.
  - **Entrada:** en `binario+0x10`, con r3 = contexto y r4 = descriptor en LS.
- **Binarios de trabajo:**
  - `0xba3680`: despachador; rellena el hueco.
  - `0xbaea80`: geometría Edge (84 KB, *scratch* de 79 KB).
  - `0xbad880`: activa la *event flag*; se hace en el PC.
- **RSX:**
  - Guarda contra saltos fuera de IO.
  - Resincronización tras los huecos, que miden 24 o 32 bytes.
  - `HOLEWAIT` en µs: 200 ms por defecto con los trabajos activados.
  - `NOJOBS=1` hace que se salten los huecos.
- **Herramientas:**
  - `HLECOUNT=1`: recuento de importaciones.
  - `JOBDUMP=1` y `JOBTRACE=1`.
  - `tools/strxref.py`: referencias a cadenas mediante la TOC.
  - `tools/dis.py ADDR N BEFORE`: desensamblado PPU.
- **Bloqueo actual:**
  - Edge espera en `binario+0x1a50..0x1b9c`.
  - Lee la etiqueta RSX 0x400 (`labels+0x400` = posición consumida del anillo de salida 0x32a3e980..0x32d3e980).
  - La RSX, a su vez, espera en un hueco: interbloqueo.
  - **Causas posibles:** el intérprete SPU es lento (los huecos caducan y se saltan, y la etiqueta deja de ser fiable) o Edge necesita varios SPU en paralelo (*max contention* 3).
- **Siguiente:**
  - Varios hilos de trabajos.
  - Entender la condición de espera (r12/r18/r10 frente a la etiqueta).
  - Recompilar estáticamente los binarios SPU a C++ para ganar velocidad.
  - Después, la RSX real: traductor VP/FP a GLSL.

## Recompilación: imagen real y primera carrera (12/11, mañana)

### Qué funciona ya
- **Renderizador RSX → OpenGL 4.5** (`src/recomp/rsx_gl.{h,cpp}`), ventana SDL en el hilo de la RSX:
  - traduce el microcódigo de vértices y de fragmentos a GLSL;
  - superficies como texturas de FBO (antialiasing 2× en horizontal → 2560×720);
  - texturas DXT1/3/5, ARGB, *swizzled* y cubemaps;
  - las constantes de los shaders de fragmentos (el juego las parchea dentro del microcódigo) van como `uniform fc[]`.
- El juego recompilado llega a: logos → avisos legales → título «PRESS START» → aviso de autoguardado → PSN «no conectado» → menú principal → Single Player → selección de coche → **carrera** (cuenta atrás, HUD, tiempo).
- **Lanzador:** `build-test\jugar_recompilado.bat`.
  - Teclado: Enter = START, X/Espacio = Cruz, Esc = Círculo, flechas, A/D = volante, W/S = acelerar/frenar.
  - El mando de Xbox también funciona.

### Arreglos importantes
- **Flips en el vblank siguiente**, como el modo vsync de la consola. Antes, dos flips seguidos perdían un evento y 2 de cada 3 arranques se colgaban.
- **Edge Zlib:** la estructura correcta es origen, destino, tamaño comprimido, tamaño final, contador|tipo, *event flag*, bits. La versión anterior dejaba vacía la memoria local de la RSX y escribía basura en direcciones bajas.
- **`GL_PRIMITIVE_RESTART_FIXED_INDEX` es 0x8D69.** Con 0x8F9D, el índice 0 cortaba primitivas.
- **Geometría Edge rota con varias SPU:** Edge elige su búfer de salida por número de SPU, que lee del contexto del núcleo SPURS (LS 0x1C8). Ahora cada hilo de trabajos escribe ahí su número.
- **Gestor de trabajos más fiel:**
  - copia el descriptor y los datos de entrada al tomar el trabajo;
  - las listas DMA usan la semántica `lsa | (ea & 0xF)`.
- **Red:**
  - el diálogo de conexión termina en «no conectado» (eventos sysutil 0x801/0x802/0x803);
  - `socket()` falla;
  - NP queda fuera de línea.

### Depuración (variables de entorno)
- `RSXSHOT=N`: cada N cuadros guarda en BMP todas las superficies de color.
- `RSXLOG=n`: vuelca los dibujos del cuadro n.
- `RSXDUMP=1`: guarda los GLSL generados en `rsxdump/`.
- `RSXSKIP=a-b`: omite esos dibujos.
- `RSXONLYFMT` / `RSXSKIPFMT`: filtran los dibujos por formato de posición.
- `RSXVSTAT=1`: estadísticas de las posiciones.
- `RSX=0`: sin ventana.
- `SPUS=n`: número de hilos de trabajos.
- `HOLEWAIT=µs`: espera máxima de los huecos de órdenes.
- `ZLIBDUMP=1`: vuelca los elementos de Edge Zlib.
- `METHODHIST=1`: histograma de métodos NV4097.

### Pendiente
- **Velocidad:** unos 7 fps, limitados por el intérprete SPU que ejecuta la geometría Edge.
  - Solución: recompilar estáticamente los binarios SPU a C++.
- **Gráficos:**
  - la carretera sale negra;
  - los números de la cuenta atrás se quedan superpuestos;
  - no se ve el coche del jugador, solo un destello;
  - el cielo sale negro o blanco;
  - los cubemaps generados por render (reflejos) se leen de memoria y no de la superficie;
  - el minimapa sale vacío.

## Recompilación: 60 fps (12/11, tarde)

### Qué ha cambiado
- **Recompilador de SPU (`tools/spu2cpp.py`) reescrito:**
  - Cada trozo de 1 KB se genera en dos variantes. La **rápida** solo tiene como puntos de entrada los destinos de salto reales (1085 de 21248 instrucciones en Edge), así el compilador optimiza entre instrucciones. La **lenta** tiene una entrada por instrucción y se usa cuando un salto indirecto cae en una dirección no prevista.
  - Las operaciones calientes usan SIMD (`src/recomp/spu_ops.h`): `shufb`, cargas y guardados, desplazamientos por bytes, máscaras de inserción y conversiones float↔entero sin `ldexp`.
  - Los ficheros generados se compilan con `-mavx2 -fno-strict-aliasing -ffp-contract=off`.
- **Prueba diferencial (`build-test/spu_diff.exe re/EBOOT.elf [pruebas]`):** ejecuta cada instrucción de los dos binarios en el intérprete y en el código generado con registros aleatorios y compara registros, memoria local y pc. Resultado: 366 776 ejecuciones, 0 diferencias. Dos NaN con distinta carga cuentan como iguales.
- **Flips:** el juego se sincroniza él mismo con el vblank (etiqueta 0x81: el hilo de render escribe 1, el manejador de vblank la pasa a 2 y la lista de órdenes espera ese 2 antes del flip). Retrasar además el flip al vblank siguiente dejaba el juego en 30 fps. Ahora un flip precedido de esa espera se muestra al momento; los demás siguen esperando al vblank. El juego usa el modo de flip 1 (hsync).
- **Hilos de trabajos:** 6 por defecto. Con 8 el juego se cuelga en el arranque (sin investigar; Edge tiene un anillo de salida por SPU).

### Medidas (escena del título)
| | Antes | Ahora |
|---|---|---|
| Trabajo de geometría Edge | 170 µs | 24 µs |
| Despachador | 4,9 µs | 2,4 µs |
| Ocupación de las SPU | 90 % (4 hilos) | 20 % (6 hilos) |
| Cuadros por segundo | 25-30 | 60 |

- 4 arranques seguidos de 22 s: todos llegan al título a 60 fps.
- Sin medir todavía en carrera.
- Quedan unas 200 000 entradas por segundo a la variante lenta (unas 4 por trabajo): saltos indirectos cuyos destinos convendría añadir como puntos de entrada.

### Variables nuevas
- `FLIPLATE=1`: comportamiento anterior, todos los flips esperan al vblank siguiente.
- `NOSPUREC=1`: intérprete SPU en vez del código recompilado.

### Pendiente
- Los fallos gráficos siguen igual que antes de este cambio (comprobado con capturas del intérprete y de la mañana): el logo deja estela (la superficie no se borra), la escena 3D sale deformada, cielo blanco.

## Recompilación: la geometría que faltaba (12/11, noche)

### Qué pasaba
Faltaba casi toda la geometría opaca (carretera, edificios, coches, cielo) y lo que sí se dibujaba dejaba estela. Eran dos fallos distintos, los dos en cómo se ejecutan los trabajos de geometría Edge en las SPU.

1. **Demasiados hilos de SPU.** La cadena de trabajos del juego tiene contención máxima 3 y solo crea búferes de salida de Edge para las SPU 0-2 (`0x01061980 + 0x80·n`; el compartido de `0x01061900` está vacío). Con 4 o 6 hilos, los que hacían de SPU 3-5 no tenían dónde escribir: Edge rellenaba el hueco de órdenes sin dibujo y la geometría se perdía (la mitad con 6 hilos). Ahora son 3 hilos.
2. **Coma flotante de la SPU.** La SPU no tiene NaN ni infinitos: el exponente 255 es un número finito enorme, los desnormalizados valen cero y los resultados saturan. Edge se apoya en ello: en el descarte de triángulos calcula `c − a·b` (`fnms`) sobre máscaras de bits, con `b = 0xFFFFFFFF`. En la SPU, `0 × 0xFFFFFFFF = 0`; con IEEE es NaN, y el resultado marcaba **todos** los triángulos como descartados. `spu_float_in` / `spu_float_out` (`spu.h`) aplican esas reglas a todas las operaciones de float, en el intérprete y en el código recompilado.

### Cómo se encontró (herramientas nuevas, variables de entorno)
- `FIFODUMP=n`: todas las órdenes RSX del cuadro n.
- `EDGEDUMP=n`: descriptor y búferes de entrada de los trabajos Edge n..n+2.
- `EDGEVIEW=1`: matrices local→mundo que reciben los trabajos.
- `EDGETRACE=n`: traza instrucción a instrucción de un trabajo Edge (intérprete) a `edgetrace_m*.txt`.
- `EDGESTAT=n`: 4000 trabajos en el intérprete con lo que devuelve la rutina de descarte (LS 0x9E68, vuelve en 0x4400 con el número de índices visibles), las órdenes MFC, las estructuras de los anillos y el bloque que el trabajo pone en su hueco. `EDGETRACEIDX=k` añade la traza de los trabajos con k índices.
- `RINGLOG=1`: KB de salida de Edge por cuadro en cada anillo.
- `RSXSTEP=n`: la superficie principal tras cada dibujo del cuadro n (`rsxstep.bin`, 640×360 RGB), empezando en magenta.
- Línea `[jobs] Edge jobs with a draw …/s, draws executed by the RSX …/s`.
- `[wait]` al salir: anillos de Edge, etiquetas RSX 0x400/0x410/0x420 y trabajos SPU aún en marcha.

### Datos de Edge en este juego
- Binario `0xbaea80`. Entradas del trabajo: 0 = plantilla de órdenes RSX (304 bytes), 9/10 = flujos de vértices, 15 = viewport (recorte u16×4, rango de profundidad, matriz vista-proyección 4×4, escala y desplazamiento del viewport, modo de muestreo), 16 = matriz local→mundo 3×4, 17 = parámetros (16 bytes; nº de vértices y de índices en +8/+10).
- Salida: `GETLLAR` del búfer de su SPU → `PUTLLUC` → `PUT` de índices y vértices → `PUTS` del bloque de órdenes en el hueco (empieza con un salto a sí mismo que la RSX espera). Cada dibujo termina escribiendo la etiqueta de su anillo (0x400, 0x410, 0x420) con el final de sus datos; el asignador trata `[actual, etiqueta)` como libre.
- El juego no entrega el cuadro a la RSX hasta que acaban sus trabajos, así que un cuadro que necesite más de un anillo entero (3 MB) no puede terminar. `ring_watchdog` lo detecta (trabajo > 150 ms con la RSX parada) y libera el anillo. Ocurre unas pocas veces en el título y los menús: hay cuadros de 2,9 MB por anillo. Habría que comprobar si Edge descarta aquí menos que en la consola.

### Estado
- Título, menús, selección de coche y carrera se ven bien: ciudad, cielo, coches, HUD, minimapa.
- Pendiente: la carretera (y el suelo de la selección de coche) sale negra; el rendimiento ha bajado a ~30 fps de media al haber mucha más geometría (trabajo Edge ~40 µs, 3 hilos).

### Después (12/11, noche): carretera y cuelgues de arranque
- **Carretera negra:** la sombra proyectada sobre el asfalto usa una textura con direccionamiento `BORDER` y multiplica el fondo; faltaba `NV4097_SET_TEXTURE_BORDER_COLOR` (0x1A1C + 0x20·i), así que fuera de la sombra se leía negro. `apply_border_color` en `rsx_gl.cpp`.
- **Cuelgue esporádico al arrancar («BAD jump»):** el manejador de flip avisa al hilo de render, que empieza a reescribir el búfer de órdenes de ese cuadro; pero tras el flip aún quedan órdenes del cuadro (contadores y el salto al otro búfer). Si el hilo de la FIFO se retrasaba (compilando un shader, por ejemplo), leía órdenes del cuadro siguiente. Ahora el manejador se llama cuando la FIFO ejecuta su siguiente salto o se queda sin órdenes (`deliver_flip`). 5 arranques de 5 correctos.
- La carrera se ve ya como el original: asfalto, marcas, coche, edificios, túnel, cielo, HUD y minimapa. Entre 30 y 60 fps; las SPU van al 27 %, así que el límite está en el hilo de la RSX o en la PPU.

### Rendimiento del renderer (13/11)
- `FIFOPROF=1` y `RSXPROF=1`: reparto del tiempo del hilo de la FIFO y de las fases de `do_draw`.
- El hilo de la RSX estaba saturado por la conversión de vértices: se convertía todo el rango entre el índice mínimo y el máximo (29 M de vértices/s para 3,4 M de índices). Ahora, si el rango es disperso, solo se convierten los vértices referenciados.
- La firma del contenido de cada textura se calcula una vez por cuadro, no en cada dibujo.
- Resultado: carrera a 60 fps estables; título ~45 fps; menús 30-60.
- Pendiente: la conversión de vértices sigue siendo la fase más cara (130-230 ms/s).

### Tras la primera prueba del usuario (13/11)
Informe: polígonos mezclados a pantalla completa durante unos segundos, 30 fps casi siempre, coches y escenarios sin brillo.
- **Polígonos mezclados:** eran los rescates de `ring_watchdog` (pisan geometría del cuadro). Causa de fondo: los anillos de salida de Edge (3 MB por SPU) están dimensionados para una RSX que los vacía enseguida. Ahora el primer trabajo Edge los sustituye por anillos de 24 MB en `0x44000000` (`EDGERING=MB`, 0 = los del juego). Deben quedar por debajo de `ioAddr + 512 MB`: Edge solo conserva 29 bits del desplazamiento RSX del búfer de índices. Sin rescates desde entonces.
- **SPU:** la regla de float se reduce a «un producto con un factor cero es cero» (`spu_mul`); el ajuste completo de operandos era correcto pero un 40 % más lento. `shufb`, los desplazamientos por bytes y `gb/gbh/gbb` trabajan directamente en la disposición de `Q`, sin conversiones (`shufb` es el 22 % de las instrucciones de Edge). Trabajo Edge: ~34 µs → ~19 µs.
- **RSX:** `convert_attr` con un bucle por formato; caché en GPU de los flujos de vértices de la memoria local (`cached_stream`, `RSXNOSTREAMCACHE=1` la desactiva): se convierten una vez y se comprueba una muestra por cuadro. `do_draw` baja a ~9,6 µs por dibujo.
- Muestreo (`SAMPLE=1 --chain`): en los tramos lentos el hilo de render del juego espera el flip el 62 % del tiempo; el límite es el hilo de la RSX cuando el cuadro pasa de ~1100 dibujos.
- **Estado:** título y carrera a 60 fps; la selección de coche y la parrilla, entre 31 y 48.
- **Pendiente:** bajar más el coste por dibujo (texturas 2,1 µs, programa 1,2, estado 1,1, índices 1,3); reflejos (los cubemaps renderizados se leen de memoria y no de la superficie: por eso falta brillo en coches y escenarios); sonido sin comprobar.
- **Reflejos:** un cubemap cuyas caras son superficies de render (6 de 256×256 a partir de la primera, separadas `w·h·4`) se monta una vez por cuadro copiando las superficies (`glCopyTexSubImage2D`) en vez de leerse de memoria.
- **Sombras:** las texturas de profundidad con función de comparación (`addr` bits 28-30) se muestrean con `shcmp` (2×2 tomas, `textureGather`) y devuelven el resultado de la comparación, no la profundidad. `texDimsOf` devuelve 3 para ellas; la función va en `u_tsc[n].z`.
- Pendiente: la pintura de los coches se ve moteada (revisar la reflexión o el mapa de normales).

### Tras la segunda prueba del usuario (13/11): 60 fps estables
Informe: parón al aparecer la pantalla con el mapa, 30 fps o menos con rivales en pantalla y en varias zonas, microparones en las colisiones.
- **Causa de los 30 fps: latencias de sondeo, no falta de velocidad.** Cronología con `FRAMELOG=n`: la FIFO ejecuta el cuadro a la vez que el juego lo construye y llega a la espera de la etiqueta 0x81 cuando el juego publica el final (12-15 ms tras el flip). Pero la espera del vblank sondeaba con `sleep_for(10 us)` (en Windows ≥ 1-2 ms) y el hilo de render sondea otra etiqueta con `sys_timer_usleep(30)` tras cada flip (~0,8 ms). Se perdían ~3,5 ms por cuadro y cualquier cuadro pesado caía al vblank siguiente.
  - La espera de la etiqueta 0x81 se despierta desde el hilo de vblank (`g_vblankTick`, `wait`/`notify_all`).
  - `sys_timer_usleep` por debajo de 1,5 ms cede la CPU en bucle hasta que pasa el tiempo.
- **6 hilos de SPU:** `run_job` crea las descripciones de búfer de Edge de los trabajadores 3-5 (copia de la de la SPU 0, anillo propio y etiquetas RSX 0x600/0x610/0x620). La espera de trabajos del hilo de render baja de 175 a 60 ms/s.
- **RSX:** un solo recorrido del programa de fragmentos por dibujo (hash y constantes), hash del programa de vértices solo tras una carga, `fnv` de 8 en 8 bytes, estado GL y parámetros de muestreo solo cuando cambian, `half2f` sin `ldexp`. Órdenes por cuadro pesado: 18 → 11,7 ms.
- **Perfiles nuevos:** `WAITPROF=1` (ms/s bloqueado por hilo y punto de espera), `RIPPROF=seg` (muestreo del hilo de la FIFO a `ripprof.txt`, con módulo para lo que no es del exe), `FRAMELOG=n` (cronología de 40 cuadros), `FIFOPROF=1` separa espera de etiqueta y flip.
- **Resultado:** 60 fps en título, menús, selección de coche, parrilla y carrera en el recorrido de prueba.
- Pendiente: microparones (probablemente compilación de shaders la primera vez que aparece un efecto), pintura moteada, sonido.

### Margen de cuadro (13/11, noche)
El juego necesita que la FIFO llegue a la espera de vblank menos de ~15,8 ms después del flip anterior. Lo que se ha quitado para conseguirlo:
- **NOP en bloque:** Edge rellena sus huecos con cientos de órdenes vacías; la FIFO las procesaba una a una con una escritura atómica cada vez (~5 ms por cuadro). Ahora salta la racha entera.
- **Reloj fuera de la ruta caliente:** el registro `[slow]` por orden leía el reloj ~80 000 veces por cuadro (25 % del hilo). Eliminado; queda `[slowdraw]` (dibujos de más de 6 ms, con lo que tuvieron que construir) y `[pace]` (peor cuadro cada 2 s).
- **Esperas con cesión, no con `Sleep`:** huecos, FIFO sin órdenes, gestor de trabajos (GUARD / END) ceden la CPU los primeros milisegundos.
- **Importaciones baratas:** el juego hace ~3200 llamadas de sistema por cuadro (`sys_ppu_thread_get_id`, `sys_lwmutex_lock/unlock`, `cellSpursRunJobChain` una vez por trabajo). Las cuatro más frecuentes se atienden al principio de `imports()` reconociéndolas por la dirección de su nombre; `WaitScope` ya no crea cadenas ni toma cerrojos (una ranura por hilo).
- **Caché de programas GL en disco** (`glcache/`, `RSXNOGLCACHE=1` la desactiva): binarios enlazados con `glGetProgramBinary`.
- Cronología típica de un cuadro pesado ahora: el juego publica el final a +10 ms del flip y la FIFO llega a la espera a +12-14 ms.
- **Pendiente:** parones en las transiciones (100-330 ms, idénticos con la caché de shaders fría y caliente, no ocurren en el hilo de la RSX: son del lado PPU); unos segundos a 30 fps al entrar en la parrilla.
- **Superficies por dirección y tamaño:** el juego usa la misma dirección para el búfer de profundidad de las caras del cubemap (256×256) y el de la escena (2560×720). `get_surface` destruía y creaba la textura (y todos los FBO) varias veces por cuadro; ahora aparca la textura de cada tamaño y la recupera. De miles de creaciones por ejecución a 49. Con esto desaparece el tramo a 30 fps de la parrilla.
- `LOGTIME=1` antepone los segundos desde el arranque a cada línea; `[hitch]` marca cada hueco de más de 60 ms entre flips.
- **Subida de texturas:** las texturas sin reordenar y sin intercambio de bytes se pasan a GL directamente desde la memoria del juego (con `GL_UNPACK_ROW_LENGTH` para el pitch); las reordenadas (*swizzled*) de 32 bits usan una tabla por eje. Una textura de 5,4 MB que cambia de contenido tardaba 20 ms (microparón); ya no aparece ningún `[slowdraw]`.
- **Perfilador:** `RIPTHREAD=nombre` elige el hilo (`fifo`, `main`, `Render`...) y `RIPLEN=seg` la duración; `RIPPROF` admite decimales.
- **Parones que quedan** (100-270 ms): solo en transiciones de pantalla y al cargar; coinciden con ráfagas de aperturas de ficheros del juego.

### Decodificador de música propio (15/11, fase 1: interfaz)
Objetivo: sustituir flashATRAC.pic (firmware de Sony) por un decodificador ATRAC3 nuestro.
- Ficheros: `.msa` = contenedor MSF (`MSF\x02`, cabecera 0x40), códec 5 = ATRAC3 105 kbps (0x98 bytes por canal y trama), estéreo, 48 kHz.
- El mezclador (tarea SPU) llama al módulo cargado en LS 0x32980: inicialización `0x32990` una vez (r3 = 0xf750; devuelve 0x38f60, 0x65c8) y decodificación `0x36c58` en bucle.
- Decodificación: r3 0x12a00, r4 0x1aa00, r5 0x22a00 (tres búferes de 0x8000), r6 0x400, r7 0x2eb80 (contexto del stream copiado por el mezclador; +0x34 = EA del contexto en memoria principal, 0x30b58220/320/420 = tres streams), r8 0x3c200, r9 0x2ea80 (EAs en memoria principal: el módulo hace su propio DMA de la trama comprimida). Devuelve r3 = {muestras (0x200 o 0), canales 2, 0x98, 0}.
- Salida: float de 32 bits, 512 muestras por canal: canal 0 en LS 0x1aa00, canal 1 en 0x1ba00 (0x808 bytes cambiados en cada uno).
- Se decodifican varios streams a la vez (música y ambientes, p. ej. wind_loop_stereo.msa).
- Depuración: `CODECLOG=n` (entradas/salidas del módulo, gancho `Spu::onRegion3`), `CODECDUMP=primera,n` (LS completo antes y después de cada llamada -> codec_NNN_in/out.bin).
- Siguiente: estructuras r7/r9 (posición de lectura, ring buffer, estado entre llamadas), luego portar el decodificador ATRAC3 (referencia: FFmpeg atrac3.c, LGPL) y comparar muestra a muestra con los volcados.

### Decodificador de música propio (15/11, fases 2-3)
- `src/recomp/atrac3.{h,cpp}`: ATRAC3 (modo no conjunto) adaptado de FFmpeg (LGPL). Idéntico a FFmpeg 9 (0,03 LSB; ojo: FFmpeg antiguo calculaba mal `num_bands`, se usa `subband_tab[nsub + 1]`). IMDCT con simetría: 18 µs por canal y trama. `atrac3_test.exe fichero.msa salida.f32` (+ `A3DEBUG=n`).
- Protocolo real (lv2.cpp `atrac_compute`): por voz, no global. Contexto r7 +0x20: byte alto 2 = sonido nuevo (resto 1; segundo byte = canales del stream, 2 o 6), +0x34 = identidad de la voz. Inicio: decodifica la trama solo para cebar, sale silencio + primera mitad, devuelve 0x200. Luego alterna: llamada con trama (decodifica, sale 2.ª mitad anterior + 1.ª nueva, devuelve 0) y llamada sin trama (sale la trama entera, devuelve 0x200). 1024 muestras int16 BE con polaridad invertida por canal (r4, r4+0x1000) + 4 extrapoladas con desbordamiento de 16 bits.
- Comprobación en partida real (`ATRACCHECK=1`, Sony decodifica y se compara): 0 retornos erróneos, peor diferencia 10 LSB, 0,13 % de muestras con más de 2 LSB.
- Sin flashATRAC.pic el juego usa el nuestro (rama tg==0). `ATRACHLE=1` lo fuerza aunque esté el firmware (`build-test/probar_musica_propia.bat`).
- RESUELTO (15/11): música «acelerada» = el mezclador lee un bloque de resultado que deja el módulo en r8 + 0x8d0 (r3 en +0..+0xc, +0x10 = dirección de la última unidad decodificada, +0x14 = bits leídos de ella) para avanzar su lectura del stream; sin él leía tramas viejas. Escrito igual que Sony (0 diferencias en 29.960 llamadas).
- RESUELTO (15/11): el «volumen extra» era ruido: las voces de 6 canales (música de carrera) pasan las 6 unidades y el módulo escribe los 6 canales en r4 + c * 0x1000; solo hacíamos 2 y el mezclador leía datos viejos en los otros 4. Ahora mismo nivel que Sony y peor diferencia 16 LSB.
- Herramientas de diagnóstico: ATRACSTAT, ATRACCHECK=1/2 (2 = sobrescribe con la nuestra), ATRACUNDO=lo-hi, ATRACREGS, ATRACDELAY=us, ATRACNOPUT, CODECLOG/CODECDUMP/CODECMFC.

### Edge nativo, etapa 1: descarte de triángulos (17/11)
- El descarte de triángulos de Edge (LS 0x9E68, llamado desde 0x43FC, vuelve a 0x4400) era el 57 % de cada trabajo Edge (13-17 µs de 23-27 µs en carrera). Ahora se hace en C++: `src/recomp/edge_cull.cpp`.
- Gancho genérico: el despachador del código SPU recompilado llama a `spurt::g_spuHle[(ea >> 4) % 64](r, s, pc)` en cada cambio de trozo (`tools/spu2cpp.py`, `spu_rt.h`). Ojo: 0x4400 es también frontera de trozo (emparejar entrada/salida por trabajo).
- Contrato: r3 = ctx (0x3FD70): +8 = nº vértices << 16 | nº índices, +0x10 posiciones float4, +0x14 salida por vértice (solo interna), +0x50 índices u16 (se compactan en su sitio), +0x58 fin de la lista (= inicio + múltiplo de 16 de 2·n). r4 = modo (0: sin descarte, lo hace la SPU; 1: sin caras traseras; 3: con caras traseras). Devuelve r3 = nº de índices en las 4 palabras y lo guarda en r1 + 0x258.
- Reglas (medidas sobre capturas, 0 excepciones): c = M·L·p (entrada 15: viewport con tijera u16, matriz 4×4 en +0x10, escala +0x50, desplazamiento +0x60; entrada 16: local→mundo 3×4). Bits: 0x04 z > w, 0x80 z < 0, con w > 0: 0x08 y > alto, 0x10 x > ancho, 0x20 y < 0, 0x40 x < 0. Triángulo: AND de bits → fuera; algún 0x80 → se queda; modo 3 y área float < 0 → fuera; tres X o tres Y ajustadas iguales (trunc(8·v) & ~1) → fuera.
- La aritmética no es la de la SPU (estimación de recíproco, orden float32), así que lo dudoso se conserva: superconjunto del resultado de la consola (+4,7 % de triángulos). Cota de error con |M|·|L|·|p| (con la matriz combinada fallaba en coches).
- Verificado: `EDGECHECK=1` (o `=carpeta` para guardar fallos) 13,4 M trabajos sin perder un triángulo; `EDGEPOISON=1` (datos por vértice basura) se ve igual. Carrera: trabajo Edge 27,0 → 17,7 µs, descarte 17 → 7,5 µs.
- Herramientas: `CULLCAP=carpeta,segundos,n` (capturas), `CULLTIME=1` ([culltime]), `build-test/cull_replay.exe carpeta` (repite la rutina SPU; `NATIVE=1` la nativa y comprueba superconjunto; `INTERP=1`, `BLOCKS=k`, `REPEAT=n`). `EDGENATIVE=0` vuelve al código SPU (`build-test/probar_edge_antiguo.bat`).
- Siguiente: el resto del trabajo Edge (~10 µs: descompresión de índices/vértices, salida 0x7140 = trozo 0x7000).

### Edge nativo, etapa 2a: atajos y memoria local por binario (17/11)
- Medido en carrera a 72 fps (trabajo Edge ~18 µs): descarte nativo 7-8 µs (5 µs en `cull_replay`, la diferencia es SMT/caché fría), preparación + GET + PUT ~2 µs, resto del código SPU ~8 µs. Bloques SPU calientes: 0x72fc (9 % del tiempo activo de los hilos), 0x12e94 y 0x12cf0 (5 % cada uno), 0xf830 (3 %), 0x5000 (2 %); ~6 % es la entrada de los trozos (despacho).
- El 53 % de los vértices está en trabajos que acaban con todo descartado (213 de 1000, los grandes). `box_outside` (edge_cull.cpp): caja de las posiciones (w = 1, sin NaN), 8 esquinas transformadas en double y la cota de error de cada vértice acotada sobre la caja (margen ×2); si un mismo plano (lejos, cerca, o un borde de pantalla con w > 0) deja fuera las 8 esquinas, se descartan todos sin pasar por los vértices. Además, tras la pasada de vértices, si todos comparten un bit de fuera se salta la de triángulos. Captura de 1000 trabajos: 112 de los 213 por atajo, 0 diferencias; en el juego `EDGECHECK=1` 8,4 M trabajos sin perder un triángulo. `EDGEBOX=0` los desactiva. Descarte 7,0-7,7 → 6,3-6,7 µs.
- El despachador (0xba3680) se intercala con los trabajos Edge en cada hilo: con una sola memoria local, el binario Edge (~64 KB) se copiaba de nuevo en 1 de cada 3 trabajos. Ahora cada hilo tiene una memoria local por binario (hasta 4; `JOBONELS=1` = antes). También fuera el mutex global + `std::map` por trabajo (solo la primera vez que un hilo ve un binario). CPU total en carrera −5-7 % (p. ej. 568 → 527 % de un núcleo en el tramo más cargado).
- `cull_replay` con `CULLSTAT=1`: trabajos descartados enteros y cuántos por atajo.
