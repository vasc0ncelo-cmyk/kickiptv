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
# Layout: opt/pacbrew/ps4/openorbis/usr/{lib,include}
if [ -d opt/pacbrew/ps4/openorbis ]; then
  echo "PACBREW_PREFIX=$DEST/opt/pacbrew/ps4/openorbis"
  echo "$DEST/opt/pacbrew/ps4/openorbis" > "$DEST/PREFIX"
fi
ls -la opt/pacbrew/ps4/openorbis/usr/lib/libmpv.a
