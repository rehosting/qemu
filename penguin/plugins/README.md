# Penguin TCG plugins

Fork-specific QEMU TCG plugins. Kept out of `contrib/plugins/` so rebasing onto
upstream QEMU never conflicts here.

Built only when the TCG (system) build is configured with `--enable-plugins`,
which `build.sh` does. They are packaged into `penguin-qemu.tar.gz` under
`lib/qemu-plugins/`.

```
ninja -C build-system penguin-plugins
```

## bootwatch

Answers **"how far did this kernel get?"** for a kernel QEMU cannot boot.

This exists for bring-your-own-kernel work: booting a vendor kernel built for a
board we do not model. Such a kernel dies in board setup, long before `init` —
so there is no console, no driver, and no userspace, and every in-guest signal
is unavailable. Everything `bootwatch` reports is observed from the emulator,
with no cooperation from the guest.

It emits a boot-progress tuple:

```
(rung, initcalls_entered, initcalls_returned, insns)
```

`rung` is the highest landmark reached along a caller-supplied ordered ladder of
kernel boot functions. Because it is the *highest ever* reached and not the most
recent, it is monotone: a jump backwards into an exception handler cannot lower
it. That is what makes two boots of the same kernel under different
configurations comparable, and therefore what lets a search loop hill-climb on
it.

Addresses are the caller's problem. For a stripped vendor `vmlinux` they come
from decoding the kernel's own kallsyms tables.

### Options

| option | meaning |
|---|---|
| `rung=<addr>[:<name>]` | Add a ladder landmark. **Repeatable; ladder order is argv order, not address order.** `name` defaults to the address. |
| `initcall=<addr>` | Address of `do_one_initcall`. Enables the initcall counters. |
| `insns=on\|off` | Count executed instructions. On by default. Costs ~18% of guest CPU (see below); turn it off if you only need the rung. |
| `io=on\|off` | Log memory accesses per region. Off by default — the callback fires on every access, and roughly doubles guest CPU. |
| `iomin=<paddr>`, `iomax=<paddr>` | Additionally record accesses in this physical window, not just ones QEMU calls IO. Requires `io=on`. See the caveat below. |
| `out=<path>` | Write the JSON report here. Defaults to QEMU's plugin log. |

At least one `rung=` or `initcall=` is required.

```
-plugin /path/libbootwatch.so,rung=0x80100000:start_kernel,\
rung=0x80101234:do_initcalls,initcall=0x80105678,io=on,iomin=0x10000000,out=bw.json
```

### Why `initcalls_returned` is inferred

`do_one_initcall` is a single call site, so one watch yields every initcall.
Returns are *inferred* rather than observed: the callee's return address is not
statically known, and on several targets the register API cannot be used to
read the link register (below), so re-entry is taken to imply that the previous
call returned.

The inference is exact except for the final call — which is the interesting
one. At exit, `entered - returned == 1` with `in_flight: true` means the kernel
is still inside the last initcall, i.e. it **hung** there rather than
panicking. `site` reports which.

### Why the memory sweep, and not an exception handler

The obvious cheap design is to catch the *fault*: accessing hardware that is not
there should trap, and traps are rare, so an exception callback
(`qemu_plugin_register_vcpu_discon_cb` with `QEMU_PLUGIN_DISCON_EXCEPTION`)
would cost nothing. On `mips`/`malta` that design detects **nothing, ever**,
because there is no fault. Verified three ways:

- The instruction trace runs straight through. A load from unmapped
  `0x1c000000` and a store to `0x1c000004` are followed by the next
  instruction in program order, with no vector to an exception handler.
- `-d int,guest_errors` over the same run logs **zero** lines.
- `MIPSCPUClass::no_data_aborts` is set only by `hw/mips/jazz.c`, and malta
  does not set `ignore_memory_transaction_failures`. Had the access reached
  `cpu_transaction_failed()`, malta *would* have raised `EXCP_DBE`. It did not,
  so the access never registered as a failed transaction at all.

The reason is that malta covers most of its unused physical space with the
`empty_slot` device, which *accepts* the access and returns zero. So the guest
reads garbage and carries on. That is the silent-failure mode this sensor exists
to catch, and instrumenting every access is the only way to see it — hence
`io=on` being worth its cost rather than a luxury.

What the cheaper configuration still sees is the *downstream symptom*, and only
sometimes: a driver that polls a register that is not there spins, which shows
up as instructions climbing while the rung stays put. If instead the driver
reads garbage and proceeds, nothing in the cheap configuration notices.

The upside of `empty_slot` being a real device is that the access is
*attributable*: with the `TLB_MMIO` fix (below) it is reported against a region
named `empty-slot`, which is a self-describing "the guest touched hardware this
machine does not model" signal.

### `is_io` was broken, and this series fixes it

`qemu_plugin_hwaddr_is_io()` could never return true. `tlb_plugin_lookup()`
tested `TLB_MMIO` against `CPUTLBEntry.addr_idx[]`, but `tlb-flags.h` puts
`TLB_MMIO` in `CPUTLBEntryFull.slow_flags[]` and leaves only `TLB_FORCE_SLOW`
in the address word. So every access was reported as non-IO with a device name
of `"RAM"`, and upstream's `contrib/plugins/hwprofile.c` — a plugin whose sole
purpose is per-device IO profiling — printed nothing but its header on every
target.

Fixed in this series (`accel/tcg: fix qemu_plugin_hwaddr_is_io() never
reporting IO`). On a full Linux boot on malta, `hwprofile` goes from an empty
table to 16 regions and ~1.8M accesses, and bootwatch attributes the probe's
unmapped access to `empty-slot`.

`iomin`/`iomax` remain useful as a second, address-based filter — they need no
cooperation from the memory-region layer, and they catch traffic to a physical
window you care about whether or not QEMU attributes it to a device. Accesses
QEMU cannot attribute (a region with no `name`, reported as `anon<ptr>` or
`RAM`) are bucketed by 64 KiB physical granule rather than collapsed into one
entry, and each bucket records the physical range touched plus the PC that
first touched it.

Note that many QEMU devices never set a `MemoryRegion` name, so even with the
fix a good fraction of regions report as `anon<ptr>`. The physical range is the
reliable identifier.

### Caveat: no register access on some targets

`qemu_plugin_get_registers()` is driven by the target's GDB register XML.
Targets that set only `gdb_num_core_regs` and no `gdb_core_xml_file` expose
**no named registers**, so the plugin register read/write API is unavailable
there. As of this writing that includes **mips**, hppa, sh4, tricore and
xtensa. `bootwatch` therefore uses no register access at all, and anything that
needs it (reading a link register, forcing a return value) has to find another
mechanism on those targets.

### Cost

Measured on a full `vmlinux.mipseb` (6.13) boot to panic on `-M malta`, median
guest CPU time over 8–15 alternating runs, pinned to one core:

| configuration | CPU | vs. no plugin |
|---|---|---|
| QEMU built `--disable-plugins` | 5.240 s | — |
| built `--enable-plugins`, **no plugin loaded** | 5.240 s | **+0.00%** |
| bootwatch, 12 rungs + initcall, `insns=off` | 5.315 s | +2% |
| bootwatch, 12 rungs + initcall (default) | 6.295 s | +20% |
| bootwatch + `io=on` | 13.400 s | +156% |

So **enabling the plugin API costs nothing when no plugin is loaded** — the
difference is below the ~0.2% resolution of the measurement. Binary size grows
0.7% (58.7 → 59.1 MB).

Almost all of the default-configuration cost is the instruction counter, which
is the one thing touching every instruction: `insns=off` recovers 18 of those 20
points. Keep it on when you need to tell "hung in a poll loop" (insns climbing,
rung static) from "wedged" (neither moving); turn it off for a search loop that
only compares rungs.

`io=on` roughly doubles guest CPU. Pay it anyway when you are looking for
unmodelled hardware — see below, there is no cheaper signal.

Instruction counting uses the inline scoreboard, not a callback. Rung and
initcall watches are ordinary callbacks, so put rungs on **one-shot landmarks,
not hot addresses** — a rung on a spin loop was observed taking ~390M callbacks
in ten seconds.

### Output

```json
{
  "schema": 1,
  "target": "mips",
  "insns": 4719734849,
  "rung": {
    "index": 1,
    "name": "probe_fn",
    "ladder": [
      {"index": 0, "name": "_start", "addr": "0x80100000", "hits": 1, "first_insn": 62},
      {"index": 1, "name": "probe_fn", "addr": "0x80100038", "hits": 3, "first_insn": 70}
    ]
  },
  "initcalls": {"watched": true, "entered": 3, "returned": 2,
                "in_flight": true, "site": "0x80100038"},
  "io": {"watched": true, "regions": [
    {"device": "RAM@0x1c000000", "is_io": false, "reads": 1, "writes": 1,
     "first_pc": "0x8010000c", "paddr_lo": "0x1c000000", "paddr_hi": "0x1c000004"}
  ]}
}
```

`rung.index` is 0 and `rung.name` is `null` when no landmark was ever reached —
the honest state for a kernel that never started executing, e.g. one loaded at
the wrong address.

Counters are global rather than per-vCPU. Penguin runs `core.smp=1` and the
boot path in question is pre-SMP.
