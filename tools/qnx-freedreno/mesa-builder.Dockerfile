FROM python:3.11-slim-bullseye AS python_tools

RUN python3 -m pip install --no-cache-dir \
      meson==1.11.2 mako pyyaml packaging

FROM qnx65-armv7-toolchain:latest

USER root
RUN apt-get -o Acquire::Check-Valid-Until=false update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      bison flex g++ pkg-config && \
    rm -rf /var/lib/apt/lists/*

COPY --from=python_tools /usr/local /usr/local

ENV PATH="/usr/local/bin:${PATH}"
