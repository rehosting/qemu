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

### Caveat: `is_io` is not enough to find unmodelled hardware

`qemu_plugin_hwaddr_is_io()` is unreliable for the case this sensor exists to
catch. Measured on `mips`/`malta`: a load from an unmapped physical address, and
even a load from the board's own serial port at `0x100003f8`, both report
`is_io == false` with a device name of `"RAM"`. Upstream's `contrib/plugins/
hwprofile.c` consequently reports *nothing at all* for those accesses.

So `iomin`/`iomax` let a caller who knows where RAM ends record everything above
it regardless of `is_io`. Accesses QEMU does not attribute to a named device are
bucketed by 64 KiB physical granule (`RAM@0x1c000000`) rather than collapsed
into one `"RAM"` entry, and each bucket records the physical range actually
touched plus the PC that first touched it — which is what identifies the
hardware to model or bypass.

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

`io=on` roughly doubles guest CPU and should be a deliberate second pass, not
the default for every boot in a search.

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
