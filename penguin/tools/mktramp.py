#!/usr/bin/env python3
"""Build a MIPS reset-vector trampoline to boot a raw kernel image.

QEMU's `-kernel` is not usable for a bring-your-own-kernel image on every
machine. On `malta` in particular the prom environment blob occupies low
physical memory, so an image linked to load at physical 0 is refused for
overlapping ROM regions before a single instruction runs.

Dropping `-kernel` entirely is simpler and closer to the truth. Pass this file
as `-bios` and place the kernel with `-device loader`:

    mktramp.py --entry 0x802d7eb0 --mask-i8259 > tramp.bin
    qemu-system-mips -M malta -cpu 34Kf -m 128 \\
        -bios tramp.bin \\
        -device loader,file=vmlinux.bin,addr=0x0,force-raw=on

`--mask-i8259` exists because of a failure mode that is easy to misdiagnose as
a guest bug. QEMU's i8259 comes out of reset with IMR = 0 -- everything
unmasked -- and the PIT free-runs, so the CPU's IP2 line is asserted forever.
A kernel written for an SoC that has never heard of an 8259 cannot mask it, so
the first `local_irq_enable()` livelocks in its own interrupt dispatcher. The
board contributes hardware the real machine does not have, and quiescing that
before entering the kernel is exactly a bootloader's job.

The PCI I/O window is at physical 0x10000000 out of reset and only moves to
0x18000000 once the malta bootloader stub programs the host bridge -- which we
are replacing -- so both kseg1 windows get the write. Whichever one is not
mapped is absorbed harmlessly.
"""
import argparse
import struct
import sys

T0, T1 = 8, 9


def lui(rt, imm):
    return 0x3C000000 | (rt << 16) | (imm & 0xFFFF)


def ori(rt, rs, imm):
    return 0x34000000 | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def sb(rt, off, base):
    return 0xA0000000 | (base << 21) | (rt << 16) | (off & 0xFFFF)


JR_T0 = 0x01000008
NOP = 0x00000000


def build(entry, mask_i8259, io_windows=(0xB000, 0xB800)):
    ins = []
    if mask_i8259:
        ins.append(ori(T1, 0, 0x00FF))
        for win in io_windows:
            ins += [lui(T0, win),
                    sb(T1, 0x0021, T0),     # master IMR
                    sb(T1, 0x00A1, T0)]     # slave IMR
    ins += [lui(T0, (entry >> 16) & 0xFFFF),
            ori(T0, T0, entry & 0xFFFF),
            JR_T0,
            NOP]                            # branch delay slot
    return b"".join(struct.pack(">I", i) for i in ins)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entry", required=True,
                    help="kernel entry point, e.g. kernel_entry (see kimage.py)")
    ap.add_argument("--mask-i8259", action="store_true",
                    help="mask both PICs before entry (see the module docstring)")
    ap.add_argument("--pad", default="0x1000", help="pad the image to this size")
    ap.add_argument("-o", "--output", help="output file (default: stdout)")
    args = ap.parse_args()

    blob = build(int(args.entry, 0), args.mask_i8259)
    blob = blob.ljust(int(args.pad, 0), b"\0")
    if args.output:
        with open(args.output, "wb") as f:
            f.write(blob)
    else:
        sys.stdout.buffer.write(blob)


if __name__ == "__main__":
    main()
