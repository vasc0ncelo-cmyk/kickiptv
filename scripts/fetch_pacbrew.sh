#!/usr/bin/env bash
# Baixa libs PacBrew (mesmas do Switchfin/GMCA) para linkar libmpv no Kick.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${PACBREW_ROOT:-$ROOT/third_party/pacbrew}"
BASE_URL="${PACBREW_URL:-https://github.com/dragonflylee/switchfin/releases/download/pacbrew}"
PKGS=(
  musl-1.5-3
  mbedtls-2.28.10-2
  sdl2-2.0.18-19
  libcurl-8.4.0-1
  dav1d-1.5.4-1
  ffmpeg-7.1.5-4
  libmpv-0.36.0-3
)
mkdir -p "$DEST"
cd "$DEST"
for p in "${PKGS[@]}"; do
  f="ps4-openorbis-${p}-any.pkg.tar.gz"
  if [ ! -f "$f" ]; then
    echo "baixando $f"
    curl -fsSL -o "$f" "$BASE_URL/$f"
  fi
  echo "extraindo $f"
  tar -xzf "$f"
done
# Dependencias extras (libass, freetype, fribidi, harfbuzz, libpng, zlib, bzip2, libsamplerate...): o libmpv.a e estatico e precisa delas.
# Em vez de adivinhar nomes/versoes, le a lista de pacotes da release e baixa os que ainda nao vieram (a versao mais nova de cada).
if command -v gh >/dev/null 2>&1; then
  have=" ${PKGS[*]} "
  gh api repos/dragonflylee/switchfin/releases/tags/pacbrew --paginate --jq '.assets[].name' 2>/dev/null \
    | grep -E '^ps4-openorbis-.*-any\.pkg\.tar\.gz$' | sort -V > extra_list.txt || true
  echo "pacotes ps4-openorbis na release: $(wc -l < extra_list.txt)"
  cp extra_list.txt "$ROOT/pacbrew-assets.txt" 2>/dev/null || true
  declare -A newest
  while read -r f; do
    base=$(echo "$f" | sed -E 's/^ps4-openorbis-(.*)-[0-9][^-]*-[0-9]+-any\.pkg\.tar\.gz$/\1/')
    newest[$base]="$f"
  done < extra_list.txt
  for base in "${!newest[@]}"; do
    f="${newest[$base]}"
    # pula o que ja foi baixado pela lista fixa (mesmo nome base)
    skip=0
    for p in "${PKGS[@]}"; do
      pb=$(echo "$p" | sed -E 's/-[0-9][^-]*-[0-9]+$//')
      [ "$pb" = "$base" ] && skip=1
    done
    [ "$skip" = 1 ] && continue
    if [ ! -f "$f" ]; then
      echo "baixando extra $f"
      curl -fsSL -o "$f" "$BASE_URL/$f" || { echo "falhou $f (ignorado)"; continue; }
    fi
    tar -xzf "$f" || true
  done
fi
# Layout: opt/pacbrew/ps4/openorbis/usr/{lib,include}
if [ -d opt/pacbrew/ps4/openorbis ]; then
  echo "PACBREW_PREFIX=$DEST/opt/pacbrew/ps4/openorbis"
  echo "$DEST/opt/pacbrew/ps4/openorbis" > "$DEST/PREFIX"
fi
ls -la opt/pacbrew/ps4/openorbis/usr/lib/libmpv.a
