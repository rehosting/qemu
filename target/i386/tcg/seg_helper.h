/*
 *  x86 segmentation related helpers macros
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SEG_HELPER_H
#define SEG_HELPER_H

#include "cpu.h"

//#define DEBUG_PCALL

#ifdef DEBUG_PCALL
# define LOG_PCALL(...) qemu_log_mask(CPU_LOG_PCALL, ## __VA_ARGS__)
# define LOG_PCALL_STATE(cpu)                                  \
    log_cpu_state_mask(CPU_LOG_PCALL, (cpu), CPU_DUMP_CCOP)
#else
# define LOG_PCALL(...) do { } while (0)
# define LOG_PCALL_STATE(cpu) do { } while (0)
#endif

int cpu_mmu_index_kernel(CPUX86State *env);

/*
 * TODO: Convert callers to compute cpu_mmu_index_kernel once
 * and use *_mmuidx_ra directly.
 */
#define cpu_lduw_kernel_ra(e, p, r) \
    cpu_lduw_le_mmuidx_ra(e, p, cpu_mmu_index_kernel(e), r)
#define cpu_ldl_kernel_ra(e, p, r) \
    cpu_ldl_le_mmuidx_ra(e, p, cpu_mmu_index_kernel(e), r)
#define cpu_ldq_kernel_ra(e, p, r) \
    cpu_ldq_le_mmuidx_ra(e, p, cpu_mmu_index_kernel(e), r)

#define cpu_stw_kernel_ra(e, p, v, r) \
    cpu_stw_le_mmuidx_ra(e, p, v, cpu_mmu_index_kernel(e), r)
#define cpu_stl_kernel_ra(e, p, v, r) \
    cpu_stl_le_mmuidx_ra(e, p, v, cpu_mmu_index_kernel(e), r)
#define cpu_stq_kernel_ra(e, p, v, r) \
    cpu_stq_le_mmuidx_ra(e, p, v, cpu_mmu_index_kernel(e), r)

#define cpu_lduw_kernel(e, p)    cpu_lduw_kernel_ra(e, p, 0)
#define cpu_ldl_kernel(e, p)     cpu_ldl_kernel_ra(e, p, 0)
#define cpu_ldq_kernel(e, p)     cpu_ldq_kernel_ra(e, p, 0)

#define cpu_stw_kernel(e, p, v)  cpu_stw_kernel_ra(e, p, v, 0)
#define cpu_stl_kernel(e, p, v)  cpu_stl_kernel_ra(e, p, v, 0)
#define cpu_stq_kernel(e, p, v)  cpu_stq_kernel_ra(e, p, v, 0)

#ifndef CONFIG_USER_ONLY
/* syscall events: the task key, the kernel's per-CPU base, a return */
static inline uint64_t x86_sc_key(CPUX86State *env)
{
    return env->cr[3] & ~0xfffull;      /* the address space, without PCID */
}
static inline uint64_t x86_sc_percpu(CPUX86State *env)
{
#ifdef TARGET_X86_64
    return env->kernelgsbase;           /* swapgs has not run yet */
#else
    return 0;
#endif
}
#define X86_SC_KMMU ((1u << MMU_KSMAP64_IDX) | (1u << MMU_KNOSMAP64_IDX) | \
                     (1u << MMU_KSMAP32_IDX) | (1u << MMU_KNOSMAP32_IDX))
void x86_sc_return(CPUX86State *env);
bool x86_sc_i386(CPUX86State *env, target_ulong next, unsigned len,
                 int a2, int usp, bool fast);
#endif

#endif /* SEG_HELPER_H */
