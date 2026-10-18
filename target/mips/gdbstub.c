/*
 * MIPS gdb server stub
 *
 * Copyright (c) 2003-2005 Fabrice Bellard
 * Copyright (c) 2013 SUSE LINUX Products GmbH
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
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "cpu.h"
#include "internal.h"
#include "gdbstub/helpers.h"
#include "exec/gdbstub.h"
#include "fpu_helper.h"

int mips_cpu_gdb_read_register(CPUState *cs, GByteArray *mem_buf, int n)
{
    CPUMIPSState *env = cpu_env(cs);

    if (n < 32) {
        return gdb_get_regl(mem_buf, env->active_tc.gpr[n]);
    }
    if (env->CP0_Config1 & (1 << CP0C1_FP) && n >= 38 && n < 72) {
        switch (n) {
        case 70:
            return gdb_get_regl(mem_buf, (int32_t)env->active_fpu.fcr31);
        case 71:
            return gdb_get_regl(mem_buf, (int32_t)env->active_fpu.fcr0);
        default:
            if (env->CP0_Status & (1 << CP0St_FR)) {
                return gdb_get_regl(mem_buf,
                    env->active_fpu.fpr[n - 38].d);
            } else {
                return gdb_get_regl(mem_buf,
                    env->active_fpu.fpr[n - 38].w[FP_ENDIAN_IDX]);
            }
        }
    }
    switch (n) {
    case 32:
        return gdb_get_regl(mem_buf, (int32_t)env->CP0_Status);
    case 33:
        return gdb_get_regl(mem_buf, env->active_tc.LO[0]);
    case 34:
        return gdb_get_regl(mem_buf, env->active_tc.HI[0]);
    case 35:
        return gdb_get_regl(mem_buf, env->CP0_BadVAddr);
    case 36:
        return gdb_get_regl(mem_buf, (int32_t)env->CP0_Cause);
    case 37:
        return gdb_get_regl(mem_buf, env->active_tc.PC |
                                     !!(env->hflags & MIPS_HFLAG_M16));
    case 72:
        return gdb_get_regl(mem_buf, 0); /* fp */
    case 89:
        return gdb_get_regl(mem_buf, (int32_t)env->CP0_PRid);
    default:
        if (n > 89) {
            return 0;
        }
        /* 16 embedded regs.  */
        return gdb_get_regl(mem_buf, 0);
    }

    return 0;
}

int mips_cpu_gdb_write_register(CPUState *cs, uint8_t *mem_buf, int n)
{
    const unsigned regsz = target_long_bits() / 8;
    CPUMIPSState *env = cpu_env(cs);
    uint64_t tmp;

    tmp = ldn_p(mem_buf, regsz);

    if (n < 32) {
        env->active_tc.gpr[n] = tmp;
        return regsz;
    }
    if (env->CP0_Config1 & (1 << CP0C1_FP) && n >= 38 && n < 72) {
        switch (n) {
        case 70:
            env->active_fpu.fcr31 = (tmp & env->active_fpu.fcr31_rw_bitmask) |
                  (env->active_fpu.fcr31 & ~(env->active_fpu.fcr31_rw_bitmask));
            restore_fp_status(env);
            break;
        case 71:
            /* FIR is read-only.  Ignore writes.  */
            break;
        default:
            if (env->CP0_Status & (1 << CP0St_FR)) {
                env->active_fpu.fpr[n - 38].d = tmp;
            } else {
                env->active_fpu.fpr[n - 38].w[FP_ENDIAN_IDX] = tmp;
            }
            break;
        }
        return regsz;
    }
    switch (n) {
    case 32:
#ifndef CONFIG_USER_ONLY
        cpu_mips_store_status(env, tmp);
#endif
        break;
    case 33:
        env->active_tc.LO[0] = tmp;
        break;
    case 34:
        env->active_tc.HI[0] = tmp;
        break;
    case 35:
        env->CP0_BadVAddr = tmp;
        break;
    case 36:
#ifndef CONFIG_USER_ONLY
        cpu_mips_store_cause(env, tmp);
#endif
        break;
    case 37:
        env->active_tc.PC = deposit64(tmp, 63, 1, 0);
        if (tmp & 1) {
            env->hflags |= MIPS_HFLAG_M16;
        } else {
            env->hflags &= ~(MIPS_HFLAG_M16);
        }
        break;
    case 72: /* fp, ignored */
        break;
    default:
        if (n > 89) {
            return 0;
        }
        /* Other registers are readonly.  Ignore writes.  */
        break;
    }

    return regsz;
}

/*
 * The core registers again as a register feature, for TCG plugins
 * (qemu_plugin_get_registers lists only registers that belong to a feature,
 * and MIPS has no core XML). gdb never sees it: without a core XML the
 * stub sends gdb no target description, so gdb keeps its built-in layout.
 * Numbering is the core one: r0-r31, sr, lo, hi, bad, cause, pc.
 */
static const char * const mips_plugin_reg_names[38] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
    "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31",
    "sr", "lo", "hi", "bad", "cause", "pc",
};

static GDBFeature mips_plugin_regs_feature;

void mips_cpu_register_plugin_regs(CPUState *cs)
{
    static gsize built;

    if (g_once_init_enter(&built)) {
        GDBFeatureBuilder builder;

        gdb_feature_builder_init(&builder, &mips_plugin_regs_feature,
                                 "org.qemu.mips.plugin-regs",
                                 "mips-plugin-regs.xml", 0);
        for (int i = 0; i < ARRAY_SIZE(mips_plugin_reg_names); i++) {
            gdb_feature_builder_append_reg(&builder, mips_plugin_reg_names[i],
                                           TARGET_LONG_BITS, i, "int", NULL);
        }
        gdb_feature_builder_end(&builder);
        g_once_init_leave(&built, 1);
    }
    gdb_register_coprocessor(cs, mips_cpu_gdb_read_register,
                             mips_cpu_gdb_write_register,
                             &mips_plugin_regs_feature);
}
