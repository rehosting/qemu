#include "qemu/osdep.h"
#include "system/penguin.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/hw_accel.h"
#include "exec/gdbstub.h"

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
static GHashTable *penguin_guest_hypercall_numbers;

static void penguin_guest_hypercalls_init(void)
{
    if (!penguin_guest_hypercall_numbers) {
        penguin_guest_hypercall_numbers = g_hash_table_new_full(g_int64_hash,
                                                                g_int64_equal,
                                                                g_free, NULL);
    }
}

void __attribute__((visibility("default")))
penguin_register_guest_hypercall(uint64_t nr)
{
    uint64_t *key;

    penguin_guest_hypercalls_init();
    if (g_hash_table_contains(penguin_guest_hypercall_numbers, &nr)) {
        return;
    }

    key = g_new(uint64_t, 1);
    *key = nr;
    g_hash_table_add(penguin_guest_hypercall_numbers, key);
}

void __attribute__((visibility("default")))
penguin_unregister_guest_hypercall(uint64_t nr)
{
    if (penguin_guest_hypercall_numbers) {
        g_hash_table_remove(penguin_guest_hypercall_numbers, &nr);
    }
}

void __attribute__((visibility("default")))
penguin_clear_guest_hypercalls(void)
{
    if (penguin_guest_hypercall_numbers) {
        g_hash_table_remove_all(penguin_guest_hypercall_numbers);
    }
}

bool __attribute__((visibility("default")))
penguin_guest_hypercall_registered(uint64_t nr)
{
    return penguin_guest_hypercall_numbers &&
           g_hash_table_contains(penguin_guest_hypercall_numbers, &nr);
}

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
    if (penguin_guest_hypercall_cb &&
        penguin_guest_hypercall_registered(nr)) {
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

int __attribute__((visibility("default")))
penguin_read_guest_reg(CPUState *cs, int regnum, uint8_t *buf, int buf_len)
{
    GByteArray *bytes;
    int len;

    if (!cs || !buf || buf_len <= 0) {
        return -1;
    }

    cpu_synchronize_state(cs);
    bytes = g_byte_array_new();
    len = gdb_read_register(cs, bytes, regnum);
    if (len <= 0 || len > buf_len) {
        g_byte_array_free(bytes, true);
        return -1;
    }
    memcpy(buf, bytes->data, len);
    g_byte_array_free(bytes, true);
    return len;
}

int __attribute__((visibility("default")))
penguin_write_guest_reg(CPUState *cs, int regnum, const uint8_t *buf, int len)
{
    if (!cs || !buf || len <= 0) {
        return -1;
    }

    cpu_synchronize_state(cs);
    if (gdb_write_register(cs, (uint8_t *)buf, regnum) <= 0) {
        return -1;
    }
    return 0;
}

void __attribute__((visibility("default")))
*penguin_cpu_env(CPUState *cs)
{
    /*
     * CPUArchState immediately follows CPUState in ArchCPU; cpu-target.c
     * validates this layout for every target. Mirrors cpu_env() without
     * needing target-specific types in common code.
     */
    if (!cs) {
        return NULL;
    }
    return (void *)(cs + 1);
}

void __attribute__((visibility("default")))
penguin_sync_cpu_state(CPUState *cs)
{
    /*
     * Pull register state out of the accelerator (KVM) into env and mark
     * the vCPU dirty so direct env writes are pushed back on next entry.
     * No-op under TCG.
     */
    if (cs) {
        cpu_synchronize_state(cs);
    }
}
