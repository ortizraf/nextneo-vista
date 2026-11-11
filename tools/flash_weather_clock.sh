#!/bin/bash
# NextNeo Vista — compilar e gravar o app principal (weather-clock) com segurança.
#
#   ./tools/flash_weather_clock.sh                 # detecta a porta /dev/cu.usbmodem*
#   ./tools/flash_weather_clock.sh /dev/cu.usbmodem1201
#   CLEAN=0 ./tools/flash_weather_clock.sh         # sem limpar o build (mais rápido)
#   MONITOR=0 ./tools/flash_weather_clock.sh       # sem abrir o monitor serial
#
# Grava SOMENTE o aplicativo no offset 0x10000 (app0). Bootloader, tabela de
# partições e NVS (Wi-Fi/configurações) ficam intactos.
# A placa usa a tabela default_8MB.csv (app0 = 3264 KB), fixada no platformio.ini.
set -euo pipefail
cd "$(dirname "$0")/.."

ENV_NAME=weather-clock
APP_OFFSET=0x10000
APP_SLOT_BYTES=$((0x330000))   # app0 = 3264 KB (default_8MB.csv)
IMAGE=".pio/build/$ENV_NAME/firmware.bin"

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

# ---- build
if [[ "${CLEAN:-1}" == "1" ]]; then
  echo "== Limpando build anterior"
  "$PIO" run -e "$ENV_NAME" -t clean
fi
echo "== Compilando $ENV_NAME"
"$PIO" run -e "$ENV_NAME"

SIZE=$(stat -f%z "$IMAGE" 2>/dev/null || stat -c%s "$IMAGE")
echo "== Tamanho do app: $SIZE bytes (limite $APP_SLOT_BYTES)"
(( SIZE <= APP_SLOT_BYTES )) || { echo "Erro: o app não cabe na partição app0 de 3264 KB. Nada foi gravado." >&2; exit 1; }
shasum -a 256 "$IMAGE" 2>/dev/null || true

# ---- gravação (somente app0)
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
if [[ -f "$ESPTOOL" ]]; then
  ESPTOOL_CMD=("$PIO_PY" "$ESPTOOL")
else
  ESPTOOL_CMD=("$PIO_PY" -m esptool)
fi
echo "== Gravando somente o aplicativo em $APP_OFFSET"
"${ESPTOOL_CMD[@]}" --chip esp32s3 --port "$PORT" --baud 921600 \
  write_flash --flash_size 16MB "$APP_OFFSET" "$IMAGE"
echo "== Gravação concluída (confira 'Hash of data verified' acima)"

# ---- monitor serial
if [[ "${MONITOR:-1}" == "1" ]]; then
  echo "== Monitor serial (Ctrl+C para sair)"
  sleep 2
  "$PIO" device monitor -b 115200 -p "$PORT"
fi
