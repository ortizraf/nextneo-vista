#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
ENV_NAME=weather-clock
PORT="${1:-}"
if [[ -z "$PORT" ]]; then
  for candidate in /dev/cu.usbmodem*; do
    [[ -e "$candidate" ]] || continue
    PORT="$candidate"
  done
fi
if [[ -z "$PORT" ]]; then
  echo "Erro: ESP32 não encontrado. Conecte o USB ou informe a porta: $0 /dev/cu.usbmodemXXXX" >&2
  exit 1
fi
PIO="$(command -v pio || true)"
if [[ -z "$PIO" ]]; then
  echo "Erro: PlatformIO CLI (pio) não encontrado. Instale PlatformIO Core e tente novamente." >&2
  exit 1
fi
PIO_DIR="$(dirname "$(dirname "$PIO")")"
PIO_PY="$HOME/.platformio/penv/bin/python"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
[[ -x "$PIO_PY" ]] || { echo "Erro: Python do PlatformIO ausente em $PIO_PY" >&2; exit 1; }
[[ -f "$ESPTOOL" ]] || { echo "Erro: esptool ausente. Execute pio run -e $ENV_NAME primeiro." >&2; exit 1; }
IMAGE=".pio/build/$ENV_NAME/firmware.bin"
echo "Compilando $ENV_NAME..."
"$PIO" run -e "$ENV_NAME"
echo "Gravando somente o aplicativo em $PORT (offset 0x10000)..."
"$PIO_PY" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 write_flash --flash_size 16MB 0x10000 "$IMAGE"
echo "Atualização concluída. Confirme 'Hash of data verified' acima."
