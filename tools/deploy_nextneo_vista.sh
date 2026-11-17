#!/bin/bash
# NextNeo Vista — deploy na placa: TESTA primeiro, grava o app só se tudo passar.
#
#   ./tools/deploy_nextneo_vista.sh                    # testes + gravação (porta detectada)
#   ./tools/deploy_nextneo_vista.sh /dev/cu.usbmodem1201   # macOS (porta manual)
#   ./tools/deploy_nextneo_vista.sh /dev/ttyACM0           # Linux (porta manual)
#   QUICK=1 ./tools/deploy_nextneo_vista.sh            # mudança simples: teste rápido (~2 min no total)
#   SUITES="test_screens" ./tools/deploy_nextneo_vista.sh   # só algumas suítes
#   CLEAN=1 ./tools/deploy_nextneo_vista.sh            # limpa o build antes
#   MONITOR=1 ./tools/deploy_nextneo_vista.sh          # abre o monitor serial no final
#   USE_DOCKER=0 ./tools/deploy_nextneo_vista.sh       # sem Docker (PlatformIO instalado)
# Padrão: tudo roda no Docker via tools/firmware.sh (também: ./tools/firmware.sh deploy).
#   SKIP_TESTS=1 ./tools/deploy_nextneo_vista.sh       # EMERGÊNCIA: grava sem testar
#
# Passos:
#   0. Testes de lógica no computador (pio test -e native, segundos). Falhou: PARA.
#   1. Testes automáticos na placa: completos (test_menu, test_screens, test_lifecycle,
#      ~7 min) ou, com QUICK=1, só test_smoke (~1 min). Se algum falhar, PARA:
#      nada do app é gravado e a placa fica com o firmware de teste.
#   2. Compila o app e confere se cabe na partição app0 (3264 KB).
#   3. Grava SOMENTE o app no offset 0x10000. Bootloader, tabela de partições e NVS
#      (Wi-Fi/configurações) ficam intactos. Tabela: default_8MB.csv (platformio.ini).
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/lib_port.sh

ENV_NAME=nextneo-vista
APP_OFFSET=0x10000
APP_SLOT_BYTES=$((0x330000))   # app0 = 3264 KB (default_8MB.csv)
# Docker (padrão): tudo via tools/firmware.sh. USE_DOCKER=0: PlatformIO instalado.
USE_DOCKER="${USE_DOCKER:-1}"; export USE_DOCKER
if [[ "$USE_DOCKER" == 1 ]]; then BUILD_DIR=.pio-docker; else BUILD_DIR=.pio; fi
IMAGE="$BUILD_DIR/build/$ENV_NAME/nextneo-vista.bin"
mkdir -p logs                  # saída completa dos testes / gravação (fora do git)

# ---- PlatformIO e porta USB (macOS e Linux: tools/lib_port.sh)
PIO=(./tools/firmware.sh pio)
ESPTOOL_CMD=(./tools/firmware.sh esptool)
PORT="$(detect_port "${1:-}")"
echo "== Porta: $PORT"

# ---- 0 + 1. testes (computador, depois placa)
if [[ "${SKIP_TESTS:-0}" == "1" ]]; then
  echo "== ATENÇÃO: testes pulados (SKIP_TESTS=1)"
else
  echo "== 0/3 Testes de lógica no computador (saída completa em logs/native.log)"
  if ! "${PIO[@]}" test -e native > logs/native.log 2>&1; then
    grep -E ":FAIL|error:|native +test_|test cases" logs/native.log | tail -20 || true
    echo "Erro: testes de lógica falharam. Nada foi gravado na placa." >&2
    exit 1
  fi
  grep -E "test cases" logs/native.log | tail -1
  # Default: the full board suites. QUICK=1: the smoke suite only (small changes).
  if [[ -z "${SUITES:-}" ]]; then
    if [[ "${QUICK:-0}" == "1" ]]; then SUITES="test_smoke"; else SUITES="test_menu test_screens test_lifecycle test_garden"; fi
  fi
  FILTER=()
  for s in $SUITES; do FILTER+=(-f "$s"); done
  echo "== 1/3 Testes na placa: $SUITES (saída completa em logs/test.log)"
  if ! "${PIO[@]}" test -e "$ENV_NAME" ${FILTER[@]+"${FILTER[@]}"} --upload-port "$PORT" --test-port "$PORT" > logs/test.log 2>&1; then
    grep -E ":FAIL|error:|$ENV_NAME +test_|test cases" logs/test.log | tail -20 || true
    echo "Erro: testes falharam. O app NÃO foi gravado (a placa está com o firmware de teste)." >&2
    echo "Veja logs/test.log. Depois de corrigir, rode este script de novo." >&2
    exit 1
  fi
  grep -E "$ENV_NAME +test_|test cases" logs/test.log | tail -6
fi

# ---- 2. build
if [[ "${CLEAN:-0}" == "1" ]]; then
  echo "== Limpando build anterior"
  "${PIO[@]}" run -e "$ENV_NAME" -t clean
fi
echo "== 2/3 Compilando $ENV_NAME"
"${PIO[@]}" run -e "$ENV_NAME" > logs/build.log 2>&1 || { tail -30 logs/build.log; echo "Erro: falha na compilação (logs/build.log)." >&2; exit 1; }
grep -E "RAM:|Flash:" logs/build.log || true

SIZE=$(file_size "$IMAGE")
echo "== Tamanho do app: $SIZE bytes (limite $APP_SLOT_BYTES)"
(( SIZE <= APP_SLOT_BYTES )) || { echo "Erro: o app não cabe na partição app0 de 3264 KB. Nada foi gravado." >&2; exit 1; }

# ---- 3. gravação (somente app0)
echo "== 3/3 Gravando somente o aplicativo em $APP_OFFSET"
"${ESPTOOL_CMD[@]}" --chip esp32s3 --port "$PORT" --baud 921600 --before usb-reset \
  write_flash --flash_size 16MB "$APP_OFFSET" "$IMAGE" > logs/upload.log 2>&1 \
  || { tail -20 logs/upload.log; echo "Erro: falha na gravação (logs/upload.log)." >&2; exit 1; }
grep -q "Hash of data verified" logs/upload.log || { echo "Erro: gravação sem verificação (logs/upload.log)." >&2; exit 1; }
echo "== Deploy concluído: testes ok, app gravado e verificado."

# ---- monitor serial (opcional)
if [[ "${MONITOR:-0}" == "1" ]]; then
  echo "== Monitor serial (Ctrl+C para sair)"
  sleep 2
  "${PIO[@]}" device monitor -b 115200 -p "$PORT"
fi
