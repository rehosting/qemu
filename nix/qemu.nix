# Penguin's PANDA-QEMU fork, built from source.
#
# This reproduces ./build.sh (the Dockerfile's single build step): configure a
# TCG-only `build-system` for every Penguin system arch, ninja the
# libqemu-system-*.so shared libraries + qemu-img, run the CFFI generators
# (scripts/penguin-cffi-gen.py + scripts/penguin-env-cffi-gen.py), then package
# everything with scripts/penguin-qemu-package.py.
#
# Two outputs:
#   out  -- the unpacked tree (bin/ include/ lib/ share/), i.e. the contents
#           that the penguin image lays down under /usr/local. This is what
#           penguin's mk-penguin-qemu.nix consumes when this flake replaces the
#           prebuilt release tarball input.
#   dist -- penguin-qemu.tar.gz, the release artifact the CI publishes.
#
# The compiled CFFI env modules (lib/penguin-qemu-env/_penguin_qemu_env_*.so)
# are tied to the building CPython's ABI, so this MUST build against the same
# nixpkgs (hence the same python3) that penguin's image uses -- both flakes pin
# the identical nixpkgs commit for exactly this reason.
{
  lib,
  stdenv,
  src,
  version,
  pkg-config,
  ninja,
  python3,
  perl,
  git, # meson resolves a couple of subproject wraps; git must be on PATH
  glib,
  pixman,
  zlib,
  libcap_ng,
  libslirp, # satisfies the slirp.wrap with the system lib (no network)
  dtc, # provides libfdt (--enable-fdt=system) and the dtc compiler
  fetchFromGitLab,
  # Restrict the built system arch set if desired (build.sh's default covers
  # the full Penguin matrix). Comma-separated, matching PENGUIN_SYSTEM_ARCHES.
  systemArches ? null,
  # KVM acceleration libraries are host-arch specific and optional; the release
  # tarball contract does not require them, so default to skipping.
  enableKvm ? false,
}:

let
  # meson git-wrap subprojects QEMU would otherwise `git clone` at setup time.
  # The build sandbox has no network, so vendor each at its wrap-pinned revision
  # and drop it into subprojects/<name> before configure. Wraps backed by a
  # system library instead (slirp -> libslirp, dtc -> libfdt) are satisfied via
  # buildInputs and need no vendoring.
  #
  # keycodemapdb is a plain wrap-git (build-time keymap database, no system pkg).
  # The berkeley-* subprojects (tests/fp) use patch_directory overlays whose
  # files live in-tree under subprojects/packagefiles/<name>; meson uses an
  # already-populated subproject dir as-is, so we overlay those files ourselves.
  vendoredSubprojects = {
    keycodemapdb = fetchFromGitLab {
      domain = "gitlab.com";
      owner = "qemu-project";
      repo = "keycodemapdb";
      rev = "f5772a62ec52591ff6870b7e8ef32482371f22c6";
      hash = "sha256-GbZ5mrUYLXMi0IX4IZzles0Oyc095ij2xAsiLNJwfKQ=";
    };
    "berkeley-softfloat-3" = fetchFromGitLab {
      domain = "gitlab.com";
      owner = "qemu-project";
      repo = "berkeley-softfloat-3";
      rev = "b64af41c3276f97f0e181920400ee056b9c88037";
      hash = "sha256-Yflpx+mjU8mD5biClNpdmon24EHg4aWBZszbOur5VEA=";
    };
    "berkeley-testfloat-3" = fetchFromGitLab {
      domain = "gitlab.com";
      owner = "qemu-project";
      repo = "berkeley-testfloat-3";
      rev = "e7af9751d9f9fd3b47911f51a5cfd08af256a9ab";
      hash = "sha256-inQAeYlmuiRtZm37xK9ypBltCJ+ycyvIeIYZK8a+RYU=";
    };
  };

  # The python used both to run configure/meson and to compile the CFFI
  # extension modules. pyelftools>=0.31 (DWARF5) and cffi are the script deps;
  # pip/setuptools/wheel let QEMU's mkvenv install its vendored meson wheel
  # offline.
  pythonForBuild = python3.withPackages (ps: [
    ps.pyelftools
    ps.cffi
    ps.setuptools
    ps.wheel
    ps.pip
  ]);
in
stdenv.mkDerivation {
  pname = "penguin-qemu";
  inherit version src;

  outputs = [
    "out"
    "dist"
  ];

  nativeBuildInputs = [
    pkg-config
    ninja
    pythonForBuild
    perl
    git
  ];

  buildInputs = [
    glib
    pixman
    zlib
    libcap_ng
    libslirp
    dtc
  ];

  # The store source is read-only but build.sh writes build-system/ and
  # pyvenv/ into the tree, so work from a writable copy.
  unpackPhase = ''
    runHook preUnpack
    cp -r ${src} qemu-src
    chmod -R u+w qemu-src
    cd qemu-src
    # Vendor the meson git-wrap subprojects offline (the wraps would otherwise
    # `git clone` them, which the build sandbox cannot do). For wraps with a
    # patch_directory, overlay the in-tree subprojects/packagefiles/<name>
    # (which carries the meson.build meson uses to configure the subproject).
    ${lib.concatStringsSep "\n" (
      lib.mapAttrsToList (name: drv: ''
        rm -rf "subprojects/${name}"
        cp -r ${drv} "subprojects/${name}"
        chmod -R u+w "subprojects/${name}"
        if [ -d "subprojects/packagefiles/${name}" ]; then
          cp -rf subprojects/packagefiles/${name}/. "subprojects/${name}/"
        fi
      '') vendoredSubprojects
    )}
    runHook postUnpack
  '';

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    ${lib.optionalString (systemArches != null) ''export PENGUIN_SYSTEM_ARCHES="${systemArches}"''}
    ${lib.optionalString (!enableKvm) ''export PENGUIN_KVM_TARGETS=none''}
    patchShebangs scripts build.sh
    ./build.sh
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$dist"
    cp penguin-qemu.tar.gz "$dist/penguin-qemu.tar.gz"

    # Unpacked tree (the /usr/local overlay payload).
    mkdir -p "$out"
    tar xzf penguin-qemu.tar.gz -C "$out"
    runHook postInstall
  '';

  # qemu-img and the .so libraries are ELF with store-path rpaths already; no
  # stripping surprises needed beyond the defaults.
  meta = {
    description = "Penguin's PANDA-QEMU fork (libqemu-system shared libs + qemu-img + CFFI bindings)";
    homepage = "https://github.com/rehosting/qemu";
    license = lib.licenses.gpl2Plus;
  };
}
