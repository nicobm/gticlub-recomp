# Racing Club+ para PC — instalador

Este instalador **no contiene ningún fichero del juego**. Fabrica la versión de PC en tu ordenador a partir de **tu propia copia** de GTI Club+ Rally Côte d'Azur de PS3.

## Qué necesitas

1. **Los paquetes `.pkg` de tu juego, tal como los compraste en PSN**
   - El juego, versión **europea** (**NPEB00060**), con su licencia `.rap` **en la misma carpeta** que el `.pkg` (el instalador la busca sola).
   - La **actualización 1.02** (`.pkg`).
   - Opcional: el pack de coches (DLC, `.pkg`; no necesita licencia).
   - Otras regiones (americana, japonesa) u otras versiones **no funcionan**: el instalador lo comprueba y se detiene.
2. **No hace falta ningún emulador ni el firmware de PS3**: el instalador extrae los paquetes y descifra el juego por sí mismo, y el decodificador de música va integrado.
3. **Python 3** con el módulo `capstone` 5.0.7: va incluido en `toolchain/python` si usas el paquete completo. Si no está, `instalar.bat` usa el Python del sistema e instala `capstone`.
4. **El compilador**: carpeta `toolchain/` junto a este fichero (llvm-mingw + CMake + Ninja, con SDL3 y zlib en `toolchain/deps`), o indicada con `--toolchain`.
5. Un PC con Windows 10/11 de 64 bits, procesador con AVX2 y gráfica con OpenGL 4.5. Unos 3 GB libres.

## Cómo se usa

Haz doble clic en **`Instalar Racing Club+.exe`**: se abre el instalador, eliges el `.pkg` del juego, el de la actualización y (si lo tienes) el del DLC, dónde instalar y el idioma del juego, y pulsas «Instalar». También hay una versión de consola (`instalar.bat`) o:

```
python instalar.py --pkg "juego.pkg" "actualizacion 1.02.pkg" ["dlc.pkg"]
```

Tarda varios minutos (traducir y compilar). Al terminar, la carpeta `Racing Club+` contiene `Racing Club+.exe` y una copia de los datos de tu juego. Las partidas guardadas y los ajustes se crean ahí mismo.

## Qué hace, paso a paso

1. Extrae los paquetes (juego, actualización encima y DLC) y comprueba que son NPEB00060 1.02.
2. Descifra el ejecutable del juego con tu licencia `.rap`.
3. Analiza el código y lo traduce a C++.
4. Traduce los programas de sonido y geometría.
5. Compila todo junto con el motor de este proyecto (gráficos, sonido, mando, menú).
6. Copia el resultado y los datos del juego a la carpeta de salida.

## Importante

La carpeta `Racing Club+` resultante contiene tu copia del juego y código derivado de él: es para tu uso personal. **No la repartas.** Lo que sí se puede compartir es este instalador.

## Jugar online

Conexión directa entre jugadores, sin servidores ni cuenta de PSN: quien crea la partida la aloja en su juego. En la misma red local las partidas se encuentran solas; por internet, quien se une escribe la IP del anfitrión en **F1 > Online**, y el anfitrión necesita el puerto **3658 (TCP y UDP)** abierto en su router (el juego lo intenta abrir solo si el router tiene UPnP). Los pasos completos están en la pestaña **«Jugar online»** del instalador.

## En el juego

- **F1** (o pulsar el panel táctil / botón PS del mando): menú de opciones.
- «Informe de problemas» crea un fichero con los datos del equipo y el registro, y abre la página de incidencias del proyecto.
