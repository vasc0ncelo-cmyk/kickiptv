#!/usr/bin/env bash
# Baixa a ultima release do OpenOrbis e prepara sce_sys/sce_module a partir do sample hello_world.
set -euo pipefail
mkdir -p tc && cd tc
# O GitHub as vezes responde HTTP 500/502 por alguns segundos: tenta de novo (ate 5 vezes) antes de desistir.
ok=0
for i in 1 2 3 4 5; do
  if gh release download --repo OpenOrbis/OpenOrbis-PS4-Toolchain --pattern '*' --clobber; then ok=1; break; fi
  echo "Falha ao baixar release (tentativa $i de 5)"
  [ "$i" -lt 5 ] && sleep $((i * 15))
done
[ "$ok" = 1 ] || { echo "Falha ao baixar release depois de 5 tentativas"; exit 1; }
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
