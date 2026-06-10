#!/usr/bin/env python3
#
# Generate CFFI declarations for CPUArchState from the DWARF of a built
# Penguin QEMU library.
#
# The emitted header gives Penguin's compatibility layer typed access to
# the full per-target CPU state (env) -- coprocessor registers, timers,
# FPU state -- beyond the GDB core register set. Generating from the
# DWARF of the exact library being packaged means the layout can never
# drift from the binary.
#
# Every emitted struct is verified field-by-field against DWARF offsets
# using cffi itself. Members that cannot be represented (bitfields,
# unsupported types) are dropped and the resulting holes are filled with
# explicit padding, so offsets and sizes are always exact; in the worst
# case a struct degrades to an opaque byte blob of the right size.

import argparse
import json
import re
import sys
from pathlib import Path

import cffi
from elftools.elf.elffile import ELFFile

MAX_REPAIR_PASSES = 200

C_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

BASE_TYPES = {
    ("boolean", 1): "uint8_t",  # avoid _Bool: cdef'd alongside headers that typedef bool
    ("signed", 1): "int8_t",
    ("signed", 2): "int16_t",
    ("signed", 4): "int32_t",
    ("signed", 8): "int64_t",
    ("signed char", 1): "int8_t",
    ("unsigned", 1): "uint8_t",
    ("unsigned", 2): "uint16_t",
    ("unsigned", 4): "uint32_t",
    ("unsigned", 8): "uint64_t",
    ("unsigned char", 1): "uint8_t",
    ("float", 4): "float",
    ("float", 8): "double",
    ("UTF", 1): "uint8_t",
    ("UTF", 2): "uint16_t",
    ("UTF", 4): "uint32_t",
}

ENCODING_NAMES = {
    0x01: "address",
    0x02: "boolean",
    0x04: "float",
    0x05: "signed",
    0x06: "signed char",
    0x07: "unsigned",
    0x08: "unsigned char",
    0x10: "UTF",
}


def log(msg):
    print(f"penguin-env-cffi-gen: {msg}", file=sys.stderr)


class Member:
    def __init__(self, name, ctype, offset, suffix=""):
        self.name = name
        self.ctype = ctype  # C type string, e.g. "uint32_t" or "struct foo"
        self.suffix = suffix  # array suffix, e.g. "[32]"
        self.offset = offset  # DWARF byte offset within parent


class StructDef:
    def __init__(self, tag, kind, size):
        self.tag = tag  # C tag name
        self.kind = kind  # "struct" or "union"
        self.size = size  # DWARF byte size
        self.members = []  # emitted Members (skipped ones omitted)
        self.pads = {}  # insert-before-index -> pad byte count
        self.trailing_pad = 0
        self.opaque = False

    def render(self):
        lines = [f"{self.kind} {self.tag} {{"]
        if self.opaque:
            lines.append(f"    uint8_t _penguin_opaque[{max(self.size, 1)}];")
        else:
            for idx, member in enumerate(self.members):
                pad = self.pads.get(idx, 0)
                if pad:
                    lines.append(f"    uint8_t _penguin_pad{idx}[{pad}];")
                lines.append(f"    {member.ctype} {member.name}{member.suffix};")
            if self.trailing_pad:
                lines.append(
                    f"    uint8_t _penguin_pad_tail[{self.trailing_pad}];")
            if not self.members and not self.trailing_pad:
                lines.append(f"    uint8_t _penguin_empty[{max(self.size, 1)}];")
        lines.append("};")
        return "\n".join(lines)


class EnvTypeExtractor:
    def __init__(self, dwarf):
        self.dwarf = dwarf
        self.structs = {}  # tag -> StructDef
        self.order = []  # emission order (dependencies first)
        self.die_tags = {}  # die offset -> tag
        self.anon_count = 0
        self.warnings = []

    # ---- DWARF navigation helpers ----

    def _attr(self, die, name):
        attr = die.attributes.get(name)
        return attr.value if attr is not None else None

    def _type_die(self, die):
        if "DW_AT_type" not in die.attributes:
            return None
        return die.get_DIE_from_attribute("DW_AT_type")

    def _strip_cv(self, die):
        while die is not None and die.tag in (
            "DW_TAG_const_type",
            "DW_TAG_volatile_type",
            "DW_TAG_restrict_type",
            "DW_TAG_atomic_type",
        ):
            die = self._type_die(die)
        return die

    def _resolve_typedefs(self, die):
        die = self._strip_cv(die)
        while die is not None and die.tag == "DW_TAG_typedef":
            die = self._strip_cv(self._type_die(die))
        return die

    def _die_name(self, die):
        name = self._attr(die, "DW_AT_name")
        if isinstance(name, bytes):
            name = name.decode("utf-8", "replace")
        return name

    def _member_offset(self, die):
        value = self._attr(die, "DW_AT_data_member_location")
        if value is None:
            return 0
        if isinstance(value, int):
            return value
        # exprloc form: DW_OP_plus_uconst <ULEB128>
        data = bytes(value)
        if data and data[0] == 0x23:
            result = 0
            shift = 0
            for byte in data[1:]:
                result |= (byte & 0x7F) << shift
                if not byte & 0x80:
                    break
                shift += 7
            return result
        raise ValueError(f"unsupported member location encoding: {value!r}")

    # ---- type resolution ----

    def _base_ctype(self, die):
        size = self._attr(die, "DW_AT_byte_size")
        encoding = self._attr(die, "DW_AT_encoding")
        key = (ENCODING_NAMES.get(encoding), size)
        return BASE_TYPES.get(key)

    def _array_dims(self, die):
        dims = []
        for child in die.iter_children():
            if child.tag != "DW_TAG_subrange_type":
                continue
            count = self._attr(child, "DW_AT_count")
            if count is None:
                upper = self._attr(child, "DW_AT_upper_bound")
                count = upper + 1 if isinstance(upper, int) else 0
            if not isinstance(count, int):
                count = 0
            dims.append(count)
        return dims or [0]

    def member_ctype(self, type_die):
        """
        Resolve a member's type to (ctype, array_suffix) or None when it
        cannot be represented (the member is then dropped and padded over).
        """
        die = self._resolve_typedefs(type_die)
        if die is None:
            return None

        if die.tag == "DW_TAG_pointer_type":
            return ("void *", "")

        if die.tag == "DW_TAG_base_type":
            ctype = self._base_ctype(die)
            return (ctype, "") if ctype else None

        if die.tag == "DW_TAG_enumeration_type":
            size = self._attr(die, "DW_AT_byte_size") or 4
            ctype = BASE_TYPES.get(("unsigned", size))
            return (ctype, "") if ctype else None

        if die.tag == "DW_TAG_array_type":
            element = self.member_ctype(self._type_die(die))
            if element is None:
                return None
            ctype, suffix = element
            dims = self._array_dims(die)
            if any(d <= 0 for d in dims):
                return None
            return (ctype, "".join(f"[{d}]" for d in dims) + suffix)

        if die.tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
            tag = self.emit_struct(die)
            if tag is None:
                return None
            kind = "struct" if die.tag == "DW_TAG_structure_type" else "union"
            return (f"{kind} {tag}", "")

        return None

    # ---- struct emission ----

    def emit_struct(self, die):
        """Emit a struct/union definition; returns its tag or None."""
        if die.offset in self.die_tags:
            return self.die_tags[die.offset]
        if self._attr(die, "DW_AT_declaration"):
            return None
        size = self._attr(die, "DW_AT_byte_size")
        if not size:
            return None

        name = self._die_name(die)
        if name and C_IDENT_RE.match(name):
            tag = f"penguin_env_{name}"
        else:
            self.anon_count += 1
            tag = f"penguin_env_anon{self.anon_count}"
        # Disambiguate distinct DIEs that share a source-level name.
        base_tag = tag
        n = 1
        while tag in self.structs:
            n += 1
            tag = f"{base_tag}_{n}"

        kind = "struct" if die.tag == "DW_TAG_structure_type" else "union"
        sdef = StructDef(tag, kind, size)
        self.die_tags[die.offset] = tag
        self.structs[tag] = sdef

        for child in die.iter_children():
            if child.tag != "DW_TAG_member":
                continue
            mname = self._die_name(child)
            if mname is None or not C_IDENT_RE.match(mname):
                self.warnings.append(
                    f"{tag}: anonymous member dropped (padded)")
                continue
            if "DW_AT_bit_size" in child.attributes:
                self.warnings.append(
                    f"{tag}.{mname}: bitfield dropped (padded)")
                continue
            resolved = self.member_ctype(self._type_die(child))
            if resolved is None:
                self.warnings.append(
                    f"{tag}.{mname}: unrepresentable type dropped (padded)")
                continue
            ctype, suffix = resolved
            offset = self._member_offset(child)
            sdef.members.append(Member(mname, ctype, offset, suffix))

        if kind == "union":
            # Pin union size regardless of which members were dropped.
            sdef.members = [m for m in sdef.members if m.offset == 0]
            sdef.trailing_pad = 0
            sdef.members.append(Member("_penguin_union_pad", "uint8_t",
                                       0, f"[{size}]"))

        self.order.append(tag)
        return tag

    # ---- rendering / verification ----

    def render_all(self):
        return "\n\n".join(self.structs[tag].render() for tag in self.order)

    def _verify_once(self, ffi):
        """Return the first mismatch found, or None when layout is exact."""
        for tag in self.order:
            sdef = self.structs[tag]
            cname = f"{sdef.kind} {sdef.tag}"
            if sdef.opaque or sdef.kind == "union":
                actual = ffi.sizeof(cname)
                if actual != sdef.size:
                    return (sdef, "size", actual)
                continue
            for idx, member in enumerate(sdef.members):
                if not member.name:
                    continue
                actual = ffi.offsetof(cname, member.name)
                if actual != member.offset:
                    return (sdef, idx, actual)
            actual = ffi.sizeof(cname)
            if actual != sdef.size:
                return (sdef, "size", actual)
        return None

    def verify_and_repair(self):
        for _ in range(MAX_REPAIR_PASSES):
            ffi = cffi.FFI()
            try:
                ffi.cdef(self.render_all())
            except Exception as exc:  # cdef parse error: cannot repair
                raise SystemExit(f"generated cdef failed to parse: {exc}")
            mismatch = self._verify_once(ffi)
            if mismatch is None:
                return
            sdef, where, actual = mismatch
            if where == "size":
                if actual < sdef.size and not sdef.opaque:
                    sdef.trailing_pad += sdef.size - actual
                    continue
                self.warnings.append(
                    f"{sdef.tag}: size mismatch ({actual} != {sdef.size}); "
                    "made opaque")
                sdef.opaque = True
                continue
            member = sdef.members[where]
            if actual < member.offset:
                sdef.pads[where] = sdef.pads.get(where, 0) + (
                    member.offset - actual)
                continue
            self.warnings.append(
                f"{sdef.tag}.{member.name}: offset mismatch "
                f"({actual} > {member.offset}); struct made opaque")
            sdef.opaque = True
        raise SystemExit("layout repair did not converge")


def find_root_dies(dwarf):
    """Locate the CPUArchState typedef target and struct CPUState size."""
    env_die = None
    cpu_state_size = None
    cus = list(dwarf.iter_CUs())

    def cu_name(cu):
        die = cu.get_top_DIE()
        name = die.attributes.get("DW_AT_name")
        if name is None:
            return ""
        value = name.value
        return value.decode("utf-8", "replace") if isinstance(value, bytes) else value

    # Target CUs define CPUArchState; check them first to finish fast.
    for cu in sorted(cus, key=lambda c: ("/target/" not in cu_name(c))):
        top = cu.get_top_DIE()
        for die in top.iter_children():
            if (env_die is None and die.tag == "DW_TAG_typedef"
                    and die.attributes.get("DW_AT_name") is not None
                    and die.attributes["DW_AT_name"].value == b"CPUArchState"):
                target = die.get_DIE_from_attribute("DW_AT_type")
                if target is not None and "DW_AT_byte_size" in target.attributes:
                    env_die = target
            if (cpu_state_size is None
                    and die.tag == "DW_TAG_structure_type"
                    and die.attributes.get("DW_AT_name") is not None
                    and die.attributes["DW_AT_name"].value == b"CPUState"
                    and "DW_AT_byte_size" in die.attributes):
                cpu_state_size = die.attributes["DW_AT_byte_size"].value
        if env_die is not None and cpu_state_size is not None:
            break
    return env_die, cpu_state_size


def generate(library_path):
    with open(library_path, "rb") as handle:
        elf = ELFFile(handle)
        if not elf.has_dwarf_info():
            raise SystemExit(f"{library_path}: no DWARF info (stripped?)")
        dwarf = elf.get_dwarf_info()
        env_die, cpu_state_size = find_root_dies(dwarf)
        if env_die is None:
            raise SystemExit(f"{library_path}: CPUArchState typedef not found")
        if cpu_state_size is None:
            raise SystemExit(f"{library_path}: struct CPUState not found")

        extractor = EnvTypeExtractor(dwarf)
        root_tag = extractor.emit_struct(env_die)
        if root_tag is None:
            raise SystemExit(f"{library_path}: CPUArchState unresolvable")
        extractor.verify_and_repair()

    for warning in extractor.warnings:
        log(f"note: {warning}")

    body = extractor.render_all()
    root = extractor.structs[root_tag]
    return "\n".join([
        "/*",
        " * Generated by scripts/penguin-env-cffi-gen.py from "
        f"{Path(library_path).name}.",
        " *",
        " * Layout-exact CFFI declarations for this target's CPUArchState,",
        " * verified field-by-field against the library's DWARF. Members",
        " * that cannot be represented (bitfields, exotic types) are",
        " * replaced by explicit padding. All pointers are void *.",
        " *",
        " * Obtain the env pointer with penguin_cpu_env(cpu) and call",
        " * penguin_sync_cpu_state(cpu) first when running under KVM.",
        " */",
        "",
        f"#define PENGUIN_CPU_STATE_SIZE {cpu_state_size}",
        "",
        body,
        "",
        f"typedef {root.kind} {root.tag} CPUArchState;",
        "",
    ])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--manifest", required=True,
                        help="cffi manifest written by penguin-cffi-gen.py; "
                             "entries gain an env_header key")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    manifest_path = Path(args.manifest)
    manifest = json.loads(manifest_path.read_text())

    generated = {}
    for entry in manifest["headers"]:
        library = build_dir / entry["library"]
        env_header = entry["header"].replace(".h", "_env.h")
        target = entry.get("qemu_target", library.name)
        if target not in generated:
            log(f"generating {env_header} from {library.name}")
            generated[target] = generate(library)
        (build_dir / env_header).write_text(generated[target])
        entry["env_header"] = env_header

    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    log(f"updated {manifest_path} with {len(manifest['headers'])} env headers")


if __name__ == "__main__":
    main()
