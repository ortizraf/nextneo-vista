# Shared helpers for the NextNeo Vista scripts (macOS and Linux). Source it:
#   source "$(dirname "$0")/lib_port.sh"

# Serial port of the board: the argument if given, else the last USB serial port found.
#   macOS: /dev/cu.usbmodem*      Linux: /dev/ttyACM* (native USB) or /dev/ttyUSB* (adapter)
detect_port() {
  local port="${1:-}" c
  if [[ -z "$port" ]]; then
    for c in /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB*; do
      [[ -e "$c" ]] && port="$c"
    done
  fi
  if [[ -z "$port" ]]; then
    echo "Erro: placa não encontrada. Conecte o USB ou informe a porta, ex.:" >&2
    echo "  macOS: $0 /dev/cu.usbmodem1201    Linux: $0 /dev/ttyACM0" >&2
    return 1
  fi
  if [[ ! -e "$port" ]]; then
    echo "Erro: a porta $port não existe. Confira o cabo USB (precisa ser de dados)." >&2
    return 1
  fi
  if [[ ! -r "$port" || ! -w "$port" ]]; then
    echo "Erro: sem permissão na porta $port." >&2
    echo "  Linux: sudo usermod -aG dialout \$USER  (e entre de novo na sessão)" >&2
    return 1
  fi
  echo "$port"
}

# PlatformIO CLI: `pio` on the PATH, else the one installed by PlatformIO.
find_pio() {
  local pio
  pio="$(command -v pio || true)"
  [[ -z "$pio" && -x "$HOME/.platformio/penv/bin/pio" ]] && pio="$HOME/.platformio/penv/bin/pio"
  [[ -n "$pio" ]] || { echo "Erro: PlatformIO (pio) não encontrado. Instale o PlatformIO Core." >&2; return 1; }
  echo "$pio"
}

# Python that has esptool / pyserial: PlatformIO's own environment (same path on
# macOS and Linux), else the system python3.
find_pio_python() {
  if [[ -x "$HOME/.platformio/penv/bin/python" ]]; then echo "$HOME/.platformio/penv/bin/python"; else command -v python3; fi
}

# Size of a file in bytes (same on macOS and Linux).
file_size() { wc -c < "$1" | tr -d ' '; }
