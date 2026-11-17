#!/bin/bash
# NextNeo Vista — imagens das telas para a documentação (docs/screens/*.png).
#   ./tools/capture_screenshots.sh [porta]
# Grava o firmware de captura (test/test_screenshots), recebe as imagens pela USB e,
# no fim, grava o app de volta com o deploy normal (com testes).
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/lib_port.sh
PIO="$(find_pio)"
PY="$(find_pio_python)"
PORT="$(detect_port "${1:-}")"
mkdir -p logs
# ONLY=garden: só a tela Jardim, a planta em cada estágio (logs/garden-shots); no fim volta
# o app com o deploy rápido (QUICK=1), já que só as fotos mudaram
OUT=docs/screens
if [ "${ONLY:-}" = garden ]; then export PLATFORMIO_BUILD_FLAGS="-DSHOT_GARDEN"; OUT=logs/garden-shots; fi
echo "== Gravando o firmware de captura"
(cd esp32-s3 && "$PIO" test -e nextneo-vista-docs --without-testing --upload-port "$PORT") > logs/screenshots-build.log 2>&1 \
  || { tail -20 logs/screenshots-build.log; exit 1; }
echo "== Recebendo as imagens (cerca de 3 minutos)"
"$PY" tools/capture_screenshots.py "$PORT" "$OUT"
echo "== Voltando o app para a placa"
if [ "${ONLY:-}" = garden ]; then
  echo "Imagens em $OUT"
  unset PLATFORMIO_BUILD_FLAGS                 # o app não leva a flag da captura
  QUICK=1 SKIP_TESTS="${SKIP_TESTS:-0}" ./tools/deploy_nextneo_vista.sh "$PORT"
else
  SKIP_TESTS="${SKIP_TESTS:-0}" ./tools/deploy_nextneo_vista.sh "$PORT"
fi
