#!/bin/bash
set -e

KVM_QEMU_DIR="$(dirname "$0")"
cd "$KVM_QEMU_DIR"

COMMON_CONFIGURE_ARGS=(
    --disable-docs
    # Modules must stay OFF: scripts/penguin-qemu-package.py ships only the
    # libqemu-*.so libraries, never QEMU's module directory. With modules
    # enabled, anything meson chooses to modularise (notably virtio-gpu) was
    # built but never packaged, so `-device virtio-gpu` failed at runtime with
    # "not a valid device model name". Building in costs ~36 KB/arch.
    --disable-modules
    --extra-cflags=-fPIC
    --disable-xen
    --disable-opengl
    --disable-spice
    --disable-gtk
    --disable-sdl
    # VNC is the only display backend we ship: it needs no host display, and
    # its only hard dependency is pixman, which we already have (meson.build's
    # `vnc = declare_dependency()` is a dummy). Costs ~141 KB/arch and zero new
    # store paths. Its optional extras are pinned off rather than left on auto
    # so the build can't silently acquire deps if buildInputs ever grow.
    --enable-vnc
    # libjpeg (tight encoding) and libpng (screendump -f png) are both cheap
    # against the image and directly improve the feature above. SASL stays off:
    # penguin authenticates VNC with password-secret, so the SASL auth path
    # would be an unused parser.
    --enable-vnc-jpeg
    --disable-vnc-sasl
    --enable-png
    --disable-qemu-vnc
    # dbus-display auto-enables off glib alone, which IS in our buildInputs.
    # Pin it off so it can't drift in unnoticed.
    --disable-dbus-display
    --enable-virtfs
    --disable-vhost-net
    --enable-vhost-user
    --disable-vhost-vdpa
    --disable-vhost-kernel
    # The block/USB/compression/disassembler group below is enabled
    # deliberately, having been costed against the penguin image rather than
    # against this package alone: every library here is either already in the
    # image (curl, lzo, bzip2, libpng, libjpeg) or under ~5 MB marginal
    # (libiscsi, libnfs, rdma-core, libusb1, usbredir). capstone is the one
    # genuinely new dependency at ~29 MB -- the image ships only the Python
    # binding, not the C library.
    --enable-capstone
    --enable-libiscsi
    --enable-libnfs
    --enable-libusb
    --enable-usb-redir
    --enable-lzo
    --enable-snappy
    --enable-bzip2
    --enable-rdma
    --enable-curl
    # linux-aio stays off: host block I/O performance only, and it changes I/O
    # behaviour rather than adding a capability.
    --disable-linux-aio
)

# Both `intel64` and `x86_64` are listed: they map to the same x86_64-softmmu
# target (so only one library is built, then deduped), but each produces its own
# CFFI header / env-module / lib-alias entry. Penguin's arch_registry canonical
# name for 64-bit x86 is `x86_64` (with `intel64` an accepted alias), and its
# qemu loader resolves `libqemu-system-<arch>.so` by the arch name with no
# intel64<->x86_64 fallback -- so the package must ship BOTH names or x86_64
# guests fail with "Unable to find QEMU library: libqemu-system-x86_64.so".
PENGUIN_SYSTEM_ARCHES="${PENGUIN_SYSTEM_ARCHES:-armel,aarch64,mipsel,mipseb,mips64el,mips64eb,powerpc,powerpc64,powerpc64el,powerpc64le,riscv64,loongarch64,intel64,x86_64}"

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
        powerpc64|powerpc64le|powerpc64el|ppc64)
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
python3 scripts/penguin-env-cffi-gen.py \
    --build-dir build-system \
    --manifest build-system/qemu_cffi_system_manifest.json

if [ "${PENGUIN_KVM_TARGETS:-}" = "none" ]; then
    # Explicitly skip the KVM build (e.g. reproducible/cross-host Nix builds
    # that only want the TCG system libraries and qemu-img).
    kvm_targets=
elif [ -n "${PENGUIN_KVM_TARGETS:-}" ]; then
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
    python3 scripts/penguin-env-cffi-gen.py \
        --build-dir build-kvm \
        --manifest build-kvm/qemu_cffi_kvm_manifest.json
fi

python3 scripts/penguin-qemu-package.py --output penguin-qemu.tar.gz
