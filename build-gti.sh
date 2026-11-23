#!/usr/bin/env bash
# build-gti.sh — empaqueta Racing Club+ (GTI Club+ recompilado para PC) en un
# Flatpak que ejecuta el .exe nativo con Wine.
#
# El paquete de Patreon ya trae el juego compilado:
#   Racing Club+/Racing Club+.exe
#   Racing Club+/juego/EBOOT.elf
#   Racing Club+/juego/USRDIR/{data,shadercache}
# No hace falta RPCS3, ni el instalador, ni mingw, ni compilar.
#
# Uso:
#   bash build-gti.sh
#   bash build-gti.sh "/ruta/Racing Club+ Instalador.zip"
#   bash build-gti.sh "/ruta/Racing Club+"
#
# Si no pasás nada, busca el zip/carpeta en XDG Downloads/Documents.
set -Eeuo pipefail

APP_ID="io.github.nicobm.GtiClubRecomp"
LOCAL_REMOTE="gticlub-local"
EXPECTED_EBOOT_SHA1="e9faf331b52770164be0c19f427c10aef747776d"

SCRIPT_PATH="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/$(basename -- "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(dirname -- "$SCRIPT_PATH")"
REPO_DIR="${GTI_REPO:-$SCRIPT_DIR}"

PREBUILT_DIR="$REPO_DIR/prebuilt"
WORK_DIR="$REPO_DIR/.flatpak"
MANIFEST="$REPO_DIR/$APP_ID.json"

TOOLBOX_CONTAINER="${GTI_TOOLBOX:-}"
TEMP_USER_REMOTE=""
USER_FLATHUB_REMOTE=""

die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }
say() { printf '\n== %s\n' "$*"; }

usage() {
  cat <<'EOF'
Uso:
  bash build-gti.sh
  bash build-gti.sh "/ruta/Racing Club+ Instalador.zip"
  bash build-gti.sh "/ruta/Racing Club+"

  # El .rar/.zip del ROM es opcional; se acepta en cualquier orden:
  bash build-gti.sh "/ruta/Racing Club+ Instalador.zip" "/ruta/GTI Club...rar"
  bash build-gti.sh "/ruta/GTI Club...rar" "/ruta/Racing Club+ Instalador.zip"

El paquete de Patreon ya contiene el juego compilado. El script lo empaqueta
en un Flatpak y lo ejecuta con Wine. No compila ni usa RPCS3.

Si no pasás nada, busca el paquete de Patreon y el ROM en XDG
Downloads/Documents. El ROM no se usa si el paquete ya trae el juego
compilado; se acepta para que puedas pasarlo igual.
EOF
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
  bash build-gti.sh \"/ruta/Racing Club+ Instalador.zip\" \"/ruta/GTI Club...rar\""

  printf 'Paquete de Patreon: %s\n' "$PATREON_SOURCE"
  if [[ -n "$ROM_SOURCE" ]]; then
    printf 'ROM del juego: %s\n' "$ROM_SOURCE"
    printf '  (no se usa: el paquete de Patreon ya trae el juego compilado y los datos)\n'
  fi
}

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

tune_prebuilt_ini() {
  local ini="$PREBUILT_DIR/gtiport.ini"
  [[ -f "$ini" ]] || return 0
  # El ini del autor viene en 4K/fullscreen/72 Hz; en otra GPU eso puede
  # quedar en negro o comer CPU. Arrancamos en ventana 720p/60 y se cambia
  # desde el menú F1 del juego.
  sed -i \
    -e 's/^pantalla_completa=.*/pantalla_completa=0/' \
    -e 's/^resolucion_interna=.*/resolucion_interna=0/' \
    -e 's/^imagenes_por_segundo=.*/imagenes_por_segundo=60/' \
    -e 's/^pantalla_120hz=.*/pantalla_120hz=0/' "$ini"
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

# ---------------------------------------------------------------- Flatpak host

disable_temp_user_remote() {
  if [[ -n "$TEMP_USER_REMOTE" ]]; then
    flatpak remote-modify --user --disable "$TEMP_USER_REMOTE" >/dev/null 2>&1 || true
    TEMP_USER_REMOTE=""
  fi
}

user_flathub_remote() {
  if flatpak remotes --user --columns=name 2>/dev/null | grep -qx flathub; then
    USER_FLATHUB_REMOTE="flathub"
    return 0
  fi
  if [[ -z "$TEMP_USER_REMOTE" ]]; then
    TEMP_USER_REMOTE="gticlub-build-flathub"
    if flatpak remotes --user --show-disabled --columns=name 2>/dev/null | grep -qx "$TEMP_USER_REMOTE"; then
      flatpak remote-modify --user --enable "$TEMP_USER_REMOTE" >/dev/null ||
        die "No se pudo habilitar $TEMP_USER_REMOTE."
    else
      flatpak remote-add --if-not-exists --user "$TEMP_USER_REMOTE" \
        https://flathub.org/repo/flathub.flatpakrepo >/dev/null ||
        die "No se pudo añadir el remoto de Flathub."
    fi
  fi
  USER_FLATHUB_REMOTE="$TEMP_USER_REMOTE"
}

ensure_user_flatpak_deps() {
  say "Comprobando runtimes de usuario de Flatpak"
  local -a missing=()
  local ref
  for ref in \
    "org.freedesktop.Platform//26.08" \
    "org.freedesktop.Sdk//26.08" \
    "org.winehq.Wine//stable-26.08"; do
    flatpak info --user "$ref" >/dev/null 2>&1 || missing+=("$ref")
  done

  if ((${#missing[@]} == 0)); then
    printf 'Los runtimes de usuario ya están instalados.\n'
    return 0
  fi

  printf 'Faltan estos runtimes de usuario de Flathub:\n'
  printf '  - %s\n' "${missing[@]}"
  printf 'No hace falta sudo: se instalan en tu usuario.\n'
  if ! ask_yes_no "¿Instalar los runtimes de Flathub en el usuario?"; then
    die "Sin esos runtimes no se puede construir el Flatpak."
  fi

  trap disable_temp_user_remote EXIT
  user_flathub_remote
  flatpak install -y --user "$USER_FLATHUB_REMOTE" "${missing[@]}" ||
    die "No se pudieron instalar los runtimes de usuario."
}

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

install_user_app() {
  say "Instalando el juego en el usuario desde el remoto local $LOCAL_REMOTE"
  [[ -d "$WORK_DIR/repo" ]] ||
    die "No encuentro el repositorio Flatpak construido en $WORK_DIR/repo."

  if flatpak info --system "$APP_ID" >/dev/null 2>&1; then
    say "Aviso: hay una instalación del sistema anterior de $APP_ID."
    printf 'Para que "flatpak run" use la de usuario, quitá la del sistema:\n'
    printf '  flatpak uninstall --system %s\n' "$APP_ID"
    printf '  flatpak remote-delete --system --force %s\n' "$LOCAL_REMOTE"
  fi

  local repo_uri
  repo_uri="$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve().as_uri())' "$WORK_DIR/repo")"

  if flatpak remotes --user --show-disabled --columns=name 2>/dev/null | grep -qx "$LOCAL_REMOTE"; then
    flatpak remote-modify --user --no-gpg-verify --no-use-for-deps --enumerate \
      --enable --url="$repo_uri" "$LOCAL_REMOTE" ||
      die "No se pudo modificar el remoto local de usuario."
  else
    flatpak remote-add --user --no-gpg-verify --no-use-for-deps \
      "$LOCAL_REMOTE" "$repo_uri" ||
      die "No se pudo añadir el remoto local de usuario."
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
    die "No se pudo instalar el Flatpak en el usuario."

  flatpak remote-modify --user --disable "$LOCAL_REMOTE" ||
    printf 'Aviso: no se pudo desactivar el remoto %s.\n' "$LOCAL_REMOTE" >&2

  flatpak list --user --app --columns=application 2>/dev/null | grep -qx "$APP_ID" ||
    die "El Flatpak se construyó pero no aparece instalado."
}

print_summary() {
  cat <<EOF

Racing Club+ está instalado como Flatpak de usuario:

  flatpak run $APP_ID

Los datos, partidas y ajustes se guardan en:
  ~/.var/app/$APP_ID/data/racing-club/game

Para desinstalarlo:
  flatpak uninstall --user $APP_ID
  flatpak remote-delete --user --force $LOCAL_REMOTE
  rm -rf ~/.var/app/$APP_ID
EOF
}

# ----------------------------------------------------------------------- host

host_stage() {
  [[ -f "$REPO_DIR/CMakeLists.txt" ]] ||
    die "No encuentro el repositorio en $REPO_DIR."
  command -v flatpak >/dev/null 2>&1 ||
    die "No encuentro 'flatpak' en el host."
  command -v toolbox >/dev/null 2>&1 ||
    die "No encuentro 'toolbox'."
  command -v python3 >/dev/null 2>&1 ||
    die "No encuentro python3."

  build_search_dirs
  parse_inputs "$@"
  prepare_prebuilt "$PATREON_SOURCE"

  ensure_user_flatpak_deps

  local container="$TOOLBOX_CONTAINER"
  [[ -n "$container" ]] || container="$(pick_toolbox)"
  if [[ -z "$container" ]]; then
    local release
    release="$(rpm -E %fedora 2>/dev/null || true)"
    [[ -n "$release" ]] || release="43"
    container="fedora-toolbox-$release"
    say "No hay contenedores Toolbox; creando $container"
    if ! toolbox create --distro fedora --release "$release" "$container"; then
      toolbox run -c "$container" true >/dev/null 2>&1 ||
        die "No se pudo crear ni usar el contenedor Toolbox '$container'."
    fi
  fi

  say "Construyendo el Flatpak en Toolbox ($container)"
  toolbox run -c "$container" -- env \
    GTI_IN_TOOLBOX=1 \
    GTI_REPO="$REPO_DIR" \
    bash "$SCRIPT_PATH" ||
    die "La construcción del Flatpak falló."

  install_user_app
  print_summary
}

# -------------------------------------------------------------------- toolbox

toolbox_stage() {
  [[ "${GTI_IN_TOOLBOX:-0}" == "1" ]] ||
    die "Este bloque debe ejecutarse dentro de Toolbox."
  [[ -f "$MANIFEST" ]] || die "No encuentro $MANIFEST."
  [[ -d "$PREBUILT_DIR" ]] || die "No encuentro $PREBUILT_DIR."
  cd "$REPO_DIR"

  if ! command -v flatpak-builder >/dev/null 2>&1; then
    printf 'Falta flatpak-builder en Toolbox.\n'
    if ! ask_yes_no "¿Instalarlo con sudo dnf?"; then
      die "Instalá flatpak-builder y volvé a ejecutar el script."
    fi
    sudo dnf install -y flatpak-builder ||
      die "dnf no pudo instalar flatpak-builder."
  fi

  local ref
  for ref in \
    "org.freedesktop.Platform//26.08" \
    "org.freedesktop.Sdk//26.08" \
    "org.winehq.Wine//stable-26.08"; do
    flatpak info --user "$ref" >/dev/null 2>&1 ||
      die "Falta $ref en la instalación de usuario de Flatpak.
Vuelve a ejecutar el script desde el host."
  done

  mkdir -p "$WORK_DIR"
  rm -rf "$REPO_DIR/.flatpak-builder"

  flatpak-builder --user --force-clean --disable-rofiles-fuse \
    --state-dir="$WORK_DIR/state" \
    --repo="$WORK_DIR/repo" "$WORK_DIR/build-dir" "$MANIFEST"
}

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  -*) die "Opción no reconocida: $1" ;;
esac

if [[ "${GTI_IN_TOOLBOX:-0}" == "1" ]]; then
  toolbox_stage
else
  host_stage "$@"
fi
