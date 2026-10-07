/*
 * QEMU Plugin API - System specific implementations
 *
 * This provides the APIs that have a specific system implementation
 * or are only relevant to system-mode.
 *
 * Copyright (C) 2017, Emilio G. Cota <cota@braap.org>
 * Copyright (C) 2019-2025, Linaro
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "migration/blocker.h"
#include "hw/core/boards.h"
#include "qemu/plugin-memory.h"
#include "qemu/plugin.h"
#include "accel/tcg/cpu-ops.h"
#include "accel/tcg/probe.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/target_page.h"
#include "exec/tlb-flags.h"

/*
 * In system mode we cannot trace the binary being executed so the
 * helpers all return NULL/0.
 */
const char *qemu_plugin_path_to_binary(void)
{
    return NULL;
}

uint64_t qemu_plugin_start_code(void)
{
    return 0;
}

uint64_t qemu_plugin_end_code(void)
{
    return 0;
}

uint64_t qemu_plugin_entry_code(void)
{
    return 0;
}

/*
 * Virtual Memory queries
 */

static __thread struct qemu_plugin_hwaddr hwaddr_info;

struct qemu_plugin_hwaddr *qemu_plugin_get_hwaddr(qemu_plugin_meminfo_t info,
                                                  uint64_t vaddr)
{
    CPUState *cpu = current_cpu;
    unsigned int mmu_idx = get_mmuidx(info);
    enum qemu_plugin_mem_rw rw = get_plugin_meminfo_rw(info);
    hwaddr_info.is_store = (rw & QEMU_PLUGIN_MEM_W) != 0;

    assert(mmu_idx < NB_MMU_MODES);

    if (!tlb_plugin_lookup(cpu, vaddr, mmu_idx,
                           hwaddr_info.is_store, &hwaddr_info)) {
        error_report("invalid use of qemu_plugin_get_hwaddr");
        return NULL;
    }

    return &hwaddr_info;
}

bool qemu_plugin_hwaddr_is_io(const struct qemu_plugin_hwaddr *haddr)
{
    return haddr->is_io;
}

uint64_t qemu_plugin_hwaddr_phys_addr(const struct qemu_plugin_hwaddr *haddr)
{
    if (haddr) {
        return haddr->phys_addr;
    }
    return 0;
}

const char *qemu_plugin_hwaddr_device_name(const struct qemu_plugin_hwaddr *h)
{
    if (h && h->is_io) {
        MemoryRegion *mr = h->mr;
        if (!mr->name) {
            unsigned maddr = (uintptr_t)mr;
            g_autofree char *temp = g_strdup_printf("anon%08x", maddr);
            return g_intern_string(temp);
        } else {
            return g_intern_string(mr->name);
        }
    } else {
        return g_intern_static_string("RAM");
    }
}

/*
 * Time control
 */
static bool has_control;
static Error *migration_blocker;

const void *qemu_plugin_request_time_control(void)
{
    if (!has_control) {
        has_control = true;
        error_setg(&migration_blocker,
                   "TCG plugin time control does not support migration");
        migrate_add_blocker(&migration_blocker, &error_fatal);
        return &has_control;
    }
    return NULL;
}

static void advance_virtual_time__async(CPUState *cpu, run_on_cpu_data data)
{
    int64_t new_time = data.host_ulong;
    qemu_clock_advance_virtual_time(new_time);
}

void qemu_plugin_update_ns(const void *handle, int64_t new_time)
{
    if (handle == &has_control) {
        /* Need to execute out of cpu_exec, so bql can be locked. */
        async_run_on_cpu(current_cpu,
                         advance_virtual_time__async,
                         RUN_ON_CPU_HOST_ULONG(new_time));
    }
}

/*
 * Consistent virtual memory access
 */

/*
 * Make every page of [addr, addr + len) accessible for @access_type, then
 * copy.  Checking all pages before copying anything means a fault on a
 * later page never leaves the guest looking at a partial write, and a read
 * is taken while no guest code runs on this vCPU.
 */
static int vaddr_access_fault(uint64_t addr, uint8_t *buf, size_t len,
                              enum qemu_plugin_vaddr_mode mode, bool fault,
                              MMUAccessType access_type)
{
    CPUState *cpu = current_cpu;
    CPUArchState *env;
    size_t page_size = qemu_target_page_size();
    void (*window)(CPUState *, bool) = NULL;
    int mmu_idx, fault_idx, ret = QEMU_PLUGIN_VADDR_OK;

    g_assert(cpu);
    env = cpu_env(cpu);

    if (len == 0) {
        return QEMU_PLUGIN_VADDR_UNMAPPED;
    }
    if (mode == QEMU_PLUGIN_VADDR_USER) {
        if (!cpu->cc->tcg_ops->user_mmu_index) {
            return QEMU_PLUGIN_VADDR_BAD_MODE;
        }
        window = cpu->cc->tcg_ops->user_access_window;
        if (window) {
            window(cpu, true);
        }
        mmu_idx = cpu->cc->tcg_ops->user_mmu_index(cpu);
        fault_idx = cpu->cc->tcg_ops->user_fault_mmu_index ?
                    cpu->cc->tcg_ops->user_fault_mmu_index(cpu) : mmu_idx;
    } else {
        mmu_idx = fault_idx = cpu_mmu_index(cpu, false);
    }

    for (int copy = 0; copy < 2 && ret == QEMU_PLUGIN_VADDR_OK; copy++) {
        size_t done = 0;

        while (done < len) {
            vaddr a = addr + done;
            size_t chunk = MIN(len - done,
                               page_size - (a & (page_size - 1)));
            CPUTLBEntryFull *full;
            void *host;
            int flags;

            /*
             * Never faults, never triggers plugin memory instrumentation.
             * For a store of the chunk it also invalidates translated code
             * on the page and marks it dirty.
             */
            flags = probe_access_full_mmu(env, a,
                                          copy && access_type == MMU_DATA_STORE
                                          ? chunk : 0,
                                          access_type, mmu_idx, &host, &full);
            if (flags & TLB_INVALID_MASK) {
                uintptr_t ra = cpu->neg.plugin_cb_ra;

                if (!fault || !ra ||
                    qemu_plugin_get_cb_flags() != QEMU_PLUGIN_CB_RW_REGS_PC) {
                    ret = QEMU_PLUGIN_VADDR_UNMAPPED;
                    break;
                }
                /*
                 * Raise the exception a guest load or store at the start of
                 * the current instruction would take (#PF, data abort, TLBL,
                 * TLBS, DSI ...).  The guest resolves it (pages the address
                 * in, breaks copy-on-write, marks the page dirty) and resumes
                 * at the same instruction, so the callback runs again.
                 * Returns only if the translation became valid meanwhile.
                 * A user access window stays open: the guest saves it with
                 * the interrupted context and restores it when it resumes
                 * here; we close it on the way out.
                 */
                probe_access(env, a, 0, access_type, fault_idx, ra);
                flags = probe_access_full_mmu(env, a, 0, access_type, mmu_idx,
                                              &host, &full);
                if (flags & TLB_INVALID_MASK) {
                    /* fault_idx may access it, mmu_idx may not */
                    ret = QEMU_PLUGIN_VADDR_UNMAPPED;
                    break;
                }
                continue;
            }
            if (flags & TLB_MMIO) {
                /* Not RAM: an access could have device side effects. */
                ret = QEMU_PLUGIN_VADDR_IO;
                break;
            }
            if (copy) {
                if (access_type == MMU_DATA_STORE) {
                    memcpy(host, buf + done, chunk);
                } else {
                    memcpy(buf + done, host, chunk);
                }
            }
            done += chunk;
        }
    }
    if (window) {
        window(cpu, false);
    }
    return ret;
}

int qemu_plugin_read_memory_vaddr_fault(uint64_t addr, GByteArray *data,
                                        size_t len,
                                        enum qemu_plugin_vaddr_mode mode,
                                        bool fault)
{
    g_byte_array_set_size(data, len);
    return vaddr_access_fault(addr, data->data, len, mode, fault,
                              MMU_DATA_LOAD);
}

int qemu_plugin_write_memory_vaddr_fault(uint64_t addr, GByteArray *data,
                                         enum qemu_plugin_vaddr_mode mode,
                                         bool fault)
{
    return vaddr_access_fault(addr, data->data, data->len, mode, fault,
                              MMU_DATA_STORE);
}
