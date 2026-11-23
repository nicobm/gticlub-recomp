# Compilar e instalar Racing Club+ como Flatpak

`build-gti.sh` compila `gtiport.exe` para Windows con mingw-w64 y lo empaqueta
en un Flatpak que usa **Wine** de Flathub como aplicación base (`base app`).
El juego corre sobre Wine, dentro del sandbox de Flatpak.

> Compila el port principal (`gtiport.exe`). La recompilación completa
> (`recomp_boot`, que necesita descifrar el `EBOOT.BIN` con RPCS3) no forma
> parte de este script.

## Requisitos

- Fedora Silverblue, Bazzite o similar, con `toolbox` y `podman`.
- Tu copia de GTI Club+ Rally Côte d'Azur (NPEB00060) en XDG Documents o
  XDG Downloads. El script busca automáticamente un `.rar`/`.zip` que contenga
  `NPEB00060`; también puedes pasarle la ruta.
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

1. En el **host** comprueba los runtimes de Flathub. Si existe el `flathub`
   del sistema (Silverblue/Bazzite lo trae, aunque venga desactivado), lo usa
   para el runtime y la base app de Wine; el SDK y la base app para compilar se
   instalan en la instalación de **usuario** porque es la única que se comparte
   con Toolbox. Si no hay `flathub` de usuario, usa uno temporal y lo borra al
   terminar: no deja un `flathub` de usuario permanente.
2. Entra en un contenedor Toolbox (lo crea si no existe).
3. Pregunta antes de instalar con `sudo dnf` lo que falte:
   `cmake`, `ninja`, `mingw64-gcc`, `mingw64-SDL3-static`,
   `mingw64-zlib-static`, `flatpak-builder`, `unar`, etc.
4. Compila `build-gti/gtiport.exe`.
5. Extrae `NPEB00060/USRDIR/data` de tu archivo del juego. No copia el
   `EBOOT.BIN` ni el `shadercache`: solo los datos que usa el port.
6. Construye el Flatpak con `flatpak-builder --user` y lo instala desde el
   **host**. Si hay `flathub` del sistema, el juego queda instalado para todo
   el sistema (desde un bundle temporal); si no, en tu usuario con un remoto
   local (igual que senjin-recomped).

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

# Forzar instalación de usuario o de sistema:
GTI_FLATPAK_SCOPE=user bash build-gti.sh
GTI_FLATPAK_SCOPE=system bash build-gti.sh
```

`GTI_FLATPAK_SCOPE=auto` (lo normal) usa el `flathub` del sistema si existe;
si no, el de usuario.

## Desinstalar

Si se instaló en el sistema:

```sh
flatpak uninstall --system io.github.nicobm.GtiClubRecomp
rm -rf ~/.var/app/io.github.nicobm.GtiClubRecomp
```

Si se instaló en el usuario:

```sh
flatpak uninstall --user io.github.nicobm.GtiClubRecomp
flatpak remote-delete --user --force gticlub-local
rm -rf ~/.var/app/io.github.nicobm.GtiClubRecomp
```

El juego se instala con el `flathub` del sistema. Para compilar en Toolbox se
usan runtimes de **usuario**; si no existía un `flathub` de usuario, el script
añade `gticlub-build-flathub` solo para eso y lo deja desactivado. No crea un
`flathub` de usuario. Si una versión anterior del script te dejó uno, puedes
quitarlo cuando ya no necesites sus runtimes de compilación:

```sh
flatpak uninstall --user --unused
flatpak remote-delete --user flathub
```

## Aviso legal

El Flatpak resultante contiene datos de **tu propia copia** del juego. Es para
uso personal: no distribuyas el paquete ni la carpeta `game-data/`.
