# Compilar e instalar Racing Club+ como Flatpak

`build-gti.sh` compila `gtiport.exe` para Windows con mingw-w64 y lo empaqueta
en un Flatpak que usa **Wine** de Flathub como aplicación base (`base app`).
El juego corre sobre Wine, dentro del sandbox de Flatpak.

> Compila el port principal (`gtiport.exe`). La recompilación completa
> (`recomp_boot`, que necesita descifrar el `EBOOT.BIN` con RPCS3) no forma
> parte de este script.

## Requisitos

- Fedora Silverblue, Bazzite o similar, con `toolbox` y `podman`.
- Tu copia de GTI Club+ Rally Côte d'Azur (NPEB00060) en `~/Documentos`.
  El script busca automáticamente un `.rar`/`.zip` que contenga `NPEB00060`.
- Unos 5 GB libres (datos extraídos + compilación + Flatpak).

## Uso

```sh
bash build-gti.sh
```

También puedes pasarle el archivo del juego (`.rar` o `.zip`) o una carpeta ya
extraída. El nombre lleva espacios y apóstrofo, así que ponlo entre comillas
dobles:

```sh
bash build-gti.sh "GTI Club Rally Cote D'Azur PSN [NPEB-00060].rar"
bash build-gti.sh "GTI Club Rally Cote D'Azur PSN [NPEB-00060].zip"
bash build-gti.sh /ruta/a/NPEB00060
```

Si no pasas nada, se busca automáticamente en XDG Documents/Downloads
(`~/Documentos`, `~/Descargas`, etc.).

El script:

1. Entra en un contenedor Toolbox (lo crea si no existe).
2. Pregunta antes de instalar con `sudo dnf` lo que falte:
   `cmake`, `ninja`, `mingw64-gcc`, `mingw64-SDL3-static`,
   `mingw64-zlib-static`, `flatpak-builder`, `unar`, `p7zip`, etc.
3. Compila `build-gti/gtiport.exe`.
4. Extrae `NPEB00060/USRDIR/data` de tu archivo del juego. No copia el
   `EBOOT.BIN` ni el `shadercache`: solo los datos que usa el port.
5. Pregunta antes de instalar los runtimes de Flathub:
   `org.freedesktop.Platform//26.08`, `org.freedesktop.Sdk//26.08` y
   `org.winehq.Wine//stable-26.08`.
6. Construye el Flatpak con `flatpak-builder` y lo instala con un remoto local
   (igual que el paquete de senjin-recomped).

Al terminar:

```sh
flatpak run io.github.nicobm.GtiClubRecomp
```

También aparece en el menú de aplicaciones como **Racing Club+**.

## Datos y partidas

Los datos se copian la primera vez a:

```
~/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club/game
```

Las partidas y ajustes se quedan ahí. Para borrarlos:

```sh
rm -rf ~/.var/app/io.github.nicobm.GtiClubRecomp/data/racing-club
```

## Permisos del sandbox

- `--share=network`: necesario para conectarse online.
- `--nofilesystem=home` y `--nofilesystem=host`: **sin acceso** a tu carpeta
  personal ni al resto del sistema.
- `--socket=wayland`, `--socket=fallback-x11`, `--socket=pulseaudio`,
  `--device=dri`, `--device=input`: pantalla, sonido, GPU y mandos.

## Variables útiles

```sh
# Si el archivo del juego no está en ~/Documentos:
GTI_ROM='/ruta/al/juego.rar' bash build-gti.sh

# Si ya tienes los datos extraídos (carpeta NPEB00060):
GTI_DATA='/ruta/a/NPEB00060' bash build-gti.sh

# Elegir un contenedor Toolbox concreto:
GTI_TOOLBOX=fedora-toolbox-43 bash build-gti.sh
```

## Desinstalar

```sh
flatpak uninstall io.github.nicobm.GtiClubRecomp
flatpak remote-delete --user gticlub-local
rm -rf ~/.var/app/io.github.nicobm.GtiClubRecomp
```

## Aviso legal

El Flatpak resultante contiene datos de **tu propia copia** del juego. Es para
uso personal: no distribuyas el paquete ni la carpeta `game-data/`.
