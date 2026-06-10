#syntax=docker/dockerfile:1.17-labs
ARG REGISTRY="docker.io"
ARG BASE_IMAGE="${REGISTRY}/ubuntu:22.04"

FROM ${BASE_IMAGE} AS builder
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        device-tree-compiler \
        git \
        libcap-ng-dev \
        libfdt-dev \
        libglib2.0-dev \
        libpixman-1-dev \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        python3-setuptools \
        python3-tomli \
        python3-venv \
        wget \
        zlib1g-dev && \
    rm -rf /var/lib/apt/lists/*

# penguin-env-cffi-gen.py parses the built libraries' DWARF (needs recent
# pyelftools for DWARF5) and verifies generated layouts with cffi.
RUN pip3 install --no-cache-dir "pyelftools>=0.31" cffi

COPY --exclude=.git \
     --exclude=.github \
     --exclude=build-system \
     --exclude=build-kvm \
     --exclude=penguin-qemu.tar.gz \
     . /qemu/

WORKDIR /qemu
RUN ./build.sh

FROM scratch AS package
COPY --from=builder /qemu/penguin-qemu.tar.gz /penguin-qemu.tar.gz
