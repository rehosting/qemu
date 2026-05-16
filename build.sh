#!/bin/bash
set -e

KVM_QEMU_DIR="$(dirname "$0")"
cd "$KVM_QEMU_DIR"

mkdir -p build
cd build

if [ ! -f config.status ]; then
    ../configure --target-list=x86_64-softmmu,i386-softmmu \
                 --enable-kvm \
                 --disable-tcg \
                 --disable-docs \
                 --enable-modules \
                 --extra-cflags="-fPIC" \
                 --disable-xen \
                 --disable-opengl \
                 --disable-spice \
                 --disable-gtk \
                 --disable-sdl \
                 --disable-vnc \
                 --enable-virtfs \
                 --disable-vhost-net \
                 --enable-vhost-user \
                 --disable-vhost-vdpa \
                 --disable-vhost-kernel \
                 --disable-capstone \
                 --disable-libiscsi \
                 --disable-libnfs \
                 --disable-libusb \
                 --disable-usb-redir \
                 --disable-lzo \
                 --disable-snappy \
                 --disable-bzip2 \
                 --disable-rdma \
                 --disable-curl \
                 --disable-linux-aio
fi

ninja libqemu-kvm-x86_64.so libqemu-kvm-i386.so
