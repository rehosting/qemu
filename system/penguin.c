#include "qemu/osdep.h"
#include "system/penguin.h"
#include "system/address-spaces.h"
#include "system/memory.h"

typedef struct PenguinMmioRegion {
    MemoryRegion mr;
    MemoryRegionOps ops;
    penguin_mmio_read_cb_t read_cb;
    penguin_mmio_write_cb_t write_cb;
    void *opaque;
    char *name;
} PenguinMmioRegion;

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

static uint64_t penguin_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    PenguinMmioRegion *region = opaque;

    if (!region->read_cb) {
        return 0;
    }

    return region->read_cb(addr, size, region->opaque);
}

static void penguin_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                               unsigned size)
{
    PenguinMmioRegion *region = opaque;

    if (!region->write_cb) {
        return;
    }

    region->write_cb(addr, data, size, region->opaque);
}

int __attribute__((visibility("default")))
penguin_qemu_add_mmio_region(uint64_t base, uint64_t size,
                             const char *name,
                             penguin_mmio_read_cb_t read_cb,
                             penguin_mmio_write_cb_t write_cb,
                             void *opaque)
{
    PenguinMmioRegion *region;

    if (!size || !name || (!read_cb && !write_cb)) {
        return -1;
    }

    region = g_new0(PenguinMmioRegion, 1);
    region->read_cb = read_cb;
    region->write_cb = write_cb;
    region->opaque = opaque;
    region->name = g_strdup(name);
    region->ops.read = penguin_mmio_read;
    region->ops.write = penguin_mmio_write;
    region->ops.endianness = DEVICE_NATIVE_ENDIAN;
    region->ops.valid.min_access_size = 1;
    region->ops.valid.max_access_size = 8;
    region->ops.impl.min_access_size = 1;
    region->ops.impl.max_access_size = 8;

    memory_region_init_io(&region->mr, NULL, &region->ops, region,
                          region->name, size);
    memory_region_add_subregion_overlap(get_system_memory(), base,
                                        &region->mr, -1000);
    return 0;
}
