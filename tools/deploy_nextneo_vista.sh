#!/bin/bash
# NextNeo Vista — deploy na placa: TESTA primeiro, grava o app só se tudo passar.
#
#   ./tools/deploy_nextneo_vista.sh                    # testes + gravação (porta detectada)
#   ./tools/deploy_nextneo_vista.sh /dev/cu.usbmodem1201
#   SUITES="test_screens" ./tools/deploy_nextneo_vista.sh   # só algumas suítes (mais rápido)
#   CLEAN=1 ./tools/deploy_nextneo_vista.sh            # limpa o build antes
#   MONITOR=1 ./tools/deploy_nextneo_vista.sh          # abre o monitor serial no final
#   SKIP_TESTS=1 ./tools/deploy_nextneo_vista.sh       # EMERGÊNCIA: grava sem testar
#
# Passos:
#   1. Testes automáticos na placa (pio test: test/test_*). Se algum falhar, PARA:
#      nada do app é gravado e a placa fica com o firmware de teste.
#   2. Compila o app e confere se cabe na partição app0 (3264 KB).
#   3. Grava SOMENTE o app no offset 0x10000. Bootloader, tabela de partições e NVS
#      (Wi-Fi/configurações) ficam intactos. Tabela: default_8MB.csv (platformio.ini).
set -euo pipefail
cd "$(dirname "$0")/.."

ENV_NAME=nextneo-vista
APP_OFFSET=0x10000
APP_SLOT_BYTES=$((0x330000))   # app0 = 3264 KB (default_8MB.csv)
IMAGE=".pio/build/$ENV_NAME/nextneo-vista.bin"
mkdir -p logs                  # saída completa dos testes / gravação (fora do git)

# ---- PlatformIO
PIO="$(command -v pio || true)"
if [[ -z "$PIO" && -x "$HOME/.platformio/penv/bin/pio" ]]; then PIO="$HOME/.platformio/penv/bin/pio"; fi
[[ -n "$PIO" ]] || { echo "Erro: PlatformIO (pio) não encontrado. Instale o PlatformIO Core." >&2; exit 1; }
PIO_PY="$HOME/.platformio/penv/bin/python"

# ---- porta USB
PORT="${1:-}"
if [[ -z "$PORT" ]]; then
  for candidate in /dev/cu.usbmodem*; do
    if [[ -e "$candidate" ]]; then PORT="$candidate"; fi
  done
fi
[[ -n "$PORT" ]] || { echo "Erro: placa não encontrada. Conecte o USB ou informe a porta: $0 /dev/cu.usbmodemXXXX" >&2; exit 1; }
echo "== Porta: $PORT"

# ---- 1. testes
if [[ "${SKIP_TESTS:-0}" == "1" ]]; then
  echo "== ATENÇÃO: testes pulados (SKIP_TESTS=1)"
else
  FILTER=()
  for s in ${SUITES:-}; do FILTER+=(-f "$s"); done
  echo "== 1/3 Testes na placa (leva alguns minutos; saída completa em logs/test.log)"
  if ! "$PIO" test -e "$ENV_NAME" ${FILTER[@]+"${FILTER[@]}"} --upload-port "$PORT" --test-port "$PORT" > logs/test.log 2>&1; then
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
  "$PIO" run -e "$ENV_NAME" -t clean
fi
echo "== 2/3 Compilando $ENV_NAME"
"$PIO" run -e "$ENV_NAME" > logs/build.log 2>&1 || { tail -30 logs/build.log; echo "Erro: falha na compilação (logs/build.log)." >&2; exit 1; }
grep -E "RAM:|Flash:" logs/build.log || true

SIZE=$(stat -f%z "$IMAGE" 2>/dev/null || stat -c%s "$IMAGE")
echo "== Tamanho do app: $SIZE bytes (limite $APP_SLOT_BYTES)"
(( SIZE <= APP_SLOT_BYTES )) || { echo "Erro: o app não cabe na partição app0 de 3264 KB. Nada foi gravado." >&2; exit 1; }

# ---- 3. gravação (somente app0)
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
if [[ -f "$ESPTOOL" ]]; then ESPTOOL_CMD=("$PIO_PY" "$ESPTOOL"); else ESPTOOL_CMD=("$PIO_PY" -m esptool); fi
echo "== 3/3 Gravando somente o aplicativo em $APP_OFFSET"
"${ESPTOOL_CMD[@]}" --chip esp32s3 --port "$PORT" --baud 921600 \
  write_flash --flash_size 16MB "$APP_OFFSET" "$IMAGE" > logs/upload.log 2>&1 \
  || { tail -20 logs/upload.log; echo "Erro: falha na gravação (logs/upload.log)." >&2; exit 1; }
grep -q "Hash of data verified" logs/upload.log || { echo "Erro: gravação sem verificação (logs/upload.log)." >&2; exit 1; }
echo "== Deploy concluído: testes ok, app gravado e verificado."

# ---- monitor serial (opcional)
if [[ "${MONITOR:-0}" == "1" ]]; then
  echo "== Monitor serial (Ctrl+C para sair)"
  sleep 2
  "$PIO" device monitor -b 115200 -p "$PORT"
fi
