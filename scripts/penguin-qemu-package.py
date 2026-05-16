#!/usr/bin/env python3
#
# Package Penguin's QEMU shared libraries and generated CFFI declarations.

import argparse
import json
import tarfile
from pathlib import Path


def add_file(archive, path, arcname):
    if path.exists():
        archive.add(path, arcname=arcname)
        return True
    return False


def load_manifest(path):
    if not path.exists():
        return []
    return json.loads(path.read_text())["headers"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--system-build-dir", default="build-system")
    parser.add_argument("--kvm-build-dir", default="build-kvm")
    parser.add_argument("--output", default="penguin-qemu.tar.gz")
    args = parser.parse_args()

    system_build_dir = Path(args.system_build_dir)
    kvm_build_dir = Path(args.kvm_build_dir)
    output = Path(args.output)

    system_manifest = system_build_dir / "qemu_cffi_system_manifest.json"
    kvm_manifest = kvm_build_dir / "qemu_cffi_kvm_manifest.json"
    manifests = [
        ("system", system_build_dir, system_manifest),
        ("kvm", kvm_build_dir, kvm_manifest),
    ]

    entries = []
    with tarfile.open(output, "w:gz") as archive:
        for mode, build_dir, manifest_path in manifests:
            if add_file(
                archive,
                manifest_path,
                f"include/penguin-qemu-cffi/{manifest_path.name}",
            ):
                entries.append(f"include/penguin-qemu-cffi/{manifest_path.name}")

            for header in load_manifest(manifest_path):
                library = build_dir / header["library"]
                header_path = build_dir / header["header"]
                lib_arcname = f"lib/{header['library']}"
                header_arcname = f"include/penguin-qemu-cffi/{header['header']}"

                if not library.exists():
                    raise SystemExit(f"missing built library: {library}")
                if not header_path.exists():
                    raise SystemExit(f"missing generated header: {header_path}")

                archive.add(library, arcname=lib_arcname)
                archive.add(header_path, arcname=header_arcname)
                entries.extend([lib_arcname, header_arcname])

        metadata = {
            "schema": 1,
            "entries": sorted(entries),
        }
        metadata_path = output.parent / "penguin-qemu-package.json"
        metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
        archive.add(metadata_path, arcname="penguin-qemu-package.json")
        metadata_path.unlink()

    print(f"Wrote {output}")


if __name__ == "__main__":
    main()
