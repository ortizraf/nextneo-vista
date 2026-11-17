#!/bin/bash
# NextNeo Vista — compilar, gravar e monitorar. Por padrão TUDO roda no Docker
# (imagem docker/Dockerfile): não precisa instalar PlatformIO, Python nem toolchain.
#
#   ./tools/firmware.sh [placa] <comando> [opções]
#   placa: s3 (padrão, pasta esp32-s3/), p4 (esp32-p4/) ou shared (testes de lógica)
#
#   ./tools/firmware.sh s3 deploy               # exemplo: testes + gravação na S3
#   ./tools/firmware.sh shared pio test -e native   # testes de lógica no computador
#   ./tools/firmware.sh build                   # compila (env nextneo-vista)
#   ./tools/firmware.sh upload                  # compila e grava (porta detectada), sem testes
#   ./tools/firmware.sh deploy                  # testes + gravação (tools/deploy_nextneo_vista.sh)
#   ./tools/firmware.sh monitor                 # monitor serial (Ctrl+C sai)
#   ./tools/firmware.sh clean                   # limpa o build
#   ./tools/firmware.sh image                   # (re)constrói só a imagem Docker
#   ./tools/firmware.sh pio <args...>           # qualquer comando do pio, ex.: pio test -e native
#   ./tools/firmware.sh esptool <args...>       # esptool, ex.: esptool --port /dev/cu.usbmodem1201 chip-id
#
# Opções (antes ou depois do comando):
#   -e <env>      ambiente do platformio.ini (padrão: nextneo-vista)
#   -p <porta>    porta serial (padrão: detectada; ex. /dev/cu.usbmodem1201, /dev/ttyACM0)
#   --local       SEGUNDA OPÇÃO: usa o PlatformIO instalado no computador, sem Docker
#                 (equivale a USE_DOCKER=0)
#
# Gravação pelo Docker:
#   Linux: a porta é repassada ao container (--device).
#   macOS: o Docker não acessa USB; tools/serial_bridge.pl (Perl do sistema) expõe a
#          porta por RFC 2217 e o container grava por rfc2217://host.docker.internal.
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/lib_port.sh

ENV_NAME=nextneo-vista
BOARD="${BOARD:-s3}"
PORT=""
USE_DOCKER="${USE_DOCKER:-1}"
CMD=""
ARGS=()
while (($#)); do
  case "$1" in
    --local) USE_DOCKER=0 ;;
    --docker) USE_DOCKER=1 ;;
    -e) ENV_NAME="$2"; shift ;;
    -p) PORT="$2"; shift ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    s3|p4|shared) if [[ -z "$CMD" ]]; then BOARD="$1"; else ARGS+=("$1"); fi ;;
    *) if [[ -z "$CMD" ]]; then CMD="$1"; else ARGS+=("$1"); fi
       if [[ "$CMD" == pio || "$CMD" == esptool ]]; then shift; ARGS+=("$@"); break; fi ;;
  esac
  shift
done
[[ -n "$CMD" ]] || { sed -n '2,30p' "$0"; exit 1; }
case "$BOARD" in
  s3|p4) PROJ_DIR="esp32-$BOARD" ;;
  shared) PROJ_DIR=shared ;;
esac
[[ -f "$PROJ_DIR/platformio.ini" ]] || { echo "Erro: $PROJ_DIR/platformio.ini não existe (placa '$BOARD' ainda não criada?)." >&2; exit 1; }

DOCKERFILE=docker/Dockerfile
IMAGE="nextneo-vista-pio:$(shasum "$DOCKERFILE" | cut -c1-12)"
VOLUME=nnv-platformio
BRIDGE_PID=""
TMPD="$(mktemp -d)"
cleanup() { [[ -n "$BRIDGE_PID" ]] && kill "$BRIDGE_PID" 2>/dev/null; rm -rf "$TMPD"; }
trap cleanup EXIT

ensure_image() {
  command -v docker >/dev/null || { echo "Erro: Docker não encontrado. Instale o Docker Desktop (ou use --local)." >&2; exit 1; }
  docker info >/dev/null 2>&1 || { echo "Erro: o Docker não está rodando. Abra o Docker Desktop." >&2; exit 1; }
  if [[ "$CMD" == image ]] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "== Construindo a imagem $IMAGE (só na primeira vez ou quando o Dockerfile muda)"
    docker build -t "$IMAGE" -f "$DOCKERFILE" docker
  fi
}

# Porta serial do computador -> como o container a enxerga.
DOCKER_DEV_OPTS=()
MAPPED=""
map_port() {
  local dev="$1"
  if [[ "$(uname)" == Linux ]]; then
    DOCKER_DEV_OPTS+=(--device "$dev:$dev" --group-add "$(stat -c %g "$dev")")
    MAPPED="$dev"
  else
    if [[ -z "$BRIDGE_PID" ]]; then
      perl tools/serial_bridge.pl "$dev" "$TMPD/port" </dev/null &
      BRIDGE_PID=$!
      for _ in $(seq 50); do [[ -s "$TMPD/port" ]] && break; sleep 0.1; done
      [[ -s "$TMPD/port" ]] || { echo "Erro: a ponte serial não iniciou." >&2; exit 1; }
    fi
    MAPPED="rfc2217://host.docker.internal:$(cat "$TMPD/port")"
    # Por rede o esptool não identifica a placa como USB nativo; por isso o platformio.ini
    # fixa board_upload.before_reset = usb-reset (seção [esp32]).
  fi
}

# esptool local: o do PlatformIO instalado (pacote tool-esptoolpy) ou o do Python.
local_esptool() {
  local py esp="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
  py="$(find_pio_python)"
  if [[ -f "$esp" ]]; then "$py" "$esp" "$@"; else "$py" -m esptool "$@"; fi
}

run_pio() { run_tool pio "$@"; }

# run_tool pio|esptool <args...>: no Docker (padrão) ou local (--local).
run_tool() {
  local tool="$1"; shift
  if [[ "$USE_DOCKER" != 1 ]]; then
    local pio_bin; pio_bin="$(find_pio)"
    (cd "$PROJ_DIR" && if [[ "$tool" == pio ]]; then "$pio_bin" "$@"; else local_esptool "$@"; fi)
    return
  fi
  local cmd=(pio)
  [[ "$tool" == esptool ]] && cmd=(/opt/platformio/penv/bin/python -m esptool)
  ensure_image
  local args=() a tty=()
  for a in "$@"; do
    if [[ "$a" == /dev/* ]]; then
      map_port "$a"; a="$MAPPED"
    fi
    args+=("$a")
  done
  [[ -t 0 && -t 1 ]] && tty=(-it)
  # Build do Docker em .pio-docker/ (não mistura com o .pio/ do PlatformIO local)
  docker run --rm --init ${tty[@]+"${tty[@]}"} \
    --user "$(id -u):$(id -g)" \
    -v "$PWD:/project" -v "$VOLUME:/opt/platformio" -w "/project/$PROJ_DIR" \
    -e PLATFORMIO_WORKSPACE_DIR="/project/$PROJ_DIR/.pio-docker" \
    ${DOCKER_DEV_OPTS[@]+"${DOCKER_DEV_OPTS[@]}"} \
    "$IMAGE" "${cmd[@]}" "${args[@]}"
}

need_port() { [[ -n "$PORT" ]] || PORT="$(detect_port "")"; echo "== Porta: $PORT" >&2; }
BUILD_DIR=$([[ "$USE_DOCKER" == 1 ]] && echo .pio-docker || echo .pio)
echo "== Placa: $PROJ_DIR · modo: $([[ "$USE_DOCKER" == 1 ]] && echo "Docker ($IMAGE)" || echo "local (sem Docker)")" >&2

case "$CMD" in
  image)   ensure_image ;;
  build)   run_pio run -e "$ENV_NAME"
           echo "== Firmware: $PROJ_DIR/$BUILD_DIR/build/$ENV_NAME/" ;;
  upload)  need_port; run_pio run -e "$ENV_NAME" -t upload --upload-port "$PORT" ;;
  monitor) need_port; run_pio device monitor -b 115200 -p "$PORT" ;;
  clean)   run_pio run -e "$ENV_NAME" -t clean ;;
  pio)     run_tool pio ${ARGS[@]+"${ARGS[@]}"} ;;
  esptool) run_tool esptool ${ARGS[@]+"${ARGS[@]}"} ;;
  deploy)  USE_DOCKER="$USE_DOCKER" BOARD="$BOARD" exec ./tools/deploy_nextneo_vista.sh ${PORT:+"$PORT"} ;;
  *) echo "Comando desconhecido: $CMD (use build, upload, deploy, monitor, clean, image, pio ou esptool)" >&2; exit 1 ;;
esac
