#!/usr/bin/env bash
# build-gti.sh — compila el port nativo de GTI Club+ (gtiport.exe) con mingw-w64
# y lo empaqueta/instala como Flatpak usando Wine (org.winehq.Wine) de Flathub.
#
# Uso:
#   bash build-gti.sh
#   bash build-gti.sh "GTI Club Rally Cote D'Azur PSN [NPEB-00060].rar"
#   bash build-gti.sh "GTI Club Rally Cote D'Azur PSN [NPEB-00060].zip"
#
# El primer argumento puede ser el .rar/.zip del juego o una carpeta ya
# extraída (NPEB00060). Si no se pasa nada, se busca en ~/Documentos.
#
# Variables opcionales:
#   GTI_ROM=/ruta/al/juego.rar   archivo del juego (si no se autodetecta)
#   GTI_DATA=/ruta/NPEB00060     datos ya extraídos (evita extraer el .rar)
#   GTI_TOOLBOX=fedora-toolbox-43  contenedor de Toolbox a usar
#
# El script se re-ejecuta dentro de Toolbox. Dentro pregunta antes de instalar
# paquetes con sudo dnf y antes de instalar los runtimes de Flathub.
set -Eeuo pipefail

APP_ID="io.github.nicobm.GtiClubRecomp"
LOCAL_REMOTE="gticlub-local"

SCRIPT_PATH="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/$(basename -- "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname -- "$SCRIPT_PATH")"
REPO_DIR="${GTI_REPO:-$SCRIPT_DIR}"

BUILD_DIR="$REPO_DIR/build-gti"
GAME_DATA_DIR="$REPO_DIR/game-data"
WORK_DIR="$REPO_DIR/.flatpak"
MANIFEST="$REPO_DIR/$APP_ID.json"

DNF_PACKAGES=(
  cmake
  ninja-build
  gcc
  gcc-c++
  mingw64-filesystem
  mingw64-gcc
  mingw64-gcc-c++
  mingw64-SDL3
  mingw64-SDL3-static
  mingw64-zlib
  mingw64-zlib-static
  flatpak
  flatpak-builder
  bsdtar
  unar
  python3
  openssl
)

DLLS_TO_COPY=()

die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }
say() { printf '\n== %s\n' "$*"; }

usage() {
  cat <<'EOF'
Uso:
  bash build-gti.sh
  bash build-gti.sh "GTI Club Rally Cote D'Azur PSN [NPEB-00060].rar"
  bash build-gti.sh "GTI Club Rally Cote D'Azur PSN [NPEB-00060].zip"
  bash build-gti.sh /ruta/a/NPEB00060

Si no pasas nada, el script busca el .rar/.zip en XDG Documents/Downloads
(por ejemplo ~/Documentos o ~/Descargas).
El nombre del juego lleva espacios y apóstrofo: ponlo siempre entre comillas.
EOF
}

# Carpetas donde buscar el archivo del juego: XDG Documents/Downloads (que en
# español suelen ser ~/Documentos y ~/Descargas) y los nombres habituales.
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
          val="${val%\"}"
          val="${val#\"}"
          val="${val/#\$HOME/$HOME}"
          val="${val/#\$\{HOME\}/$HOME}"
          if [[ -d "$val" && "$val" != "$HOME" ]]; then
            dirs+=("$val")
          fi
          ;;
      esac
    done < "$conf"
  fi

  for d in "$HOME/Documentos" "$HOME/Documents" "$HOME/Descargas" "$HOME/Downloads"; do
    [[ -d "$d" ]] && dirs+=("$d")
  done

  local seen=""
  for d in "${dirs[@]}"; do
    case ":$seen:" in
      *":$d:"*) continue ;;
    esac
    seen="$seen:$d"
    XDG_SEARCH_DIRS+=("$d")
  done
}

# Acepta una ruta absoluta/relativa, o un nombre que esté en XDG Documents /
# XDG Downloads. Sirve tanto para el .rar/.zip como para una carpeta de datos.
resolve_candidate() {
  local p="$1" dir base
  [[ -n "$p" ]] || return 1
  if [[ -e "$p" ]]; then
    readlink -f -- "$p"
    return 0
  fi
  for dir in "$PWD" ${XDG_SEARCH_DIRS[@]+"${XDG_SEARCH_DIRS[@]}"}; do
    if [[ -e "$dir/$p" ]]; then
      readlink -f -- "$dir/$p"
      return 0
    fi
  done
  base="$(basename -- "$p")"
  if [[ "$base" != "$p" ]]; then
    for dir in "$PWD" ${XDG_SEARCH_DIRS[@]+"${XDG_SEARCH_DIRS[@]}"}; do
      if [[ -e "$dir/$base" ]]; then
        readlink -f -- "$dir/$base"
        return 0
      fi
    done
  fi
  return 1
}

parse_user_path() {
  local candidate resolved
  if (($# == 1)); then
    candidate="$1"
  else
    # Reconstruye el nombre si se olvidaron las comillas:
    #   bash build-gti.sh GTI Club ... .rar
    candidate="$*"
    if ! resolve_candidate "$candidate" >/dev/null 2>&1; then
      candidate="$1"
    fi
  fi

  resolved="$(resolve_candidate "$candidate" || true)"
  if [[ -z "$resolved" ]]; then
    die "No encuentro el archivo o carpeta: $candidate
El nombre del juego lleva espacios y apóstrofo; ponlo entre comillas dobles:
  bash build-gti.sh \"GTI Club Rally Cote D'Azur PSN [NPEB-00060].rar\""
  fi

  if [[ -d "$resolved" ]]; then
    GTI_DATA="$resolved"
  else
    GTI_ROM="$resolved"
  fi
}

ask_yes_no() {
  local prompt="$1" answer
  printf '%s [s/N] ' "$prompt"
  if ! read -r answer; then
    return 1
  fi
  case "$answer" in
    s|S|y|Y|si|sí|SI|SÍ) return 0 ;;
    *) return 1 ;;
  esac
}

# ---------------------------------------------------------------- host stage

pick_toolbox() {
  local -a names=()
  if command -v podman >/dev/null 2>&1; then
    mapfile -t names < <(
      podman ps -a --filter label=com.github.containers.toolbox=true \
        --format '{{.Names}}' 2>/dev/null || true
    )
  fi
  ((${#names[@]} > 0)) || return 0
  local n
  for n in "${names[@]}"; do
    case "$n" in
      fedora-toolbox-*) printf '%s\n' "$n"; return 0 ;;
    esac
  done
  printf '%s\n' "${names[0]}"
}

host_stage() {
  [[ -f "$REPO_DIR/CMakeLists.txt" ]] ||
    die "No encuentro $REPO_DIR/CMakeLists.txt. Ejecuta el script desde el repositorio."

  command -v toolbox >/dev/null 2>&1 ||
    die "No encuentro 'toolbox'. Este script está pensado para Fedora Silverblue/Bazzite con Toolbox."
  command -v flatpak >/dev/null 2>&1 ||
    die "No encuentro 'flatpak' en el host. Sin Flatpak no se puede instalar el paquete final."

  local container="${GTI_TOOLBOX:-}"
  if [[ -z "$container" ]]; then
    container="$(pick_toolbox)"
  fi
  if [[ -z "$container" ]]; then
    local release
    release="$(rpm -E %fedora 2>/dev/null || true)"
    [[ -n "$release" ]] || release="43"
    container="fedora-toolbox-$release"
    say "No hay contenedores Toolbox; creando $container"
    if ! toolbox create --distro fedora --release "$release" "$container"; then
      # Puede que existiera pero no lo detectamos (por ejemplo, sin la etiqueta
      # de podman): comprobamos si al menos se puede entrar.
      toolbox run -c "$container" true >/dev/null 2>&1 ||
        die "No se pudo crear ni usar el contenedor Toolbox '$container'."
    fi
  fi

  say "Entrando en Toolbox ($container)"
  exec toolbox run -c "$container" -- env \
    GTI_IN_TOOLBOX=1 \
    GTI_REPO="$REPO_DIR" \
    GTI_ROM="${GTI_ROM:-}" \
    GTI_DATA="${GTI_DATA:-}" \
    bash "$SCRIPT_PATH"
}

# ------------------------------------------------------------- toolbox stage

require_commands() {
  local cmd
  for cmd in cmake ninja x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ \
             flatpak flatpak-builder python3 openssl; do
    command -v "$cmd" >/dev/null 2>&1 ||
      die "Falta el comando '$cmd'. Instala las dependencias con dnf y vuelve a intentarlo."
  done
}

check_dnf_dependencies() {
  say "Comprobando dependencias de compilación"
  local -a missing=()
  local pkg
  for pkg in "${DNF_PACKAGES[@]}"; do
    if ! rpm -q "$pkg" >/dev/null 2>&1; then
      missing+=("$pkg")
    fi
  done

  if ((${#missing[@]} == 0)); then
    printf 'Todas las dependencias están instaladas.\n'
    return 0
  fi

  printf 'Faltan estos paquetes en el Toolbox:\n'
  printf '  - %s\n' "${missing[@]}"
  if ! ask_yes_no "¿Instalarlos ahora con sudo dnf?"; then
    die "Instala las dependencias y vuelve a ejecutar 'bash build-gti.sh'."
  fi
  sudo dnf install -y "${missing[@]}" ||
    die "dnf no pudo instalar las dependencias."
}

build_windows_exe() {
  say "Compilando gtiport.exe con mingw-w64"
  local toolchain="/usr/share/mingw/toolchain-mingw64.cmake"
  [[ -f "$toolchain" ]] ||
    die "No encuentro $toolchain (paquete mingw64-filesystem)."

  # Si alguien configuró antes este directorio para Linux, la caché estorba.
  if [[ -f "$BUILD_DIR/CMakeCache.txt" ]] &&
     ! grep -q '^CMAKE_SYSTEM_NAME:.*=Windows' "$BUILD_DIR/CMakeCache.txt"; then
    rm -rf "$BUILD_DIR"
  fi

  local -a cmake_args=(
    -S "$REPO_DIR"
    -B "$BUILD_DIR"
    -G Ninja
    -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
    -DZLIB_USE_STATIC_LIBS=ON
  )

  # Fedora instala la configuración de SDL3 dentro del sysroot mingw. Se pasa
  # explícitamente para que no se confunda con una SDL3 nativa del host.
  local sdl3_dir="/usr/x86_64-w64-mingw32/sys-root/mingw/lib/cmake/SDL3"
  [[ -d "$sdl3_dir" ]] && cmake_args+=("-DSDL3_DIR=$sdl3_dir")

  cmake "${cmake_args[@]}"
  cmake --build "$BUILD_DIR" --target gtiport --parallel "$(nproc)"

  local exe="$BUILD_DIR/gtiport.exe"
  if [[ ! -f "$exe" ]]; then
    exe="$(find "$BUILD_DIR" -type f -iname 'gtiport.exe' -print -quit)"
  fi
  [[ -n "$exe" && -f "$exe" ]] ||
    die "La compilación terminó pero no encuentro gtiport.exe."

  if [[ "$exe" != "$BUILD_DIR/gtiport.exe" ]]; then
    cp -f "$exe" "$BUILD_DIR/gtiport.exe"
  fi

  if command -v file >/dev/null 2>&1; then
    file "$BUILD_DIR/gtiport.exe" | grep -q 'PE32+ executable' ||
      die "$BUILD_DIR/gtiport.exe no es un ejecutable Windows x86-64."
  fi
  printf 'Compilado: %s\n' "$BUILD_DIR/gtiport.exe"

  # El proyecto enlaza SDL3 y zlib en estático, pero si alguna vez cambiara,
  # copiamos las DLLs que falten junto al ejecutable para que Wine las vea.
  if command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
    local -a imports=()
    mapfile -t imports < <(
      x86_64-w64-mingw32-objdump -p "$BUILD_DIR/gtiport.exe" |
        awk '/DLL Name:/ {print $3}' | sort -u
    )
    printf 'DLLs importadas: %s\n' "${imports[*]:-ninguna}"
    local dll
    for dll in SDL3.dll zlib1.dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
      local found=0 i
      for i in "${imports[@]}"; do
        [[ "$i" == "$dll" ]] && found=1
      done
      ((found == 0)) || DLLS_TO_COPY+=("$dll")
    done
  fi
}

find_rom() {
  if [[ -n "${GTI_ROM:-}" ]]; then
    [[ -f "$GTI_ROM" ]] || die "GTI_ROM no existe: $GTI_ROM"
    printf '%s\n' "$GTI_ROM"
    return 0
  fi

  local dir found
  # Primero el nombre exacto de la versión soportada.
  for dir in ${XDG_SEARCH_DIRS[@]+"${XDG_SEARCH_DIRS[@]}"}; do
    [[ -d "$dir" ]] || continue
    found="$(find "$dir" -maxdepth 2 -type f \
      \( -iname '*NPEB*00060*.rar' -o -iname '*NPEB*00060*.zip' \) \
      -print -quit 2>/dev/null || true)"
    if [[ -n "$found" ]]; then
      printf '%s\n' "$found"
      return 0
    fi
  done
  # Si el archivo tiene otro nombre, prueba con GTI/Club.
  for dir in ${XDG_SEARCH_DIRS[@]+"${XDG_SEARCH_DIRS[@]}"}; do
    [[ -d "$dir" ]] || continue
    found="$(find "$dir" -maxdepth 2 -type f \
      \( -iname '*gti*' -o -iname '*club*' \) \
      \( -iname '*.rar' -o -iname '*.zip' \) \
      -print -quit 2>/dev/null || true)"
    if [[ -n "$found" ]]; then
      printf '%s\n' "$found"
      return 0
    fi
  done
  return 0
}

extract_archive() {
  local src="$1" dest="$2"
  mkdir -p "$dest"

  if command -v unar >/dev/null 2>&1; then
    if unar -q -o "$dest" "$src" >/dev/null 2>&1; then
      return 0
    fi
    printf 'unar no pudo extraer %s; probando 7z...\n' "$(basename "$src")" >&2
  fi
  if command -v 7z >/dev/null 2>&1; then
    if 7z x -y -o"$dest" "$src" >/dev/null 2>&1; then
      return 0
    fi
  fi
  if command -v 7zz >/dev/null 2>&1; then
    if 7zz x -y -o"$dest" "$src" >/dev/null 2>&1; then
      return 0
    fi
  fi
  if command -v 7za >/dev/null 2>&1; then
    if 7za x -y -o"$dest" "$src" >/dev/null 2>&1; then
      return 0
    fi
  fi
  if command -v bsdtar >/dev/null 2>&1; then
    if bsdtar -xf "$src" -C "$dest"; then
      return 0
    fi
  fi
  return 1
}

extract_pkg_files() {
  local pkg="$1" dest="$2"
  python3 - "$pkg" "$dest" <<'PY'
import hashlib
import os
import struct
import subprocess
import sys

PKG_KEY = bytes.fromhex("2e7b71d7c9c9a14ea3221f188828b8f8")
HDR_FMT = ">4sHHIIIIQQQ48s16s16s"
ITEM_FMT = ">IIQQII"
ITEM_SIZE = 0x20
PREFIX = "usrdir/data"
FLAG_DIR = 0x04


def debug_keystream(key16, first_block, nbytes):
    """Keystream de los PKG 'debug' (revision 0): SHA-1 por bloque."""
    n = (nbytes + 15) // 16
    buf = bytearray(64)
    buf[0:8] = buf[8:16] = key16[0:8]
    buf[16:24] = buf[24:32] = key16[8:16]
    out = bytearray(n * 16)
    sha1 = hashlib.sha1
    pack_into = struct.pack_into
    for i in range(n):
        pack_into(">Q", buf, 56, first_block + i)
        out[i * 16:(i + 1) * 16] = sha1(buf).digest()[:16]
    return bytes(out[:nbytes])


def fail(msg):
    print("ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def main():
    pkg, dest_root = sys.argv[1], sys.argv[2]
    with open(pkg, "rb") as f:
        raw = f.read(struct.calcsize(HDR_FMT))
        if len(raw) != struct.calcsize(HDR_FMT):
            fail("PKG truncado: " + pkg)
        (magic, revision, pkg_type, _meta_off, _meta_cnt, _hdr_size,
         item_count, _total_size, data_off, data_size,
         _content_id, qa_digest, iv) = struct.unpack(HDR_FMT, raw)
        if magic != b"\x7fPKG":
            fail("No es un PKG de PS3: " + pkg)
        if pkg_type != 1:
            fail("Solo se admiten PKG de PS3 (tipo %#x): %s" % (pkg_type, pkg))
        f.seek(data_off)
        data = f.read(data_size)

    if len(data) != data_size:
        fail("PKG truncado (%d de %d bytes): %s" % (len(data), data_size, pkg))

    # revision == 0 -> PKG debug (keystream SHA-1); resto -> PKG retail (AES-CTR).
    if revision == 0:
        plain = bytes(a ^ b for a, b in zip(data, debug_keystream(qa_digest, 0, data_size)))
    else:
        proc = subprocess.run(
            ["openssl", "enc", "-d", "-aes-128-ctr",
             "-K", PKG_KEY.hex(), "-iv", iv.hex()],
            input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        if proc.returncode != 0:
            fail("openssl no pudo descifrar %s: %s" %
                 (pkg, proc.stderr.decode("utf-8", "replace").strip()))
        plain = proc.stdout
        if len(plain) != data_size:
            fail("Descifrado incompleto en %s" % pkg)

    table = plain[:item_count * ITEM_SIZE]
    files = 0
    written = 0

    for i in range(item_count):
        n_off, n_size, d_off, d_size, flags, _ = struct.unpack_from(
            ITEM_FMT, table, i * ITEM_SIZE)
        if n_off + n_size > data_size:
            continue
        name = plain[n_off:n_off + n_size].rstrip(b"\0").decode("utf-8", "replace")
        name = name.replace("\\", "/").lstrip("/")
        parts = [p for p in name.split("/") if p not in ("", ".")]
        if not parts or any(p == ".." for p in parts):
            continue
        rel = "/".join(parts)
        if rel.lower() != PREFIX and not rel.lower().startswith(PREFIX + "/"):
            continue  # solo USRDIR/data: sin EBOOT, sin shadercache

        out = os.path.join(dest_root, "NPEB00060", *parts)
        if flags & FLAG_DIR:
            os.makedirs(out, exist_ok=True)
            continue
        if d_off + d_size > data_size:
            continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as w:
            w.write(plain[d_off:d_off + d_size])
        files += 1
        written += d_size

    print("  %s: %d archivos de datos (%d MB)" %
          (os.path.basename(pkg), files, written // (1024 * 1024)))


main()
PY
}

extract_rom() {
  local rom="$1"
  local tmp="$WORK_DIR/rom-tmp"
  rm -rf "$tmp"
  mkdir -p "$tmp/outer"

  say "Extrayendo el archivo del juego: $(basename "$rom")"
  extract_archive "$rom" "$tmp/outer" ||
    printf 'Aviso: la extracción del archivo exterior devolvió un error; sigo buscando PKGs.\n' >&2

  local -a pkgs=()
  mapfile -t pkgs < <(find "$tmp/outer" -type f -iname '*.pkg' -print 2>/dev/null || true)

  if ((${#pkgs[@]} == 0)); then
    # El RAR del juego suele contener a su vez un RAR multivolumen (.rar + .r00...).
    local inner
    inner="$(find "$tmp/outer" -type f -iname '*.rar' -printf '%s %p\n' 2>/dev/null |
      sort -nr | sed -n '1s/^[0-9][0-9]* //p' || true)"
    if [[ -n "$inner" ]]; then
      say "Extrayendo el archivo interior: $(basename "$inner")"
      mkdir -p "$tmp/inner"
      extract_archive "$inner" "$tmp/inner" ||
        printf 'Aviso: la extracción del archivo interior devolvió un error; sigo buscando PKGs.\n' >&2
      mapfile -t pkgs < <(find "$tmp/inner" -type f -iname '*.pkg' -print 2>/dev/null || true)
    fi
  fi

  if ((${#pkgs[@]} == 0)); then
    # Por si el archivo ya contiene el juego extraído en vez de PKGs.
    local extracted
    extracted="$(find "$tmp" -type d -path '*/NPEB00060/USRDIR/data' -print -quit 2>/dev/null || true)"
    if [[ -n "$extracted" ]]; then
      say "Encontré los datos ya extraídos dentro del archivo"
      mkdir -p "$GAME_DATA_DIR/NPEB00060/USRDIR"
      cp -a "$extracted" "$GAME_DATA_DIR/NPEB00060/USRDIR/"
    else
      die "No encontré PKGs ni NPEB00060/USRDIR/data en $rom.
Puedes pasar los datos ya extraídos con: GTI_DATA=/ruta/a/NPEB00060 bash build-gti.sh"
    fi
  else
    # Primero el PKG más grande (el juego base) y después DLC/actualizaciones.
    local -a ordered=()
    mapfile -t ordered < <(
      for p in "${pkgs[@]}"; do
        printf '%s\t%s\n' "$(stat -c %s "$p")" "$p"
      done | sort -nr | cut -f2-
    )
    local p
    for p in "${ordered[@]}"; do
      say "Extrayendo $(basename "$p")"
      extract_pkg_files "$p" "$GAME_DATA_DIR"
    done
  fi

  rm -rf "$tmp"
}

copy_dlls_if_needed() {
  ((${#DLLS_TO_COPY[@]} > 0)) || return 0
  mkdir -p "$GAME_DATA_DIR"
  local dll src
  for dll in "${DLLS_TO_COPY[@]}"; do
    src="$(find /usr/x86_64-w64-mingw32/sys-root/mingw/bin \
      /usr/lib/gcc/x86_64-w64-mingw32 -name "$dll" -type f -print -quit 2>/dev/null || true)"
    if [[ -n "$src" ]]; then
      cp -f "$src" "$GAME_DATA_DIR/"
      printf 'Copiada %s junto al ejecutable.\n' "$dll"
    else
      printf 'Aviso: no encuentro %s para copiarla.\n' "$dll" >&2
    fi
  done
}

prepare_game_data() {
  say "Preparando los datos del juego"
  if [[ -f "$GAME_DATA_DIR/.ok" &&
        -d "$GAME_DATA_DIR/NPEB00060/USRDIR/data/cars" ]]; then
    printf 'Ya hay datos extraídos en %s; no se vuelven a extraer.\n' "$GAME_DATA_DIR"
    copy_dlls_if_needed
    return 0
  fi

  rm -rf "$GAME_DATA_DIR"
  mkdir -p "$GAME_DATA_DIR/NPEB00060/USRDIR"

  if [[ -n "${GTI_DATA:-}" ]]; then
    [[ -e "$GTI_DATA" ]] || die "GTI_DATA no existe: $GTI_DATA"
    local src="$GTI_DATA"
    if [[ -d "$src/NPEB00060/USRDIR/data" ]]; then
      cp -a "$src/NPEB00060/USRDIR/data" "$GAME_DATA_DIR/NPEB00060/USRDIR/"
    elif [[ -d "$src/USRDIR/data" ]]; then
      cp -a "$src/USRDIR/data" "$GAME_DATA_DIR/NPEB00060/USRDIR/"
    elif [[ -d "$src/data/cars" ]]; then
      cp -a "$src/data" "$GAME_DATA_DIR/NPEB00060/USRDIR/"
    elif [[ -d "$src/cars" ]]; then
      cp -a "$src" "$GAME_DATA_DIR/NPEB00060/USRDIR/data"
    elif [[ -f "$src" ]]; then
      case "$src" in
        *.pkg|*.PKG) extract_pkg_files "$src" "$GAME_DATA_DIR" ;;
        *) extract_rom "$src" ;;
      esac
    else
      die "GTI_DATA=$GTI_DATA no contiene NPEB00060/USRDIR/data (o una carpeta data/)."
    fi
  else
    local rom
    rom="$(find_rom)"
    [[ -n "$rom" ]] || die "No encuentro el archivo del juego (NPEB00060).
Pásalo con:
  bash build-gti.sh \"GTI Club Rally Cote D'Azur PSN [NPEB-00060].rar\"
O pásale los datos ya extraídos con:
  bash build-gti.sh /ruta/a/NPEB00060"
    extract_rom "$rom"
  fi

  [[ -d "$GAME_DATA_DIR/NPEB00060/USRDIR/data/cars" ]] ||
    die "Los datos extraídos no contienen NPEB00060/USRDIR/data/cars."

  copy_dlls_if_needed
  touch "$GAME_DATA_DIR/.ok"
  printf 'Datos listos en %s\n' "$GAME_DATA_DIR"
}

flatpak_ref_installed_user() {
  flatpak info --user "$1" >/dev/null 2>&1
}

install_flatpak_dependencies() {
  say "Comprobando runtimes de Flatpak"
  local -a missing=()
  local ref
  for ref in \
    "org.freedesktop.Platform//26.08" \
    "org.freedesktop.Sdk//26.08" \
    "org.winehq.Wine//stable-26.08"; do
    if ! flatpak_ref_installed_user "$ref"; then
      missing+=("$ref")
    fi
  done

  if ((${#missing[@]} == 0)); then
    printf 'Los runtimes necesarios ya están instalados.\n'
    return 0
  fi

  printf 'Faltan estos runtimes de Flathub:\n'
  printf '  - %s\n' "${missing[@]}"
  printf 'Wine se usa como aplicación base (base app) para ejecutar el .exe.\n'
  if ! ask_yes_no "¿Instalar el runtime/deps de Flathub?"; then
    die "Sin esos runtimes no se puede construir ni ejecutar el Flatpak.
Instálalos manualmente con:
  flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathub.flatpakrepo
  flatpak install --user flathub ${missing[*]}"
  fi

  if ! flatpak remotes --user --columns=name 2>/dev/null | grep -qx flathub; then
    say "Añadiendo el remoto Flathub (usuario)"
    flatpak remote-add --if-not-exists --user flathub \
      https://flathub.org/repo/flathub.flatpakrepo
  fi

  flatpak install -y --user flathub "${missing[@]}" ||
    die "Flatpak no pudo instalar los runtimes necesarios."
}

build_and_install_flatpak() {
  say "Construyendo el Flatpak (puede tardar varios minutos)"
  [[ -f "$MANIFEST" ]] || die "No encuentro el manifiesto $MANIFEST."
  mkdir -p "$WORK_DIR"

  (
    cd "$REPO_DIR"
    flatpak-builder --user --force-clean --disable-rofiles-fuse \
      --repo="$WORK_DIR/repo" "$WORK_DIR/build-dir" "$MANIFEST"
  ) || die "flatpak-builder falló. Revisa el mensaje anterior."

  local repo_uri
  repo_uri="$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve().as_uri())' "$WORK_DIR/repo")"

  if flatpak remotes --user --show-disabled --columns=name 2>/dev/null | grep -qx "$LOCAL_REMOTE"; then
    flatpak remote-modify --user --no-gpg-verify --no-use-for-deps --enumerate \
      --enable --url="$repo_uri" "$LOCAL_REMOTE"
  else
    flatpak remote-add --user --no-gpg-verify --no-use-for-deps \
      "$LOCAL_REMOTE" "$repo_uri"
  fi

  local -a refs=()
  local ref
  while IFS= read -r ref; do
    case "$ref" in
      app/"$APP_ID"/*|"$APP_ID"/*) refs+=("$ref") ;;
    esac
  done < <(flatpak remote-ls --user --all --columns=ref "$LOCAL_REMOTE" 2>/dev/null || true)

  if ((${#refs[@]} == 0)); then
    refs=("app/$APP_ID/x86_64/master")
  fi

  flatpak install --user --reinstall --noninteractive "$LOCAL_REMOTE" "${refs[@]}" ||
    die "No se pudo instalar el Flatpak desde el repositorio local."

  # Es una compilación local: flatpak update no tiene nada que buscar.
  flatpak remote-modify --user --disable "$LOCAL_REMOTE" ||
    printf 'Aviso: no se pudo desactivar el remoto %s.\n' "$LOCAL_REMOTE" >&2

  flatpak list --user --app --columns=application 2>/dev/null | grep -qx "$APP_ID" ||
    die "El Flatpak se construyó pero no aparece instalado."
}

toolbox_stage() {
  [[ "${GTI_IN_TOOLBOX:-0}" == "1" ]] ||
    die "Este bloque debe ejecutarse dentro de Toolbox."
  [[ -f "$REPO_DIR/CMakeLists.txt" ]] ||
    die "No encuentro el repositorio en $REPO_DIR."
  cd "$REPO_DIR"

  check_dnf_dependencies
  require_commands
  build_windows_exe
  prepare_game_data
  install_flatpak_dependencies
  build_and_install_flatpak

  say "Listo"
  cat <<EOF
Racing Club+ está instalado como Flatpak de usuario:

  flatpak run $APP_ID

También aparece en el menú de aplicaciones como "Racing Club+".
Los datos y las partidas se guardan en:
  ~/.var/app/$APP_ID/data/racing-club/game

Para desinstalarlo:
  flatpak uninstall $APP_ID
  flatpak remote-delete --user $LOCAL_REMOTE
EOF
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  -*) die "Opción no reconocida: $1" ;;
esac

build_search_dirs

if (($# > 0)); then
  parse_user_path "$@"
fi

if [[ "${GTI_IN_TOOLBOX:-0}" == "1" ]]; then
  toolbox_stage
else
  host_stage
fi
