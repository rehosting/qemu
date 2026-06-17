/*
 * Penguin integration hooks.
 *
 * These hooks are intentionally small C ABI surfaces for embedding QEMU in
 * Penguin without depending on PANDA internals.
 */
#ifndef QEMU_SYSTEM_PENGUIN_H
#define QEMU_SYSTEM_PENGUIN_H

#include "qemu/typedefs.h"

typedef int (*penguin_guest_hypercall_cb_t)(CPUState *cs, uint64_t nr,
                                            uint64_t a0, uint64_t a1,
                                            uint64_t a2, uint64_t a3,
                                            uint64_t a4, uint64_t a5,
                                            uint64_t *ret, void *opaque);

typedef int (*kvm_penguin_hypercall_cb_t)(CPUState *cs, uint64_t nr,
                                          uint64_t a0, uint64_t a1,
                                          uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5,
                                          uint64_t *ret);

typedef uint64_t (*penguin_mmio_read_cb_t)(uint64_t addr, unsigned size,
                                           void *opaque);
typedef void (*penguin_mmio_write_cb_t)(uint64_t addr, uint64_t data,
                                        unsigned size, void *opaque);

void set_penguin_guest_hypercall_callback(penguin_guest_hypercall_cb_t cb,
                                          void *opaque);
void penguin_register_guest_hypercall(uint64_t nr);
void penguin_unregister_guest_hypercall(uint64_t nr);
void penguin_clear_guest_hypercalls(void);
bool penguin_guest_hypercall_registered(uint64_t nr);
void set_kvm_penguin_hypercall_callback(kvm_penguin_hypercall_cb_t cb);

bool penguin_handle_guest_hypercall(CPUState *cs, uint64_t nr,
                                    uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5,
                                    uint64_t *ret);

int penguin_qemu_add_mmio_region(uint64_t base, uint64_t size,
                                 const char *name,
                                 penguin_mmio_read_cb_t read_cb,
                                 penguin_mmio_write_cb_t write_cb,
                                 void *opaque);

/*
 * Guest register access by GDB core-feature register number. Reads append
 * the register bytes (target byte order) into @buf and return the register
 * width; writes consume exactly the register width from @buf. Both return
 * a negative value on failure.
 */
int penguin_read_guest_reg(CPUState *cs, int regnum, uint8_t *buf,
                           int buf_len);
int penguin_write_guest_reg(CPUState *cs, int regnum, const uint8_t *buf,
                            int len);

/*
 * Direct CPUArchState access. penguin_cpu_env returns the env pointer for
 * a CPU (the layout contract validated in cpu-target.c); callers decode it
 * with the build-generated CPUArchState CFFI header. penguin_sync_cpu_state
 * must be called before env reads (and to make env writes stick) under
 * hardware accelerators; it is a no-op under TCG.
 */
void *penguin_cpu_env(CPUState *cs);
void penguin_sync_cpu_state(CPUState *cs);

/*
 * Save/load an internal VM snapshot by name. Minimal C ABI wrappers around
 * save_snapshot()/load_snapshot() for Penguin's CFFI layer. Must be called
 * with the BQL held from the main loop context. Return true on success.
 */
bool penguin_save_snapshot(const char *name);
bool penguin_load_snapshot(const char *name);

/*
 * Schedule a save (load=false) or load (load=true) snapshot to run on the
 * main loop. Safe to call from a vCPU thread; fire-and-forget.
 */
void penguin_schedule_snapshot(const char *name, bool load);

#endif /* QEMU_SYSTEM_PENGUIN_H */
