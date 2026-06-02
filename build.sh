#!/bin/bash
set -e

KVM_QEMU_DIR="$(dirname "$0")"
cd "$KVM_QEMU_DIR"

COMMON_CONFIGURE_ARGS=(
    --disable-docs
    --enable-modules
    --extra-cflags=-fPIC
    --disable-xen
    --disable-opengl
    --disable-spice
    --disable-gtk
    --disable-sdl
    --disable-vnc
    --enable-virtfs
    --disable-vhost-net
    --enable-vhost-user
    --disable-vhost-vdpa
    --disable-vhost-kernel
    --disable-capstone
    --disable-libiscsi
    --disable-libnfs
    --disable-libusb
    --disable-usb-redir
    --disable-lzo
    --disable-snappy
    --disable-bzip2
    --disable-rdma
    --disable-curl
    --disable-linux-aio
)

PENGUIN_SYSTEM_ARCHES="${PENGUIN_SYSTEM_ARCHES:-armel,aarch64,mipsel,mipseb,mips64el,mips64eb,powerpc,powerpc64,powerpc64le,riscv64,loongarch64,intel64}"

configure_build_dir() {
    local build_dir="$1"
    shift

    mkdir -p "$build_dir"
    if [ ! -f "$build_dir/config.status" ]; then
        (
            cd "$build_dir"
            ../configure "${COMMON_CONFIGURE_ARGS[@]}" "$@"
        )
    fi
}

append_unique() {
    local value="$1"
    shift
    local existing

    for existing in "$@"; do
        if [ "$existing" = "$value" ]; then
            return 1
        fi
    done

    printf "%s\n" "$value"
}

penguin_system_arch_to_qemu_target() {
    case "$1" in
        intel64|x86_64)
            printf "x86_64-softmmu\n"
            ;;
        armel|arm)
            printf "arm-softmmu\n"
            ;;
        aarch64)
            printf "aarch64-softmmu\n"
            ;;
        mipsel)
            printf "mipsel-softmmu\n"
            ;;
        mipseb|mips)
            printf "mips-softmmu\n"
            ;;
        mips64el)
            printf "mips64el-softmmu\n"
            ;;
        mips64eb|mips64)
            printf "mips64-softmmu\n"
            ;;
        powerpc|ppc)
            printf "ppc-softmmu\n"
            ;;
        powerpc64|powerpc64le|ppc64)
            printf "ppc64-softmmu\n"
            ;;
        riscv64)
            printf "riscv64-softmmu\n"
            ;;
        loongarch64)
            printf "loongarch64-softmmu\n"
            ;;
        *)
            printf "Unsupported Penguin system arch: %s\n" "$1" >&2
            return 1
            ;;
    esac
}

build_system_target_list() {
    local arch
    local target
    local targets=()

    IFS=',' read -ra split_arches <<< "$PENGUIN_SYSTEM_ARCHES"
    for arch in "${split_arches[@]}"; do
        target="$(penguin_system_arch_to_qemu_target "$arch")"
        if append_unique "$target" "${targets[@]}" >/dev/null; then
            targets+=("$target")
        fi
    done

    local IFS=,
    printf "%s\n" "${targets[*]}"
}

build_system_lib_list() {
    local arch
    local target
    local lib
    local libs=()

    IFS=',' read -ra split_arches <<< "$PENGUIN_SYSTEM_ARCHES"
    for arch in "${split_arches[@]}"; do
        target="$(penguin_system_arch_to_qemu_target "$arch")"
        lib="libqemu-system-${target%-softmmu}.so"
        if append_unique "$lib" "${libs[@]}"; then
            libs+=("$lib")
        fi
    done

    printf "%s\n" "${libs[@]}"
}

stage_system_lib_aliases() {
    local arch
    local target
    local source
    local alias

    IFS=',' read -ra split_arches <<< "$PENGUIN_SYSTEM_ARCHES"
    for arch in "${split_arches[@]}"; do
        target="$(penguin_system_arch_to_qemu_target "$arch")"
        source="build-system/libqemu-system-${target%-softmmu}.so"
        alias="build-system/libqemu-system-${arch}.so"

        if [ "$source" != "$alias" ]; then
            cp -f "$source" "$alias"
        fi
    done
}

targets_to_kvm_libs() {
    local targets="$1"
    local target

    IFS=',' read -ra split_targets <<< "$targets"
    for target in "${split_targets[@]}"; do
        printf "libqemu-kvm-%s.so\n" "${target%-softmmu}"
    done
}

system_targets="$(build_system_target_list)"

configure_build_dir build-system \
    --target-list="$system_targets" \
    --enable-tcg \
    --disable-kvm

mapfile -t system_libs < <(build_system_lib_list)
ninja -C build-system "${system_libs[@]}" qemu-img
stage_system_lib_aliases
python3 scripts/penguin-cffi-gen.py \
    --mode system \
    --build-dir build-system \
    --arches "$PENGUIN_SYSTEM_ARCHES"

if [ -n "${PENGUIN_KVM_TARGETS:-}" ]; then
    kvm_targets="$PENGUIN_KVM_TARGETS"
else
    case "$(uname -m)" in
        x86_64)
            kvm_targets=x86_64-softmmu
            ;;
        aarch64)
            kvm_targets=aarch64-softmmu
            ;;
        *)
            kvm_targets=
            ;;
    esac
fi

if [ -n "$kvm_targets" ]; then
    configure_build_dir build-kvm \
        --target-list="$kvm_targets" \
        --enable-kvm \
        --disable-tcg

    mapfile -t kvm_libs < <(targets_to_kvm_libs "$kvm_targets")
    ninja -C build-kvm "${kvm_libs[@]}"
    python3 scripts/penguin-cffi-gen.py \
        --mode kvm \
        --build-dir build-kvm \
        --targets "$kvm_targets"
fi

python3 scripts/penguin-qemu-package.py --output penguin-qemu.tar.gz
