{
  description = "Penguin's PANDA-QEMU fork: libqemu-system shared libraries + qemu-img + CFFI bindings";

  nixConfig = {
    extra-substituters = [ "https://rehosting-tools.cachix.org" ];
    extra-trusted-public-keys = [
      "rehosting-tools.cachix.org-1:iNKSaFwG7MfGn6Fk7oTmIcLHqfffQ+cQIE5gWc6MlY0="
    ];
  };

  # Pinned to the same nixpkgs commit as penguin / penguin-tools / fw2tar so the
  # closures (and the building CPython ABI the CFFI env modules are tied to) all
  # match, and Cachix hits are shared across the rehosting flakes.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/b6067cc0127d4db9c26c79e4de0513e58d0c40c9";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      pkgsFor = system: import nixpkgs { inherit system; };
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
          lib = pkgs.lib;

          version = lib.fileContents ./VERSION;

          # Build from the flake source (git-tracked tree only -- excludes the
          # gitignored build-system/ build-kvm/ pyvenv/ scratch dirs and .git).
          src = self;

          penguin-qemu = pkgs.callPackage ./nix/qemu.nix {
            inherit src version;
          };
        in
        {
          inherit penguin-qemu;
          # The release artifact (penguin-qemu.tar.gz).
          dist = penguin-qemu.dist;
          default = penguin-qemu;
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ pkgs.qemu ];
            packages = [
              pkgs.ninja
              pkgs.pkg-config
              (pkgs.python3.withPackages (ps: [
                ps.pyelftools
                ps.cffi
              ]))
            ];
          };
        }
      );
    };
}
