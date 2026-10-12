#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/input.h>
#include <serika/intc.h>
#include <serika/page.h>
#include <serika/pci.h>
#include <serika/printk.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/virtio.h>

/* virtio-input: a keyboard, a mouse or a tablet from qemu. the device says what it is in its configuration (a name,
 * the ids and the bits of what it can send), and sends its events through a queue of buffers that we hand out */

#define VIRTIO_ID_INPUT 0x1052

#define CFG_ID_NAME   0x01
#define CFG_ID_DEVIDS 0x03
#define CFG_PROP_BITS 0x10
#define CFG_EV_BITS   0x11
#define CFG_ABS_INFO  0x12

#define CFG_SELECT 0
#define CFG_SUBSEL 1
#define CFG_SIZE   2
#define CFG_DATA   8

#define EVENTQ_BUFFERS 64 /* a page of 8 byte events holds many more than a queue does */

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    int32_t  value;
} __attribute__((packed));

struct vinput {
    struct virtio_dev *vdev;
    struct virtqueue   vq;
    struct virtio_input_event *events; /* a page, the buffers of the queue */
    uintptr_t          events_phys;
    unsigned           nbuf;

    struct input_dev   dev;
    char               parent[96];
};

/* what the device has in its configuration for (select, subsel): its size and bytes */
static unsigned cfg_read(struct virtio_dev *v, uint8_t select, uint8_t subsel, void *out, unsigned max) {
    volatile uint8_t *cfg = v->device;

    cfg[CFG_SELECT] = select;
    cfg[CFG_SUBSEL] = subsel;
    __asm__ volatile ("mfence" ::: "memory");

    unsigned size = cfg[CFG_SIZE];

    if (size > max) size = max;

    for (unsigned i = 0; i < size; i++) ((uint8_t *)out)[i] = cfg[CFG_DATA + i];

    return size;
}

static void vinput_irq(struct trapframe *tf, void *data) {
    (void)tf;

    struct vinput *vi = data;
    struct virtqueue *vq = &vi->vq;
    bool any = false;

    while (vq->last_used != vq->used->idx) {
        uint16_t id = vq->used->ring[vq->last_used % vq->size].id;

        __asm__ volatile ("" ::: "memory");

        if (id < vi->nbuf) {
            struct virtio_input_event e = vi->events[id];

            input_event(&vi->dev, e.type, e.code, e.value);

            /* the buffer goes back to the device */
            vq->avail->ring[vq->avail_idx % vq->size] = id;
            __asm__ volatile ("" ::: "memory");
            vq->avail->idx = ++vq->avail_idx;
            any = true;
        }

        vq->last_used++;
    }

    if (any) virtqueue_kick(vi->vdev, vq, 0);

    intc_eoi();
}

static void hex2(char *out, unsigned v) {
    out[0] = "0123456789abcdef"[(v >> 4) & 15];
    out[1] = "0123456789abcdef"[v & 15];
}

static void vinput_probe(struct pci_dev *pci, int n) {
    struct vinput *vi = kmalloc(sizeof(*vi));
    if (!vi) return;

    memset(vi, 0, sizeof(*vi));

    vi->vdev = virtio_attach(pci);
    if (!vi->vdev || virtio_negotiate(vi->vdev, 0) != 0) goto fail;

    struct input_dev *dev = &vi->dev;

    cfg_read(vi->vdev, CFG_ID_NAME, 0, dev->name, sizeof(dev->name) - 1);

    uint16_t ids[4] = { 6, 1, 1, 1 }; /* virtual bus, if the device does not say */

    cfg_read(vi->vdev, CFG_ID_DEVIDS, 0, ids, sizeof(ids));

    dev->bustype = ids[0];
    dev->vendor  = ids[1];
    dev->product = ids[2];
    dev->version = ids[3];

    cfg_read(vi->vdev, CFG_PROP_BITS, 0, dev->propbit, sizeof(dev->propbit));

    struct { unsigned ev; uint8_t *bits; unsigned size; } types[] = {
        { EV_KEY, dev->keybit, sizeof(dev->keybit) }, { EV_REL, dev->relbit, sizeof(dev->relbit) },
        { EV_ABS, dev->absbit, sizeof(dev->absbit) }, { EV_MSC, dev->mscbit, sizeof(dev->mscbit) },
        { EV_LED, dev->ledbit, sizeof(dev->ledbit) }, { EV_SW, dev->swbit, sizeof(dev->swbit) },
    };

    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (cfg_read(vi->vdev, CFG_EV_BITS, types[i].ev, types[i].bits, types[i].size)) input_set_bit(dev->evbit, types[i].ev);
    }

    for (unsigned a = 0; a <= ABS_MAX; a++) {
        if (!input_test_bit(dev->absbit, a)) continue;

        uint32_t info[5] = { 0 };

        cfg_read(vi->vdev, CFG_ABS_INFO, a, info, sizeof(info));

        dev->absinfo[a].minimum    = info[0];
        dev->absinfo[a].maximum    = info[1];
        dev->absinfo[a].fuzz       = info[2];
        dev->absinfo[a].flat       = info[3];
        dev->absinfo[a].resolution = info[4];
    }

    /* where it is, for /sys */
    strcpy(vi->parent, "devices/pci0000:00/0000:00:00.0/virtio0");
    hex2(vi->parent + strlen("devices/pci0000:00/0000:00:"), pci->slot);
    vi->parent[strlen("devices/pci0000:00/0000:00:00.0/virtio") ] = '0' + n;
    dev->parent_path = vi->parent;

    strcpy(dev->phys, "virtio0/input0");
    dev->phys[6] = '0' + n;

    /* buffers for the events: the whole queue handed to the device */
    void *page = alloc_pages(0);
    if (!page) goto fail;

    vi->events_phys = (uintptr_t)page;
    vi->events = (struct virtio_input_event *)P2V(vi->events_phys);
    memset(vi->events, 0, PAGE_SIZE);

    if (pci_msix_request(pci, 0, vinput_irq, vi) != 0) goto fail;

    bool irq;

    if (virtio_queue_setup(vi->vdev, 0, &vi->vq, 0, &irq) != 0 || !irq) goto fail;

    vi->nbuf = vi->vq.size < EVENTQ_BUFFERS ? vi->vq.size : EVENTQ_BUFFERS;

    for (unsigned i = 0; i < vi->nbuf; i++) {
        vi->vq.desc[i] = (struct vring_desc){ vi->events_phys + i * sizeof(struct virtio_input_event),
                                              sizeof(struct virtio_input_event), VRING_DESC_F_WRITE, 0 };
        vi->vq.avail->ring[i] = i;
    }

    vi->vq.avail_idx = vi->nbuf;
    __asm__ volatile ("" ::: "memory");
    vi->vq.avail->idx = vi->nbuf;

    virtio_driver_ok(vi->vdev);
    virtqueue_kick(vi->vdev, &vi->vq, 0);

    if (input_register(dev) != 0) goto fail;
    return;

fail:
    printk("virtio-input: %02x:%02x.%x unusable\n", pci->bus, pci->slot, pci->fn);
}

void virtio_input_init(void) {
    for (int i = 0; ; i++) {
        struct pci_dev *pci = pci_find(VIRTIO_VENDOR, VIRTIO_ID_INPUT, i);
        if (!pci) break;

        vinput_probe(pci, i);
    }
}
