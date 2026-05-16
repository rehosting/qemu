#include "qemu/osdep.h"
#include "system/penguin.h"

static penguin_guest_hypercall_cb_t penguin_guest_hypercall_cb;
static void *penguin_guest_hypercall_opaque;
static kvm_penguin_hypercall_cb_t kvm_penguin_hypercall_cb;

void __attribute__((visibility("default")))
set_penguin_guest_hypercall_callback(penguin_guest_hypercall_cb_t cb,
                                     void *opaque)
{
    penguin_guest_hypercall_cb = cb;
    penguin_guest_hypercall_opaque = opaque;
}

void __attribute__((visibility("default")))
set_kvm_penguin_hypercall_callback(kvm_penguin_hypercall_cb_t cb)
{
    kvm_penguin_hypercall_cb = cb;
}

bool penguin_handle_guest_hypercall(CPUState *cs, uint64_t nr,
                                    uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5,
                                    uint64_t *ret)
{
    if (penguin_guest_hypercall_cb) {
        return penguin_guest_hypercall_cb(cs, nr, a0, a1, a2, a3, a4, a5,
                                          ret,
                                          penguin_guest_hypercall_opaque) == 0;
    }

    if (kvm_penguin_hypercall_cb) {
        return kvm_penguin_hypercall_cb(cs, nr, a0, a1, a2, a3, a4, a5,
                                        ret) == 0;
    }

    return false;
}
