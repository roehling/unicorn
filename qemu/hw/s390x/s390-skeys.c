/*
 * s390 storage key device
 *
 * Copyright 2015 IBM Corp.
 * Author(s): Jason J. Herne <jjherne@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "target/s390x/cpu.h"
#include "hw/s390x/storage-keys.h"

#define S390_SKEYS_BUFFER_SIZE (128 * KiB)  /* Room for 128k storage keys */
#define S390_SKEYS_SAVE_FLAG_EOS 0x01
#define S390_SKEYS_SAVE_FLAG_SKEYS 0x02
#define S390_SKEYS_SAVE_FLAG_ERROR 0x04

#if 0
S390SKeysState *s390_get_skeys_device(void)
{
    S390SKeysState *ss;

    ss = S390_SKEYS(object_resolve_path_type("", TYPE_S390_SKEYS, NULL));
    assert(ss);
    return ss;
}

void s390_skeys_init(void)
{
    Object *obj;

    obj = object_new(TYPE_QEMU_S390_SKEYS);
    object_property_add_child(qdev_get_machine(), TYPE_S390_SKEYS,
                              obj, NULL);
    object_unref(obj);

    qdev_init_nofail(DEVICE(obj));
}

static void qemu_s390_skeys_init(Object *obj)
{
    QEMUS390SKeysState *skeys = QEMU_S390_SKEYS(obj);
    MachineState *machine = MACHINE(qdev_get_machine());

    skeys->key_count = machine->ram_size / TARGET_PAGE_SIZE;
    skeys->keydata = g_malloc0(skeys->key_count);
}

static int qemu_s390_skeys_enabled(S390SKeysState *ss)
{
    return 1;
}

/*
 * TODO: for memory hotplug support qemu_s390_skeys_set and qemu_s390_skeys_get
 * will have to make sure that the given gfn belongs to a memory region and not
 * a memory hole.
 */
static int qemu_s390_skeys_set(S390SKeysState *ss, uint64_t start_gfn,
                              uint64_t count, uint8_t *keys)
{
    QEMUS390SKeysState *skeydev = QEMU_S390_SKEYS(ss);
    int i;

    /* Check for uint64 overflow and access beyond end of key data */
    if (start_gfn + count > skeydev->key_count || start_gfn + count < count) {
        error_report("Error: Setting storage keys for page beyond the end "
                     "of memory: gfn=%" PRIx64 " count=%" PRId64,
                     start_gfn, count);
        return -EINVAL;
    }

    for (i = 0; i < count; i++) {
        skeydev->keydata[start_gfn + i] = keys[i];
    }
    return 0;
}

static int qemu_s390_skeys_get(S390SKeysState *ss, uint64_t start_gfn,
                               uint64_t count, uint8_t *keys)
{
    QEMUS390SKeysState *skeydev = QEMU_S390_SKEYS(ss);
    int i;

    /* Check for uint64 overflow and access beyond end of key data */
    if (start_gfn + count > skeydev->key_count || start_gfn + count < count) {
        error_report("Error: Getting storage keys for page beyond the end "
                     "of memory: gfn=%" PRIx64 " count=%" PRId64,
                     start_gfn, count);
        return -EINVAL;
    }

    for (i = 0; i < count; i++) {
        keys[i] = skeydev->keydata[start_gfn + i];
    }
    return 0;
}

static void qemu_s390_skeys_class_init(ObjectClass *oc, void *data)
{
    S390SKeysClass *skeyclass = S390_SKEYS_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    skeyclass->skeys_enabled = qemu_s390_skeys_enabled;
    skeyclass->get_skeys = qemu_s390_skeys_get;
    skeyclass->set_skeys = qemu_s390_skeys_set;

    /* Reason: Internal device (only one skeys device for the whole memory) */
    dc->user_creatable = false;
}

static const TypeInfo qemu_s390_skeys_info = {
    .name          = TYPE_QEMU_S390_SKEYS,
    .parent        = TYPE_S390_SKEYS,
    .instance_init = qemu_s390_skeys_init,
    .instance_size = sizeof(QEMUS390SKeysState),
    .class_init    = qemu_s390_skeys_class_init,
    .class_size    = sizeof(S390SKeysClass),
};

static void write_keys(FILE *f, uint8_t *keys, uint64_t startgfn,
                       uint64_t count, Error **errp)
{
    uint64_t curpage = startgfn;
    uint64_t maxpage = curpage + count - 1;

    for (; curpage <= maxpage; curpage++) {
        uint8_t acc = (*keys & 0xF0) >> 4;
        int fp =  (*keys & 0x08);
        int ref = (*keys & 0x04);
        int ch = (*keys & 0x02);
        int res = (*keys & 0x01);

        fprintf(f, "page=%03" PRIx64 ": key(%d) => ACC=%X, FP=%d, REF=%d,"
                " ch=%d, reserved=%d\n",
                curpage, *keys, acc, fp, ref, ch, res);
        keys++;
    }
}

static void s390_storage_keys_save(QEMUFile *f, void *opaque)
{
    S390SKeysState *ss = S390_SKEYS(opaque);
    S390SKeysClass *skeyclass = S390_SKEYS_GET_CLASS(ss);
    uint64_t pages_left = ram_size / TARGET_PAGE_SIZE;
    uint64_t read_count, eos = S390_SKEYS_SAVE_FLAG_EOS;
    vaddr cur_gfn = 0;
    int error = 0;
    uint8_t *buf;

    if (!skeyclass->skeys_enabled(ss)) {
        goto end_stream;
    }

    buf = g_try_malloc(S390_SKEYS_BUFFER_SIZE);
    if (!buf) {
        error_report("storage key save could not allocate memory");
        goto end_stream;
    }

    /* We only support initial memory. Standby memory is not handled yet. */
    qemu_put_be64(f, (cur_gfn * TARGET_PAGE_SIZE) | S390_SKEYS_SAVE_FLAG_SKEYS);
    qemu_put_be64(f, pages_left);

    while (pages_left) {
        read_count = MIN(pages_left, S390_SKEYS_BUFFER_SIZE);

        if (!error) {
            error = skeyclass->get_skeys(ss, cur_gfn, read_count, buf);
            if (error) {
                /*
                 * If error: we want to fill the stream with valid data instead
                 * of stopping early so we pad the stream with 0x00 values and
                 * use S390_SKEYS_SAVE_FLAG_ERROR to indicate failure to the
                 * reading side.
                 */
                error_report("S390_GET_KEYS error %d", error);
                memset(buf, 0, S390_SKEYS_BUFFER_SIZE);
                eos = S390_SKEYS_SAVE_FLAG_ERROR;
            }
        }

        qemu_put_buffer(f, buf, read_count);
        cur_gfn += read_count;
        pages_left -= read_count;
    }

    g_free(buf);
end_stream:
    qemu_put_be64(f, eos);
}

static int s390_storage_keys_load(QEMUFile *f, void *opaque, int version_id)
{
    S390SKeysState *ss = S390_SKEYS(opaque);
    S390SKeysClass *skeyclass = S390_SKEYS_GET_CLASS(ss);
    int ret = 0;

    while (!ret) {
        ram_addr_t addr;
        int flags;

        addr = qemu_get_be64(f);
        flags = addr & ~TARGET_PAGE_MASK;
        addr &= TARGET_PAGE_MASK;

        switch (flags) {
        case S390_SKEYS_SAVE_FLAG_SKEYS: {
            const uint64_t total_count = qemu_get_be64(f);
            uint64_t handled_count = 0, cur_count;
            uint64_t cur_gfn = addr / TARGET_PAGE_SIZE;
            uint8_t *buf = g_try_malloc(S390_SKEYS_BUFFER_SIZE);

            if (!buf) {
                error_report("storage key load could not allocate memory");
                ret = -ENOMEM;
                break;
            }

            while (handled_count < total_count) {
                cur_count = MIN(total_count - handled_count,
                                S390_SKEYS_BUFFER_SIZE);
                qemu_get_buffer(f, buf, cur_count);

                ret = skeyclass->set_skeys(ss, cur_gfn, cur_count, buf);
                if (ret < 0) {
                    error_report("S390_SET_KEYS error %d", ret);
                    break;
                }
                handled_count += cur_count;
                cur_gfn += cur_count;
            }
            g_free(buf);
            break;
        }
        case S390_SKEYS_SAVE_FLAG_ERROR: {
            error_report("Storage key data is incomplete");
            ret = -EINVAL;
            break;
        }
        case S390_SKEYS_SAVE_FLAG_EOS:
            /* normal exit */
            return 0;
        default:
            error_report("Unexpected storage key flag data: %#x", flags);
            ret = -EINVAL;
        }
    }

    return ret;
}

static void s390_skeys_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo s390_skeys_info = {
    .name          = TYPE_S390_SKEYS,
    .parent        = TYPE_DEVICE,
    .instance_init = NULL,
    .instance_size = sizeof(S390SKeysState),
    .class_init    = s390_skeys_class_init,
    .class_size    = sizeof(S390SKeysClass),
    .abstract = true,
};

static void qemu_s390_skeys_register_types(void)
{
    type_register_static(&s390_skeys_info);
    type_register_static(&qemu_s390_skeys_info);
}

type_init(qemu_s390_skeys_register_types)
#endif