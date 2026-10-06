# Penguin QEMU tools

Host-side helpers for bring-your-own-kernel work. They pair with the
`bootwatch` plugin in `../plugins/`, which needs kernel virtual addresses that
a stripped vendor image does not hand you.

Pure stdlib Python 3, no build step.

## kimage.py — load base and symbols from a headerless image

```
kimage.py <image> [--little] [--syms out.txt] [--ladder sym,sym,...]
```

Takes a raw kernel blob — no ELF, no headers, stripped — and recovers the
address it was linked at plus its whole symbol table. `--ladder` prints a
ready-made `bootwatch` argument string.

It assumes the image is **flat**: file offset plus base equals virtual address,
which is what an `objcopy -O binary` payload out of a firmware image looks
like. Hand it an ELF and it will produce a confident, wrong answer, because an
ELF's file offsets are not a single linear map — use `nm` on those. A
self-decompressing image has to be unpacked first; the tables live in the
payload, not the stub.

The order is the interesting part, and getting it wrong costs a lot of time:

1. **`__ksymtab` fixes the load base.** Each entry is
   `{unsigned long value; const char *name;}` and the *name* is a string that
   can also be found by content, so `base = name_ptr − file_offset_of_string`
   with no heuristics. Two adjacent entries (name pointers exactly 8 bytes
   apart) confirm it.
2. **`kallsyms` then becomes a lookup, not a search.** The name stream can be
   walked without the token table (each record has an explicit length byte), so
   the layout is pinned down before anything is decoded, and `kallsyms_markers`
   checks the walk at every 256th symbol. Finally the address array's offset is
   *elected* by voting over the ksymtab pairs.

Anchoring on kallsyms first is the tempting mistake. Its sub-tables are
separated by linker alignment padding whose size the format does not imply, so
deriving one table's offset from another's is off by a few entries — which
silently shifts every name↔address pairing and makes every cross-check
disagree by a handful of bytes for no visible reason.

The output is self-checking: the tool reports how many `__ksymtab` pairs the
recovered kallsyms table reproduces. Anything short of near-unanimous means the
result is wrong, not approximate.

`KERNEL_LO`/`KERNEL_HI` at the top bound what counts as a kernel address; they
are set for MIPS kseg0 and need adjusting per target.

## mktramp.py — reset-vector trampoline for a raw image

```
mktramp.py --entry 0x802d7eb0 [--mask-i8259] -o tramp.bin
```

`-kernel` is not usable for every BYOK image: on `malta` the prom environment
blob occupies low physical memory, so an image linked to load at physical 0 is
refused for overlapping ROMs before a single instruction runs. Dropping
`-kernel` is simpler and closer to the truth — pass the trampoline as `-bios`
and place the kernel with `-device loader`:

```
qemu-system-mips -M malta -cpu 34Kf -m 128 \
    -bios tramp.bin \
    -device loader,file=vmlinux.bin,addr=0x0,force-raw=on
```

`--mask-i8259` deserves its own paragraph, because the failure it prevents
looks like a guest bug and is not. QEMU's i8259 resets with `IMR = 0` and the
PIT free-runs, so IP2 is asserted forever; a kernel written for an SoC that has
never heard of an 8259 cannot mask it, and its first `local_irq_enable()`
livelocks in its own interrupt dispatcher. The machine model contributes
hardware the real board does not have. Quiescing that before entering the
kernel is exactly what a bootloader is for, which is why it belongs here rather
than in a patch to the guest.

## klog.py — printk ring buffer out of a running guest

```
klog.py --qmp /tmp/q.sock --log-buf 0x80e154b0 [--base 0x80000000] [--little]
```

A kernel booted on a machine that is not its own has no console — its UART is
at an address the board does not have, so its output goes wherever unassigned
accesses go. `__log_buf` is ordinary RAM, though, so the log can be read from
outside with no cooperation from the guest, no console and no working driver.
Get `__log_buf` from `kimage.py`.

This is the counterpart to `bootwatch`, not a replacement. The ladder is a
monotone scalar you can hill-climb on; the log is what tells you *which lever
to pull*. On a real bring-up the log was the difference between "stopped
climbing" and the guest's own one-line explanation of what it failed to
find, and it costs nothing at runtime.

Reads `struct printk_log`, i.e. Linux 3.5 through 5.9. Newer kernels use the
`printk_ringbuffer` and need a different decoder.
