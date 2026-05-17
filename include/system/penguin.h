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

#endif /* QEMU_SYSTEM_PENGUIN_H */
