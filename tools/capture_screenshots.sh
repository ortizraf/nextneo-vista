#!/bin/bash
# NextNeo Vista — imagens das telas para a documentação (docs/screens/*.png).
#   ./tools/capture_screenshots.sh [porta]
# Grava o firmware de captura (test/test_screenshots), recebe as imagens pela USB e,
# no fim, grava o app de volta com o deploy normal (com testes).
set -euo pipefail
cd "$(dirname "$0")/.."
PIO="$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")"
PY="$HOME/.platformio/penv/bin/python"
PORT="${1:-}"
if [[ -z "$PORT" ]]; then for c in /dev/cu.usbmodem*; do [[ -e "$c" ]] && PORT="$c"; done; fi
[[ -n "$PORT" ]] || { echo "Erro: placa não encontrada." >&2; exit 1; }
mkdir -p logs
echo "== Gravando o firmware de captura"
"$PIO" test -e nextneo-vista-docs --without-testing --upload-port "$PORT" > logs/screenshots-build.log 2>&1 \
  || { tail -20 logs/screenshots-build.log; exit 1; }
echo "== Recebendo as imagens (cerca de 3 minutos)"
"$PY" tools/capture_screenshots.py "$PORT" docs/screens
echo "== Voltando o app para a placa"
SKIP_TESTS="${SKIP_TESTS:-0}" ./tools/deploy_nextneo_vista.sh "$PORT"
