/*
 * bootwatch - boot-progress observer for Penguin's bring-your-own-kernel work
 *
 * Answers "how far did this kernel get?" for a kernel QEMU cannot boot,
 * without any cooperation from the guest. That last part is the whole point:
 * when a vendor kernel dies in board setup there is no console, no driver and
 * no userspace, so every in-guest signal is unavailable. Everything here is
 * observed from the emulator.
 *
 * It reports a boot-progress tuple:
 *
 *   (rung, initcalls_entered, initcalls_returned, insns)
 *
 * "rung" is the highest landmark reached along an ordered ladder of kernel
 * boot functions (start_kernel, setup_arch, ... run_init_process) supplied by
 * the caller, who resolves the addresses from the kernel's own kallsyms. The
 * ladder is monotone by construction, so two boots of the same kernel under
 * different configurations are directly comparable -- which is what lets a
 * search loop hill-climb on it.
 *
 * The initcall counters come from a single watch on do_one_initcall.
 * entered > returned means the kernel is stuck *inside* an initcall, which
 * distinguishes a hang in a probe from a panic in one; the address of the
 * offending initcall is reported so the caller can name it.
 *
 * Optionally it also logs IO accesses per memory region, which is how
 * accesses to unmapped MMIO (hardware this machine does not model) are found.
 *
 * Copyright (c) 2026 Massachusetts Institute of Technology
 *
 * License: GNU GPL, version 2 or later.
 *   See the COPYING file in the top-level directory.
 */

#include <glib.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define BOOTWATCH_SCHEMA 1

/* Bucket size for grouping accesses that QEMU does not attribute to a device. */
#define IO_GRANULE_MASK 0xffffULL

/*
 * A landmark on the boot ladder. "index" is the caller's ordering, and the
 * run's rung is the highest index ever reached -- never the most recent one,
 * so a jump backwards (an exception, a retry loop) cannot lower it.
 */
typedef struct {
    uint64_t addr;
    char *name;
    unsigned index;
    uint64_t hits;
    uint64_t first_insn;    /* insn count at first arrival, for ordering */
} Rung;

/*
 * Per-region tally, keyed by the region name QEMU reports. Unmapped physical
 * space has no name of its own -- it comes back as "RAM" -- so the physical
 * range actually touched is recorded too, which is what identifies the
 * hardware the caller has to model or bypass.
 */
typedef struct {
    char *device;
    bool is_io;
    uint64_t reads;
    uint64_t writes;
    uint64_t first_pc;
    uint64_t paddr_lo;
    uint64_t paddr_hi;
} IoRegion;

static GHashTable *rungs_by_addr;   /* uint64_t* -> Rung*   */
static GHashTable *io_by_device;    /* char*     -> IoRegion* */

static uint64_t initcall_addr;
static bool watch_initcalls;
static bool watch_io;
/*
 * Counting instructions costs ~23% of guest CPU time even via the inline
 * scoreboard, because it is the one thing that touches every instruction.
 * It is worth that by default -- it is what separates "hung in a poll loop"
 * (insns climbing, rung static) from "wedged" (neither moving) -- but a
 * search loop that only needs the rung can turn it off.
 */
static bool count_insns = true;
/*
 * Physical window of interest, in addition to anything QEMU calls IO.
 *
 * qemu_plugin_hwaddr_is_io() is not sufficient on its own for finding accesses
 * to hardware the machine does not model, which is the case we care about: on
 * mips/malta a load from an unmapped physical address reports is_io == false
 * and a device name of "RAM" (upstream's hwprofile.c reports nothing at all
 * for such an access, for the same reason). Callers who know where RAM ends
 * can therefore set iomin to catch them.
 */
static uint64_t io_min;
static uint64_t io_max = UINT64_MAX;
static bool io_window_set;
static char *out_path;
static const char *target_name = "unknown";

/*
 * Instruction count uses the inline scoreboard rather than a callback: this
 * fires on every instruction, so a callback (let alone a locked one) would
 * dominate the run. The rest of the state is touched rarely -- a few hundred
 * times per boot -- and is guarded by a plain mutex.
 */
static struct qemu_plugin_scoreboard *insn_sb;
static qemu_plugin_u64 insn_count_u64;

static GMutex state_lock;
static unsigned max_rung_index;
static const char *max_rung_name = NULL;
static uint64_t initcalls_entered;
static uint64_t initcalls_returned;
static bool inside_initcall;
static uint64_t last_initcall_pc;   /* return addr of the in-flight initcall */

static void rung_free(void *p)
{
    Rung *r = p;
    g_free(r->name);
    g_free(r);
}

static void io_region_free(void *p)
{
    IoRegion *io = p;
    g_free(io->device);
    g_free(io);
}

/* ---------------------------------------------------------------- callbacks */

static void vcpu_rung_hit(unsigned int cpu_index, void *udata)
{
    Rung *r = udata;
    uint64_t now = qemu_plugin_u64_sum(insn_count_u64);

    g_mutex_lock(&state_lock);
    if (r->hits++ == 0) {
        r->first_insn = now;
    }
    if (r->index > max_rung_index || max_rung_name == NULL) {
        max_rung_index = r->index;
        max_rung_name = r->name;
    }
    g_mutex_unlock(&state_lock);
}

/*
 * do_one_initcall entry.
 *
 * Returns are inferred rather than observed: the callee's return address is
 * not statically known, and on several targets (see README) the register API
 * is unavailable, so we cannot read the link register to find it. Re-entry
 * therefore implies the previous call returned. The inference is exact except
 * for the final call, which is precisely the interesting case: at exit,
 * entered - returned == 1 means the kernel is still inside the last initcall,
 * i.e. it hung there rather than panicking.
 */
static void vcpu_initcall(unsigned int cpu_index, void *udata)
{
    uint64_t pc = (uint64_t)(uintptr_t)udata;

    g_mutex_lock(&state_lock);
    if (inside_initcall) {
        initcalls_returned++;
    }
    inside_initcall = true;
    initcalls_entered++;
    last_initcall_pc = pc;
    g_mutex_unlock(&state_lock);
}

static void vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    struct qemu_plugin_hwaddr *hw = qemu_plugin_get_hwaddr(info, vaddr);

    if (!hw) {
        return;
    }

    /*
     * Resolve everything about the handle before returning: it is only valid
     * for the duration of this callback.
     */
    bool is_io = qemu_plugin_hwaddr_is_io(hw);
    uint64_t paddr = qemu_plugin_hwaddr_phys_addr(hw);

    if (!is_io &&
        !(io_window_set && paddr >= io_min && paddr <= io_max)) {
        return;
    }

    const char *device = qemu_plugin_hwaddr_device_name(hw);
    bool is_store = qemu_plugin_mem_is_store(info);
    uint64_t pc = (uint64_t)(uintptr_t)udata;

    /*
     * Named devices are their own bucket. Everything else shares whatever
     * name the address space carries ("RAM"), which would collapse unrelated
     * regions together, so bucket those by physical granule instead.
     */
    g_autofree char *key = is_io
        ? g_strdup(device)
        : g_strdup_printf("%s@0x%" PRIx64, device,
                          (uint64_t)(paddr & ~IO_GRANULE_MASK));

    g_mutex_lock(&state_lock);
    IoRegion *io = g_hash_table_lookup(io_by_device, key);
    if (!io) {
        io = g_new0(IoRegion, 1);
        io->device = g_strdup(key);
        io->is_io = is_io;
        io->first_pc = pc;
        io->paddr_lo = io->paddr_hi = paddr;
        g_hash_table_insert(io_by_device, io->device, io);
    }
    if (paddr < io->paddr_lo) {
        io->paddr_lo = paddr;
    }
    if (paddr > io->paddr_hi) {
        io->paddr_hi = paddr;
    }
    if (is_store) {
        io->writes++;
    } else {
        io->reads++;
    }
    g_mutex_unlock(&state_lock);
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t vaddr = qemu_plugin_insn_vaddr(insn);

        if (count_insns) {
            qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, insn_count_u64, 1);
        }

        Rung *r = g_hash_table_lookup(rungs_by_addr, &vaddr);
        if (r) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_rung_hit,
                                                   QEMU_PLUGIN_CB_NO_REGS, r);
        }

        if (watch_initcalls && vaddr == initcall_addr) {
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, vcpu_initcall, QEMU_PLUGIN_CB_NO_REGS,
                (void *)(uintptr_t)vaddr);
        }

        if (watch_io) {
            qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem,
                                             QEMU_PLUGIN_CB_NO_REGS,
                                             QEMU_PLUGIN_MEM_RW,
                                             (void *)(uintptr_t)vaddr);
        }
    }
}

/* ------------------------------------------------------------------- report */

static gint rung_by_index(gconstpointer a, gconstpointer b)
{
    const Rung *ra = *(const Rung **)a;
    const Rung *rb = *(const Rung **)b;
    return (ra->index > rb->index) - (ra->index < rb->index);
}

static void append_json(GString *s, uint64_t insn_count)
{
    g_string_append_printf(s, "{\n");
    g_string_append_printf(s, "  \"schema\": %d,\n", BOOTWATCH_SCHEMA);
    g_string_append_printf(s, "  \"target\": \"%s\",\n", target_name);
    if (count_insns) {
        g_string_append_printf(s, "  \"insns\": %" PRIu64 ",\n", insn_count);
    } else {
        /* Not counted: say so rather than reporting a zero that reads as
         * "nothing executed", which is a real and different outcome. */
        g_string_append_printf(s, "  \"insns\": null,\n");
    }

    g_string_append_printf(s, "  \"rung\": {\n");
    g_string_append_printf(s, "    \"index\": %u,\n",
                           max_rung_name ? max_rung_index : 0);
    if (max_rung_name) {
        g_string_append_printf(s, "    \"name\": \"%s\",\n", max_rung_name);
    } else {
        g_string_append_printf(s, "    \"name\": null,\n");
    }
    g_string_append_printf(s, "    \"ladder\": [\n");

    g_autoptr(GPtrArray) ladder = g_ptr_array_new();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, rungs_by_addr);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        g_ptr_array_add(ladder, v);
    }
    g_ptr_array_sort(ladder, rung_by_index);

    for (guint i = 0; i < ladder->len; i++) {
        Rung *r = g_ptr_array_index(ladder, i);
        g_autofree char *first = (r->hits && count_insns)
            ? g_strdup_printf("%" PRIu64, r->first_insn)
            : g_strdup("null");
        g_string_append_printf(
            s,
            "      {\"index\": %u, \"name\": \"%s\", \"addr\": \"0x%" PRIx64
            "\", \"hits\": %" PRIu64 ", \"first_insn\": %s}%s\n",
            r->index, r->name, r->addr, r->hits, first,
            i + 1 < ladder->len ? "," : "");
    }
    g_string_append_printf(s, "    ]\n");
    g_string_append_printf(s, "  },\n");

    g_string_append_printf(s, "  \"initcalls\": ");
    if (watch_initcalls) {
        g_string_append_printf(
            s,
            "{\"watched\": true, \"entered\": %" PRIu64 ", \"returned\": %"
            PRIu64 ", \"in_flight\": %s, \"site\": \"0x%" PRIx64 "\"},\n",
            initcalls_entered, initcalls_returned,
            inside_initcall ? "true" : "false", last_initcall_pc);
    } else {
        g_string_append_printf(s, "{\"watched\": false},\n");
    }

    g_string_append_printf(s, "  \"io\": ");
    if (!watch_io) {
        g_string_append_printf(s, "{\"watched\": false}\n");
    } else {
        g_string_append_printf(s, "{\"watched\": true, \"regions\": [\n");
        g_autoptr(GList) keys = g_hash_table_get_keys(io_by_device);
        keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
        for (GList *l = keys; l; l = l->next) {
            IoRegion *io = g_hash_table_lookup(io_by_device, l->data);
            g_string_append_printf(
                s,
                "    {\"device\": \"%s\", \"is_io\": %s, \"reads\": %" PRIu64
                ", \"writes\": %" PRIu64 ", \"first_pc\": \"0x%" PRIx64
                "\", \"paddr_lo\": \"0x%" PRIx64
                "\", \"paddr_hi\": \"0x%" PRIx64 "\"}%s\n",
                io->device, io->is_io ? "true" : "false",
                io->reads, io->writes, io->first_pc,
                io->paddr_lo, io->paddr_hi, l->next ? "," : "");
        }
        g_string_append_printf(s, "  ]}\n");
    }

    g_string_append_printf(s, "}\n");
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    g_autoptr(GString) report = g_string_new(NULL);
    uint64_t insns = qemu_plugin_u64_sum(insn_count_u64);

    g_mutex_lock(&state_lock);
    append_json(report, insns);
    g_mutex_unlock(&state_lock);

    if (out_path) {
        /*
         * Plain fopen rather than g_file_set_contents: the latter writes a
         * temporary alongside the target and renames it, which fails on
         * anything that is not a regular file -- including out=/dev/null,
         * the obvious way to ask for the report to be discarded.
         */
        FILE *f = fopen(out_path, "w");
        if (f && fwrite(report->str, 1, report->len, f) == report->len) {
            fclose(f);
        } else {
            if (f) {
                fclose(f);
            }
            fprintf(stderr, "bootwatch: cannot write %s: %s\n",
                    out_path, g_strerror(errno));
            qemu_plugin_outs(report->str);
        }
    } else {
        qemu_plugin_outs(report->str);
    }

    g_hash_table_destroy(rungs_by_addr);
    g_hash_table_destroy(io_by_device);
    qemu_plugin_scoreboard_free(insn_sb);
    g_free(out_path);
}

/* -------------------------------------------------------------------- setup */

/* rung=<addr>:<name> -- ladder order is argv order, not address order. */
static bool add_rung(const char *spec)
{
    g_auto(GStrv) parts = g_strsplit(spec, ":", 2);
    if (!parts[0] || !parts[0][0]) {
        return false;
    }

    Rung *r = g_new0(Rung, 1);
    r->addr = g_ascii_strtoull(parts[0], NULL, 0);
    r->name = g_strdup(parts[1] && parts[1][0] ? parts[1] : parts[0]);
    r->index = g_hash_table_size(rungs_by_addr);

    if (g_hash_table_contains(rungs_by_addr, &r->addr)) {
        fprintf(stderr, "bootwatch: duplicate rung address 0x%" PRIx64 "\n",
                r->addr);
        rung_free(r);
        return false;
    }
    /* Key points into the value; the value owns it and outlives the table. */
    g_hash_table_insert(rungs_by_addr, &r->addr, r);
    return true;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    if (info && info->target_name) {
        target_name = info->target_name;
    }

    insn_sb = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    insn_count_u64 = qemu_plugin_scoreboard_u64(insn_sb);

    rungs_by_addr = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                          NULL, rung_free);
    /* Keys are the IoRegion's own device string; the value frees it. */
    io_by_device = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         NULL, io_region_free);

    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) tokens = g_strsplit(argv[i], "=", 2);
        const char *key = tokens[0];
        const char *val = tokens[1];

        if (g_strcmp0(key, "rung") == 0 && val) {
            if (!add_rung(val)) {
                return -1;
            }
        } else if (g_strcmp0(key, "initcall") == 0 && val) {
            initcall_addr = g_ascii_strtoull(val, NULL, 0);
            watch_initcalls = true;
        } else if (g_strcmp0(key, "io") == 0 && val) {
            if (!qemu_plugin_bool_parse(key, val, &watch_io)) {
                fprintf(stderr, "bootwatch: bad boolean: %s\n", argv[i]);
                return -1;
            }
        } else if (g_strcmp0(key, "insns") == 0 && val) {
            if (!qemu_plugin_bool_parse(key, val, &count_insns)) {
                fprintf(stderr, "bootwatch: bad boolean: %s\n", argv[i]);
                return -1;
            }
        } else if (g_strcmp0(key, "iomin") == 0 && val) {
            io_min = g_ascii_strtoull(val, NULL, 0);
            io_window_set = true;
        } else if (g_strcmp0(key, "iomax") == 0 && val) {
            io_max = g_ascii_strtoull(val, NULL, 0);
            io_window_set = true;
        } else if (g_strcmp0(key, "out") == 0 && val) {
            g_free(out_path);
            out_path = g_strdup(val);
        } else {
            fprintf(stderr, "bootwatch: unrecognised option: %s\n", argv[i]);
            return -1;
        }
    }

    if (io_window_set && !watch_io) {
        fprintf(stderr, "bootwatch: iomin/iomax given without io=on\n");
        return -1;
    }

    if (g_hash_table_size(rungs_by_addr) == 0 && !watch_initcalls) {
        fprintf(stderr, "bootwatch: nothing to watch; pass at least one "
                        "'rung=<addr>:<name>' or 'initcall=<addr>'\n");
        return -1;
    }

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    return 0;
}
