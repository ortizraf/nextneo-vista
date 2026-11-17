# Imagem de build/gravação do NextNeo Vista: PlatformIO Core + o que ele precisa.
# Usada por tools/firmware.sh (modo padrão). Nada disso precisa estar instalado no computador.
FROM python:3.12-slim-bookworm

RUN apt-get update \
 && apt-get install -y --no-install-recommends git build-essential ca-certificates \
 && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir platformio==6.2.0

# Pacotes do PlatformIO (toolchain, framework, bibliotecas) ficam num volume Docker
# (nnv-platformio) montado aqui; 777 para funcionar com qualquer --user.
ENV PLATFORMIO_CORE_DIR=/opt/platformio \
    HOME=/tmp \
    PYTHONUNBUFFERED=1
RUN mkdir -p /opt/platformio && chmod 777 /opt/platformio

WORKDIR /project
