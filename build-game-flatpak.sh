#!/usr/bin/env bash
# build-game-flatpak.sh — construye e instala Racing Club+ como Flatpak.
#
# El paquete de Patreon ya trae el juego compilado (Racing Club+.exe + juego/),
# así que este wrapper no compila código del juego: valida ese paquete y lo
# empaqueta con flatpak-builder sobre la base org.winehq.Wine. La lógica del
# wrapper (Toolbox, runtimes, remoto local, --debug) es la misma que usan
# senjin-recomped y otomedius-recomp.
#
# Uso:
#   ./build-game-flatpak.sh [--debug]
#   ./build-game-flatpak.sh [--debug] "/ruta/Racing Club+ Instalador.zip"
#   ./build-game-flatpak.sh [--debug] "/ruta/Racing Club+"
#
# Si no pasás nada, busca el paquete de Patreon y el ROM en XDG
# Downloads/Documents. El ROM es opcional: el paquete de Patreon ya trae el
# juego compilado. Los argumentos pueden venir en cualquier orden.
#
# En Fedora Atomic (Silverblue, Bazzite) el script se reejecuta dentro del
# contenedor Toolbox "gticlub-recomp", creándolo si hace falta; dentro instala
# flatpak/flatpak-builder con dnf si faltan. Usá GTI_TOOLBOX=otro-nombre para
# elegir otro contenedor. Sin Toolbox (Debian, Arch, Alpine, Fedora normal,
# ...) no toca el host: imprime el comando del gestor de paquetes detectado.
#
# --debug instala también el runtime <app-id>.Debug que flatpak-builder separa
# del build. Solo sirve con gdb; no hace falta para jugar, por eso no se
# instala por defecto.
#
# El resultado se exporta al directorio persistente repo/ y se instala desde
# un único remoto local, "gticlub-local", que queda desactivado: como la app
# se construye localmente, `flatpak update` no tiene nada que descargar. Para
# actualizarla, volvé a ejecutar este script.
set -Eeuo pipefail

APP_ID="io.github.nicobm.GtiClubRecomp"
DEBUG_ID="$APP_ID.Debug"
LOCAL_REMOTE="gticlub-local"
EXPECTED_EBOOT_SHA1="e9faf331b52770164be0c19f427c10aef747776d"

SCRIPT_PATH="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/$(basename -- "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname -- "$SCRIPT_PATH")"
REPO_DIR="${GTI_REPO:-$SCRIPT_DIR}"
PREBUILT_DIR="$REPO_DIR/prebuilt"
MANIFEST="$REPO_DIR/$APP_ID.json"

RUNTIME_REF="org.freedesktop.Platform//26.08"
SDK_REF="org.freedesktop.Sdk//26.08"
WINE_REF="org.winehq.Wine//stable-26.08"

die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }
say() { printf '\n== %s\n' "$*"; }

usage() {
  cat <<'EOF'
Uso:
  ./build-game-flatpak.sh [--debug]
  ./build-game-flatpak.sh [--debug] "/ruta/Racing Club+ Instalador.zip"
  ./build-game-flatpak.sh [--debug] "/ruta/Racing Club+"

  # El .rar/.zip del ROM es opcional; se acepta en cualquier orden:
  ./build-game-flatpak.sh "/ruta/Racing Club+ Instalador.zip" "/ruta/GTI Club...rar"
  ./build-game-flatpak.sh "/ruta/GTI Club...rar" "/ruta/Racing Club+ Instalador.zip"

El paquete de Patreon ya contiene el juego compilado. El script lo empaqueta
en un Flatpak y lo ejecuta con Wine. No compila ni usa RPCS3.

Sin argumentos busca el paquete de Patreon y el ROM en XDG Downloads/Documents.
El ROM no se usa si el paquete ya trae el juego compilado; se acepta para que
puedas pasarlo igual.

Variables de entorno:
  GTI_TOOLBOX          nombre del contenedor Toolbox (gticlub-recomp)
  GTI_PREBUILT         ruta al zip/carpeta del paquete de Patreon
  GTI_ROM              ruta al .rar/.zip del ROM (opcional)
  GTI_REPO             raíz del repositorio (por defecto, la del script)
EOF
}

# --------------------------------------------------------------- Toolbox

in_toolbox() {
  [ "${GTI_IN_TOOLBOX:-0}" = 1 ] && return 0
  [ -f /run/.toolboxenv ] && return 0
  return 1
}

in_container() {
  [ -f /run/.containerenv ] && return 0
  [ -f /.dockerenv ] && return 0
  return 1
}

toolbox_has_container() {
  toolbox list --containers 2>/dev/null |
    awk -v name="$1" '$2 == name { found = 1 } END { exit !found }'
}

# Sistemas sin Toolbx (Debian, Arch, Alpine, Fedora normal, ...): no tocar el
# host, solo explicar el comando del gestor de paquetes local.
print_install_hint() {
  local os_name
  os_name="$(
    if [ -r /etc/os-release ]; then
      # shellcheck disable=SC1091
      . /etc/os-release
      printf '%s' "${PRETTY_NAME:-${NAME:-your distribution}}"
    else
      printf '%s' "your distribution"
    fi
  )"
  echo "  Detectado: $os_name" >&2
  if command -v rpm-ostree >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    sudo rpm-ostree install flatpak flatpak-builder" >&2
    echo "  (o instalá Toolbx: sudo rpm-ostree install toolbox, y reiniciá)" >&2
  elif command -v apt >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    sudo apt install flatpak flatpak-builder" >&2
  elif command -v apt-get >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    sudo apt-get install flatpak flatpak-builder" >&2
  elif command -v dnf >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    sudo dnf install flatpak flatpak-builder" >&2
  elif command -v yum >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    sudo yum install flatpak flatpak-builder" >&2
  elif command -v pacman >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    sudo pacman -S flatpak flatpak-builder" >&2
  elif command -v zypper >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    sudo zypper install flatpak flatpak-builder" >&2
  elif command -v apk >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    sudo apk add flatpak flatpak-builder" >&2
  elif command -v xbps-install >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    sudo xbps-install -S flatpak flatpak-builder" >&2
  elif command -v eopkg >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    sudo eopkg install flatpak flatpak-builder" >&2
  elif command -v emerge >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    sudo emerge --ask sys-apps/flatpak dev-util/flatpak-builder" >&2
  elif command -v nix-env >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    nix-env -iA nixpkgs.flatpak nixpkgs.flatpak-builder" >&2
  elif command -v guix >/dev/null 2>&1; then
    echo "  Instalalos con:" >&2
    echo "    guix install flatpak flatpak-builder" >&2
  else
    echo "  Instalá los paquetes 'flatpak' y 'flatpak-builder' con el gestor" >&2
    echo "  de paquetes de tu distribución." >&2
  fi
  echo "  Después volvé a ejecutar este script." >&2
}

# 1. Argumentos. --debug instala el runtime de símbolos; el resto son rutas
#    del paquete de Patreon y, opcionalmente, del ROM. Los argumentos se
#    clasifican después, así que pueden venir en cualquier orden. Se leen
#    antes de entrar al Toolbox para que --help no necesite Flatpak.
WITH_DEBUG=0
RAW_ARGS=()
while (($# > 0)); do
  case "$1" in
    --debug) WITH_DEBUG=1 ;;
    --)
      shift
      RAW_ARGS+=("$@")
      break
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*) die "Opción no reconocida: $1" ;;
    *) RAW_ARGS+=("$1") ;;
  esac
  shift
done

# En Fedora Atomic, flatpak-builder vive en un Toolbox. El --assumeyes global
# hace que `toolbox create` descargue la imagen Fedora sin preguntar. La
# instancia interna instala flatpak-builder con dnf si falta.
if ! in_toolbox && ! in_container &&
   { ! command -v flatpak >/dev/null 2>&1 ||
     ! command -v flatpak-builder >/dev/null 2>&1; }; then
  if ! command -v toolbox >/dev/null 2>&1; then
    echo "error: falta flatpak y/o flatpak-builder, y toolbox no está disponible." >&2
    print_install_hint
    exit 1
  fi
  TOOLBOX_NAME="${GTI_TOOLBOX:-gticlub-recomp}"
  if ! toolbox_has_container "$TOOLBOX_NAME"; then
    echo "Creando el contenedor Toolbox $TOOLBOX_NAME..."
    echo "  (la imagen Fedora de Toolbox se descarga automáticamente si hace falta)"
    toolbox --assumeyes create --container "$TOOLBOX_NAME"
  fi
  echo "Entrando en el contenedor Toolbox $TOOLBOX_NAME..."
  exec toolbox run --container "$TOOLBOX_NAME" -- \
    env GTI_IN_TOOLBOX=1 "$SCRIPT_PATH" "$@"
fi

cd "$SCRIPT_DIR"

# 1. Herramientas de Flatpak. Dentro del Toolbox se instala lo que falte:
#    Fedora Atomic no puede instalarlo con dnf en el host, y el Toolbox tiene
#    sudo sin contraseña.
install_flatpak_packages() {
  if [ "$(id -u)" -eq 0 ]; then
    dnf install -y flatpak flatpak-builder
  elif sudo dnf install -y flatpak flatpak-builder; then
    :
  else
    # Toolbx deja la contraseña vacía. Si `toolbox run` no tiene TTY,
    # sudo -S acepta igualmente un salto de línea sin preguntar.
    printf '\n' | sudo -S dnf install -y flatpak flatpak-builder
  fi
}

if ! command -v flatpak >/dev/null 2>&1 ||
   ! command -v flatpak-builder >/dev/null 2>&1; then
  if in_toolbox && command -v dnf >/dev/null 2>&1; then
    echo "Instalando flatpak y flatpak-builder en el Toolbox..."
    if ! install_flatpak_packages; then
      echo "error: no se pudo instalar flatpak/flatpak-builder automáticamente." >&2
      print_install_hint
      exit 1
    fi
  fi
fi
command -v flatpak >/dev/null 2>&1 || {
  echo "error: flatpak no está instalado." >&2
  print_install_hint
  exit 1
}
command -v flatpak-builder >/dev/null 2>&1 || {
  echo "error: flatpak-builder no está instalado." >&2
  print_install_hint
  exit 1
}
command -v python3 >/dev/null 2>&1 || {
  echo "error: hace falta python3 para preparar el paquete y el remoto local." >&2
  exit 1
}

# ---------------------------------------------------------- rutas de búsqueda

XDG_SEARCH_DIRS=()

build_search_dirs() {
  local -a dirs=()
  local d key val conf
  if command -v xdg-user-dir >/dev/null 2>&1; then
    for key in DOCUMENTS DOWNLOAD; do
      d="$(xdg-user-dir "$key" 2>/dev/null || true)"
      if [[ -n "$d" && "$d" != "$HOME" && -d "$d" ]]; then
        dirs+=("$d")
      fi
    done
  fi
  conf="$HOME/.config/user-dirs.dirs"
  if [[ -f "$conf" ]]; then
    while IFS='=' read -r key val; do
      case "$key" in
        XDG_DOCUMENTS_DIR|XDG_DOWNLOAD_DIR)
          val="${val%\"}"; val="${val#\"}"
          val="${val/#\$HOME/$HOME}"
          val="${val/#\$\{HOME\}/$HOME}"
          if [[ -d "$val" && "$val" != "$HOME" ]]; then
            dirs+=("$val")
          fi
          ;;
      esac
    done < "$conf"
  fi
  for d in "$HOME/Descargas" "$HOME/Downloads" "$HOME/Documentos" "$HOME/Documents"; do
    [[ -d "$d" ]] && dirs+=("$d")
  done
  local seen=""
  for d in "${dirs[@]}"; do
    case ":$seen:" in *":$d:"*) continue ;; esac
    seen="$seen:$d"
    XDG_SEARCH_DIRS+=("$d")
  done
}

resolve_path() {
  local p="$1" dir base
  [[ -n "$p" ]] || return 1
  if [[ -e "$p" ]]; then
    readlink -f -- "$p"
    return 0
  fi
  for dir in "$PWD" ${XDG_SEARCH_DIRS[@]+"${XDG_SEARCH_DIRS[@]}"}; do
    [[ -e "$dir/$p" ]] && { readlink -f -- "$dir/$p"; return 0; }
  done
  base="$(basename -- "$p")"
  if [[ "$base" != "$p" ]]; then
    for dir in "$PWD" ${XDG_SEARCH_DIRS[@]+"${XDG_SEARCH_DIRS[@]}"}; do
      [[ -e "$dir/$base" ]] && { readlink -f -- "$dir/$base"; return 0; }
    done
  fi
  return 1
}

PATREON_SOURCE=""
ROM_SOURCE=""

classify_path() {
  local p="$1" found kind
  if [[ -d "$p" ]]; then
    if [[ -f "$p/Racing Club+.exe" ]]; then
      PATREON_SOURCE="$p"
      return 0
    fi
    found="$(find "$p" -maxdepth 3 -type f -path '*/Racing Club+/Racing Club+.exe' -print -quit 2>/dev/null || true)"
    if [[ -n "$found" ]]; then
      PATREON_SOURCE="$(dirname -- "$found")"
      return 0
    fi
    if [[ -d "$p/NPEB00060" ]] ||
       [[ -n "$(find "$p" -maxdepth 3 -type d -name 'NPEB00060' -print -quit 2>/dev/null || true)" ]] ||
       [[ -n "$(find "$p" -maxdepth 3 -type f \( -iname '*.pkg' -o -iname '*.r00' -o -iname '*NPEB*' \) -print -quit 2>/dev/null || true)" ]]; then
      ROM_SOURCE="$p"
      return 0
    fi
    return 1
  fi

  [[ -f "$p" ]] || return 1
  case "$p" in
    *.rar|*.RAR)
      ROM_SOURCE="$p"
      return 0
      ;;
    *.zip|*.ZIP)
      kind="$(python3 - "$p" <<'PY'
import sys, zipfile
try:
    with zipfile.ZipFile(sys.argv[1]) as z:
        names = [n.replace("\\", "/") for n in z.namelist()]
except Exception:
    print("unknown")
    raise SystemExit
patreon = any(n.endswith("Racing Club+/Racing Club+.exe") or n.endswith("/Racing Club+.exe") for n in names)
rom = any("NPEB" in n.upper() or n.lower().endswith(".pkg") or "GTI CLUB" in n.upper() or n.lower().endswith(".r00") for n in names)
if patreon:
    print("patreon")
elif rom:
    print("rom")
else:
    print("unknown")
PY
)"
      case "$kind" in
        patreon) PATREON_SOURCE="$p"; return 0 ;;
        rom) ROM_SOURCE="$p"; return 0 ;;
        *) return 1 ;;
      esac
      ;;
  esac
  return 1
}

auto_detect_sources() {
  local dir found

  if [[ -z "$PATREON_SOURCE" && -f "$PREBUILT_DIR/Racing Club+.exe" ]]; then
    PATREON_SOURCE="$PREBUILT_DIR"
  fi

  if [[ -z "$PATREON_SOURCE" ]]; then
    for dir in ${XDG_SEARCH_DIRS[@]+"${XDG_SEARCH_DIRS[@]}"}; do
      [[ -d "$dir" ]] || continue
      found="$(find "$dir" -maxdepth 2 -type f -iname 'Racing Club+*.zip' -print -quit 2>/dev/null || true)"
      if [[ -n "$found" ]]; then
        PATREON_SOURCE="$found"
        break
      fi
    done
  fi

  if [[ -z "$PATREON_SOURCE" ]]; then
    for dir in ${XDG_SEARCH_DIRS[@]+"${XDG_SEARCH_DIRS[@]}"}; do
      [[ -d "$dir" ]] || continue
      found="$(find "$dir" -maxdepth 3 -type f -path '*/Racing Club+/Racing Club+.exe' -print -quit 2>/dev/null || true)"
      if [[ -n "$found" ]]; then
        PATREON_SOURCE="$(dirname -- "$found")"
        break
      fi
    done
  fi

  if [[ -z "$ROM_SOURCE" ]]; then
    for dir in ${XDG_SEARCH_DIRS[@]+"${XDG_SEARCH_DIRS[@]}"}; do
      [[ -d "$dir" ]] || continue
      found="$(find "$dir" -maxdepth 2 -type f \
        \( -iname '*NPEB*00060*.rar' -o -iname '*NPEB*00060*.zip' \
           -o -iname '*GTI*Club*.rar' -o -iname '*GTI*Club*.zip' \) \
        -print -quit 2>/dev/null || true)"
      if [[ -n "$found" && "$found" != "$PATREON_SOURCE" ]]; then
        ROM_SOURCE="$found"
        break
      fi
    done
  fi
}

parse_inputs() {
  PATREON_SOURCE=""
  ROM_SOURCE=""
  local -a raw=("$@")
  local i j cand res found

  if [[ -n "${GTI_PREBUILT:-}" ]]; then
    res="$(resolve_path "$GTI_PREBUILT" || true)"
    [[ -n "$res" ]] || die "GTI_PREBUILT no existe: $GTI_PREBUILT"
    classify_path "$res" ||
      die "GTI_PREBUILT no es un paquete de Patreon ni un ROM válido: $res"
  fi

  if [[ -n "${GTI_ROM:-}" ]]; then
    res="$(resolve_path "$GTI_ROM" || true)"
    [[ -n "$res" ]] || die "GTI_ROM no existe: $GTI_ROM"
    if ! classify_path "$res"; then
      ROM_SOURCE="$res"
    fi
  fi

  if ((${#raw[@]} > 0)); then
    i=0
    while ((i < ${#raw[@]})); do
      found=0
      for ((j=${#raw[@]}-1; j>=i; j--)); do
        cand="${raw[*]:i:j-i+1}"
        res="$(resolve_path "$cand" || true)"
        if [[ -n "$res" ]] && classify_path "$res"; then
          i=$((j + 1))
          found=1
          break
        fi
      done
      if ((found == 0)); then
        res="$(resolve_path "${raw[i]}" || true)"
        if [[ -n "$res" ]] && classify_path "$res"; then
          i=$((i + 1))
          found=1
        fi
      fi
      if ((found == 0)); then
        die "No reconozco el archivo: ${raw[i]}
Pasá el zip de Patreon (con Racing Club+/Racing Club+.exe) y/o el .rar/.zip del ROM.
Si el nombre tiene espacios o apóstrofo, ponelo entre comillas dobles."
      fi
    done
  fi

  auto_detect_sources

  [[ -n "$PATREON_SOURCE" ]] || die "No encuentro el paquete de Patreon con Racing Club+/Racing Club+.exe.
El .rar/.zip del ROM solo no alcanza para el juego completo: necesitás el paquete de Patreon (o el EBOOT.elf/prebuilt).
Pasalo así:
  ./build-game-flatpak.sh \"/ruta/Racing Club+ Instalador.zip\" \"/ruta/GTI Club...rar\""

  printf 'Paquete de Patreon: %s\n' "$PATREON_SOURCE"
  if [[ -n "$ROM_SOURCE" ]]; then
    printf 'ROM del juego: %s\n' "$ROM_SOURCE"
    printf '  (no se usa: el paquete de Patreon ya trae el juego compilado y los datos)\n'
  fi
}

# ---------------------------------------------------- paquete del juego

extract_prebuilt_from_zip() {
  local zip="$1"
  rm -rf "$PREBUILT_DIR"
  mkdir -p "$PREBUILT_DIR"
  say "Extrayendo el juego compilado de $(basename "$zip")"
  python3 - "$zip" "$PREBUILT_DIR" <<'PY'
import os
import shutil
import sys
import zipfile

src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as z:
    names = z.namelist()
    marker = None
    for n in names:
        nn = n.replace("\\", "/")
        if nn.endswith("Racing Club+/Racing Club+.exe"):
            marker = nn[:-len("Racing Club+.exe")]
            break
    if marker is None:
        sys.exit("ERROR: no encuentro Racing Club+/Racing Club+.exe en el zip")

    count = 0
    for info in z.infolist():
        name = info.filename.replace("\\", "/")
        if not name.startswith(marker):
            continue
        rel = name[len(marker):]
        if not rel or rel.endswith("/"):
            continue
        out = os.path.join(dst, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with z.open(info) as r, open(out, "wb") as w:
            shutil.copyfileobj(r, w)
        mode = (info.external_attr >> 16) & 0o777
        if mode:
            os.chmod(out, mode | 0o600)
        if rel.endswith(".exe") or mode & 0o111:
            os.chmod(out, os.stat(out).st_mode | 0o111)
        count += 1
    print("  %d archivos" % count)
PY
}

copy_prebuilt_from_dir() {
  local src="$1"
  say "Copiando el juego compilado desde $src"
  rm -rf "$PREBUILT_DIR"
  mkdir -p "$PREBUILT_DIR"
  cp -a "$src/." "$PREBUILT_DIR/"
}

tune_prebuilt_ini() {
  local ini="$PREBUILT_DIR/gtiport.ini"
  [[ -f "$ini" ]] || return 0
  # El ini del autor viene en 4K/fullscreen/72 Hz; en otra GPU eso puede
  # quedar en negro o comer CPU. Arrancamos en ventana 720p/60 y se cambia
  # desde el menú F1 del juego.
  #
  # AA/sharpen en 1 fuerza el present por shader. Con ambos en 0 se usa
  # glBlitFramebuffer directo, que en AMD/Mesa (y llvmpipe) deja la ventana
  # blanca; con el shader se ven los logos (aunque el hardware AMD siga
  # fallando por el renderer RSX).
  sed -i \
    -e 's/^pantalla_completa=.*/pantalla_completa=0/' \
    -e 's/^resolucion_interna=.*/resolucion_interna=1/' \
    -e 's/^imagenes_por_segundo=.*/imagenes_por_segundo=60/' \
    -e 's/^pantalla_120hz=.*/pantalla_120hz=0/' \
    -e 's/^antialiasing=.*/antialiasing=1/' \
    -e 's/^nitidez=.*/nitidez=1/' \
    -e 's/^filtrado_anisotropo=.*/filtrado_anisotropo=0/' "$ini"
}

verify_prebuilt() {
  [[ -f "$PREBUILT_DIR/Racing Club+.exe" ]] ||
    die "Falta $PREBUILT_DIR/Racing Club+.exe."
  [[ -f "$PREBUILT_DIR/juego/EBOOT.elf" ]] ||
    die "Falta $PREBUILT_DIR/juego/EBOOT.elf."
  [[ -d "$PREBUILT_DIR/juego/USRDIR/data/cars" ]] ||
    die "Falta $PREBUILT_DIR/juego/USRDIR/data/cars."
  [[ -d "$PREBUILT_DIR/juego/USRDIR/shadercache" ]] ||
    die "Falta $PREBUILT_DIR/juego/USRDIR/shadercache."

  local sha
  sha="$(sha1sum "$PREBUILT_DIR/juego/EBOOT.elf" | cut -d' ' -f1)"
  [[ "$sha" == "$EXPECTED_EBOOT_SHA1" ]] ||
    die "El EBOOT.elf no es el oficial 1.02 (SHA1 $sha, esperado $EXPECTED_EBOOT_SHA1)."

  local exe_size
  exe_size="$(stat -c %s "$PREBUILT_DIR/Racing Club+.exe")"
  say "Juego compilado listo: $PREBUILT_DIR"
  printf '  Racing Club+.exe: %d MB\n' "$((exe_size / 1024 / 1024))"
  printf '  EBOOT.elf: SHA1 correcto\n'
  printf '  Datos y shadercache: presentes\n'

  tune_prebuilt_ini
}

prepare_prebuilt() {
  local path="$1"
  if [[ -d "$path" && -d "$PREBUILT_DIR" &&
        "$(readlink -f -- "$path")" == "$(readlink -f -- "$PREBUILT_DIR")" ]]; then
    verify_prebuilt
    return 0
  fi
  if [[ -d "$path" ]]; then
    if [[ -f "$path/Racing Club+.exe" ]]; then
      copy_prebuilt_from_dir "$path"
    elif [[ -f "$path/Racing Club/Racing Club+.exe" ]]; then
      copy_prebuilt_from_dir "$path/Racing Club"
    else
      die "$path no contiene Racing Club+/Racing Club+.exe."
    fi
  elif [[ -f "$path" ]]; then
    case "$path" in
      *.zip|*.ZIP) extract_prebuilt_from_zip "$path" ;;
      *) die "El paquete debe ser un .zip de Patreon o una carpeta Racing Club+." ;;
    esac
  else
    die "No encuentro $path."
  fi
  verify_prebuilt
}

# ------------------------------------------------------- runtimes de Flatpak

flatpak_ref_installed() {
  flatpak info "$1" >/dev/null 2>&1
}

flatpak_has_remote() {
  # `flatpak remotes` oculta los remotos desactivados salvo con
  # --show-disabled, y gticlub-local queda desactivado entre builds. Sin eso,
  # una segunda ejecución no encontraría el remoto y `remote-add` fallaría con
  # "Remote gticlub-local already exists". Flatpak antiguos no tienen
  # --show-disabled: se usa la lista normal como respaldo.
  local remotes
  remotes=$(flatpak remotes "$1" --show-disabled --columns=name 2>/dev/null) ||
    remotes=$(flatpak remotes "$1" --columns=name 2>/dev/null) || return 1
  printf '%s\n' "$remotes" | grep -qx "$2"
}

install_flatpak_ref() {
  local ref="$1" remote
  flatpak_ref_installed "$ref" && return 0

  # Preferir la instalación de usuario: no necesita root y es la misma que usa
  # flatpak-builder --user.
  for remote in flathub flathub-verified_floss flathub-floss fedora; do
    if flatpak_has_remote --user "$remote"; then
      echo "Instalando $ref desde $remote (usuario)..."
      if flatpak install -y --user "$remote" "$ref"; then
        flatpak_ref_installed "$ref" && return 0
      fi
    fi
  done

  # Fedora suele configurar sus remotos a nivel de sistema, y --user no puede
  # usar un remoto de sistema. Probar después la instalación de sistema.
  for remote in flathub flathub-verified_floss flathub-floss fedora; do
    if flatpak_has_remote --system "$remote"; then
      echo "Instalando $ref desde $remote (sistema)..."
      if flatpak install -y "$remote" "$ref"; then
        flatpak_ref_installed "$ref" && return 0
      fi
    fi
  done

  # Último recurso: añadir Flathub como remoto de usuario e instalar ahí.
  if ! flatpak_has_remote --user flathub; then
    echo "Añadiendo el remoto Flathub (usuario)..."
    if flatpak remote-add --if-not-exists --user flathub \
        https://flathub.org/repo/flathub.flatpakrepo; then
      echo "Instalando $ref desde flathub (usuario)..."
      if flatpak install -y --user flathub "$ref"; then
        flatpak_ref_installed "$ref" && return 0
      fi
    fi
  fi

  echo "error: no se pudo instalar $ref" >&2
  echo "  instalalo manualmente con:" >&2
  echo "    flatpak install -y flathub $RUNTIME_REF $SDK_REF $WINE_REF" >&2
  echo "  (o añadí Flathub a tu instalación de usuario y usá --user)" >&2
  return 1
}

install_runtimes() {
  say "Comprobando los runtimes de Flatpak"
  install_flatpak_ref "$RUNTIME_REF" || exit 1
  install_flatpak_ref "$SDK_REF" || exit 1
  install_flatpak_ref "$WINE_REF" || exit 1
}

# ------------------------------------------- build, export e instalación

flatpak_origin_of() {
  flatpak info --user --show-origin "$1" 2>/dev/null || true
}

# Remotos *-origin que dejó una instalación anterior hecha con
# `flatpak-builder --install`. Se borran después de reinstalar las refs desde
# gticlub-local, nunca antes: borrarlos primero dejaría a `flatpak update` con
# refs cuyo origen no existe.
collect_stale_origins() {
  STALE_ORIGINS=""
  local ref origin
  # `flatpak list --columns=ref` imprime las refs sin el prefijo de tipo
  # (io.github.nicobm.GtiClubRecomp/x86_64/master), al contrario que
  # `flatpak remote-ls`; se aceptan las dos formas.
  for ref in $(flatpak list --user --all --columns=ref 2>/dev/null); do
    case "$ref" in
      app/"$APP_ID"/*|"$APP_ID"/*|runtime/"$DEBUG_ID"/*|"$DEBUG_ID"/*) ;;
      *) continue ;;
    esac
    origin=$(flatpak_origin_of "$ref")
    case "$origin" in
      ""|-|"$LOCAL_REMOTE") continue ;;
      *-origin) ;;
      *) continue ;;
    esac
    case " $STALE_ORIGINS " in
      *" $origin "*) ;;
      *) STALE_ORIGINS="$STALE_ORIGINS $origin" ;;
    esac
  done
}

build_flatpak() {
  say "Construyendo $APP_ID (tarda unos minutos)"
  flatpak-builder --user --force-clean --disable-rofiles-fuse \
    --repo="$PWD/repo" build-dir "$MANIFEST" ||
    die "La construcción del Flatpak falló."
}

install_user_app() {
  say "Instalando $APP_ID desde el remoto local $LOCAL_REMOTE"

  if flatpak info --system "$APP_ID" >/dev/null 2>&1; then
    say "Aviso: hay una instalación del sistema anterior de $APP_ID."
    printf 'Para que "flatpak run" use la de usuario, quitá la del sistema:\n'
    printf '  flatpak uninstall --system %s\n' "$APP_ID"
    printf '  flatpak remote-delete --system --force %s\n' "$LOCAL_REMOTE"
  fi

  local repo_uri
  repo_uri="$(python3 -c 'import pathlib, sys
print(pathlib.Path(sys.argv[1]).as_uri())' "$PWD/repo")"

  # No agregar --no-enumerate: flatpak oculta las refs de un remoto
  # no-enumerate en `flatpak remote-ls` salvo que tenga un xa.main-ref (ver
  # flatpak_dir_list_remote_refs), y `flatpak remote-add` no lo pone, así que
  # el remoto aparecería vacío. En cambio se desactiva después de instalar,
  # lo que también lo mantiene fuera de las tiendas y de las actualizaciones.
  if flatpak_has_remote --user "$LOCAL_REMOTE"; then
    flatpak remote-modify --user --no-gpg-verify --no-use-for-deps --enumerate \
      --enable --url="$repo_uri" "$LOCAL_REMOTE" ||
      die "No se pudo modificar el remoto local de usuario."
  else
    flatpak remote-add --user --no-gpg-verify --no-use-for-deps \
      "$LOCAL_REMOTE" "$repo_uri" ||
      die "No se pudo añadir el remoto local de usuario."
  fi

  # Instalar la app, más el runtime de debug solo con --debug: instalar la app
  # no arrastra su extensión de debug.
  # `flatpak remote-ls` imprime refs completas y `flatpak list` sin el prefijo
  # de tipo; se aceptan las dos formas.
  local refs ref
  refs=$(flatpak remote-ls --user --all --columns=ref "$LOCAL_REMOTE" 2>/dev/null || true)

  set --
  if [ -n "$refs" ]; then
    while IFS= read -r ref; do
      case "$ref" in
        app/"$APP_ID"/*) set -- "$@" "$ref" ;;
        "$APP_ID"/*) set -- "$@" "app/$ref" ;;
        runtime/"$DEBUG_ID"/*)
          if [ "$WITH_DEBUG" = 1 ]; then set -- "$@" "$ref"; fi
          ;;
        "$DEBUG_ID"/*)
          if [ "$WITH_DEBUG" = 1 ]; then set -- "$@" "runtime/$ref"; fi
          ;;
      esac
    done <<EOF
$refs
EOF
  else
    # El remoto no se pudo listar: usar las refs que acaba de escribir la
    # exportación (repo/refs/heads/<kind>/<id>/<arch>/<branch>).
    for ref in "$PWD"/repo/refs/heads/app/"$APP_ID"/*/*; do
      [ -f "$ref" ] || continue
      set -- "$@" "${ref#"$PWD"/repo/refs/heads/}"
    done
    if [ "$WITH_DEBUG" = 1 ]; then
      for ref in "$PWD"/repo/refs/heads/runtime/"$DEBUG_ID"/*/*; do
        [ -f "$ref" ] || continue
        set -- "$@" "${ref#"$PWD"/repo/refs/heads/}"
      done
    fi
  fi
  [ "$#" -gt 0 ] || die "$LOCAL_REMOTE ($repo_uri) no ofrece $APP_ID.
  comprobalo con: flatpak remote-ls --user $LOCAL_REMOTE"

  flatpak install --user --reinstall --noninteractive "$LOCAL_REMOTE" "$@" ||
    die "No se pudo instalar el Flatpak en el usuario."

  flatpak list --user --app --columns=application 2>/dev/null | grep -qx "$APP_ID" ||
    echo "aviso: el Flatpak se construyó pero no aparece instalado en el usuario." >&2

  # El runtime de debug es opcional: quitar el que haya dejado un build
  # anterior (o una instalación manual) para que deje de aparecer como runtime
  # fijado en `flatpak uninstall --unused`. Con --debug se conserva.
  if [ "$WITH_DEBUG" = 0 ]; then
    for ref in $(flatpak list --user --all --columns=ref 2>/dev/null); do
      case "$ref" in
        runtime/"$DEBUG_ID"/*|"$DEBUG_ID"/*) ;;
        *) continue ;;
      esac
      echo "Quitando el runtime de debug $ref (usá --debug para conservarlo)..."
      flatpak uninstall --user -y --noninteractive "$ref" >/dev/null 2>&1 || true
    done
  fi

  # Los builds locales se actualizan ejecutando este script, no con
  # `flatpak update`.
  flatpak remote-modify --user --disable "$LOCAL_REMOTE" ||
    echo "aviso: no se pudo desactivar el remoto $LOCAL_REMOTE" >&2

  # Ahora los antiguos remotos *-origin ya no tienen refs instaladas. --force
  # es lo que hace efectivo el borrado: sin él flatpak se niega (y pide quitar
  # las refs) cuando un remoto todavía tiene refs instaladas.
  local remote
  for remote in $STALE_ORIGINS; do
    flatpak_has_remote --user "$remote" || continue
    if flatpak list --user --all --columns=origin 2>/dev/null | grep -qxF "$remote"; then
      echo "aviso: $remote sigue siendo el origen de una ref instalada; se conserva" >&2
      continue
    fi
    echo "Quitando el remoto Flatpak antiguo $remote..."
    flatpak remote-delete --user --force --noninteractive "$remote" >/dev/null 2>&1 ||
      echo "aviso: no se pudo quitar el remoto Flatpak $remote" >&2
  done
}

print_summary() {
  cat <<EOF

Racing Club+ está instalado como Flatpak de usuario:

  flatpak run --user $APP_ID

Los datos, partidas y ajustes se guardan en:
  ~/.var/app/$APP_ID/data/racing-club/game

Para desinstalarlo:
  flatpak uninstall --user $APP_ID
  flatpak remote-delete --user --force $LOCAL_REMOTE
  rm -rf ~/.var/app/$APP_ID
EOF
}

# ---------------------------------------------------------------- principal

[[ -f "$MANIFEST" ]] || die "No encuentro $MANIFEST."
[[ -f "$REPO_DIR/flatpak/racing-club-launch" ]] ||
  die "No encuentro $REPO_DIR/flatpak/racing-club-launch."

build_search_dirs
parse_inputs ${RAW_ARGS[@]+"${RAW_ARGS[@]}"}
prepare_prebuilt "$PATREON_SOURCE"

install_runtimes
collect_stale_origins
build_flatpak
install_user_app
print_summary
