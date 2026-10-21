#!/usr/bin/env bash
# Baixa a ultima release do OpenOrbis e prepara sce_sys/sce_module a partir do sample hello_world.
set -euo pipefail
mkdir -p tc && cd tc
gh release download --repo OpenOrbis/OpenOrbis-PS4-Toolchain --pattern '*' || { echo "Falha ao baixar release"; exit 1; }
ls -la
for f in *.tar.gz *.tgz; do [ -f "$f" ] && tar xzf "$f"; done
for f in *.zip; do [ -f "$f" ] && unzip -q "$f"; done
LINK=$(find . -name link.x | head -n1)
[ -n "$LINK" ] || { echo "link.x nao encontrado. Conteudo:"; find . -maxdepth 3 | head -80; exit 1; }
ROOT=$(cd "$(dirname "$LINK")" && pwd)
echo "OO_PS4_TOOLCHAIN=$ROOT" >> "$GITHUB_ENV"
chmod +x "$ROOT"/bin/linux/* || true
cd ..
SAMPLE="$ROOT/samples/hello_world"
[ -d "$SAMPLE/sce_sys" ] || { echo "sample hello_world nao encontrado em $SAMPLE"; find "$ROOT" -maxdepth 2 | head -60; exit 1; }
cp -r "$SAMPLE/sce_sys" .
[ -d "$SAMPLE/sce_module" ] && cp -r "$SAMPLE/sce_module" . || true
echo "Toolchain pronto em $ROOT"
