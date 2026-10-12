#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/page.h>
#include <serika/pci.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/virtio.h>

/* the modern (1.0) pci transport: every register is in a bar, found by the vendor capabilities */

#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_ISR    3
#define CAP_DEVICE 4

/* virtio_pci_common_cfg */
#define COMMON_DEVICE_FEATURE_SELECT 0x00
#define COMMON_DEVICE_FEATURE        0x04
#define COMMON_DRIVER_FEATURE_SELECT 0x08
#define COMMON_DRIVER_FEATURE        0x0c
#define COMMON_STATUS                0x14
#define COMMON_QUEUE_SELECT          0x16
#define COMMON_QUEUE_SIZE            0x18
#define COMMON_QUEUE_MSIX_VECTOR     0x1a
#define COMMON_QUEUE_ENABLE          0x1c
#define COMMON_QUEUE_NOTIFY_OFF      0x1e
#define COMMON_QUEUE_DESC            0x20
#define COMMON_QUEUE_DRIVER          0x28
#define COMMON_QUEUE_DEVICE          0x30

#define STATUS_ACK         1
#define STATUS_DRIVER      2
#define STATUS_DRIVER_OK   4
#define STATUS_FEATURES_OK 8

#define MSIX_NO_VECTOR 0xffff

#define QUEUE_MAX 128 /* the rings of this many fit in a page */

static uint8_t  r8(struct virtio_dev *v, unsigned off) { return *(volatile uint8_t *)(v->common + off); }
static uint16_t r16(struct virtio_dev *v, unsigned off) { return *(volatile uint16_t *)(v->common + off); }
static uint32_t r32(struct virtio_dev *v, unsigned off) { return *(volatile uint32_t *)(v->common + off); }

static void w8(struct virtio_dev *v, unsigned off, uint8_t x) { *(volatile uint8_t *)(v->common + off) = x; }
static void w16(struct virtio_dev *v, unsigned off, uint16_t x) { *(volatile uint16_t *)(v->common + off) = x; }
static void w32(struct virtio_dev *v, unsigned off, uint32_t x) { *(volatile uint32_t *)(v->common + off) = x; }

static void w64(struct virtio_dev *v, unsigned off, uint64_t x) {
    w32(v, off, (uint32_t)x);
    w32(v, off + 4, x >> 32);
}

struct virtio_dev *virtio_attach(struct pci_dev *pci) {
    static struct virtio_dev devs[4];
    static int ndevs;

    if (ndevs == 4) return NULL;

    struct virtio_dev *v = &devs[ndevs];

    memset(v, 0, sizeof(*v));
    v->pci = pci;

    pci_enable(pci);

    for (uint8_t cap = pci_find_cap(pci, PCI_CAP_VENDOR, 0); cap; cap = pci_find_cap(pci, PCI_CAP_VENDOR, cap)) {
        uint8_t type = pci_read8(pci, cap + 3), bar = pci_read8(pci, cap + 4);
        uint32_t off = pci_read32(pci, cap + 8);

        if (type != CAP_COMMON && type != CAP_NOTIFY && type != CAP_DEVICE) continue;

        volatile uint8_t *base = pci_map_bar(pci, bar);
        if (!base) return NULL;

        base += off;

        if (type == CAP_COMMON) {
            v->common = base;
        } else if (type == CAP_NOTIFY) {
            v->notify = base;
            v->notify_mult = pci_read32(pci, cap + 16);
        } else {
            v->device = base;
        }
    }

    if (!v->common || !v->notify || !v->device) return NULL;

    ndevs++;

    w8(v, COMMON_STATUS, 0);
    while (r8(v, COMMON_STATUS)) ; /* the reset is done when it reads 0 */

    w8(v, COMMON_STATUS, STATUS_ACK);
    w8(v, COMMON_STATUS, STATUS_ACK | STATUS_DRIVER);
    return v;
}

int virtio_negotiate(struct virtio_dev *v, uint64_t wanted) {
    uint64_t offered = 0;

    for (unsigned i = 0; i < 2; i++) {
        w32(v, COMMON_DEVICE_FEATURE_SELECT, i);
        offered |= (uint64_t)r32(v, COMMON_DEVICE_FEATURE) << (32 * i);
    }

    if (!(offered & VIRTIO_F_VERSION_1)) return -EINVAL;

    uint64_t features = VIRTIO_F_VERSION_1 | (wanted & offered);

    for (unsigned i = 0; i < 2; i++) {
        w32(v, COMMON_DRIVER_FEATURE_SELECT, i);
        w32(v, COMMON_DRIVER_FEATURE, features >> (32 * i));
    }

    w8(v, COMMON_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK);

    if (!(r8(v, COMMON_STATUS) & STATUS_FEATURES_OK)) return -EINVAL; /* it does not take them */
    return 0;
}

int virtio_queue_setup(struct virtio_dev *v, unsigned index, struct virtqueue *vq, int msix_entry, bool *irq) {
    w16(v, COMMON_QUEUE_SELECT, index);

    unsigned size = r16(v, COMMON_QUEUE_SIZE);
    if (!size) return -ENODEV;
    if (size > QUEUE_MAX) size = QUEUE_MAX;

    w16(v, COMMON_QUEUE_SIZE, size);

    uint8_t *page = alloc_pages(0);
    if (!page) return -ENOMEM;

    uintptr_t phys = (uintptr_t)page;
    page = (uint8_t *)P2V(phys);
    memset(page, 0, PAGE_SIZE);

    /* descriptors, then the available ring, then the used ring on a 4 byte boundary */
    unsigned avail_off = 16 * size, used_off = ALIGN_UP(avail_off + 6 + 2 * size, 4);

    vq->size      = size;
    vq->avail_idx = vq->last_used = 0;
    vq->desc      = (struct vring_desc *)page;
    vq->avail     = (struct vring_avail *)(page + avail_off);
    vq->used      = (volatile struct vring_used *)(page + used_off);

    w64(v, COMMON_QUEUE_DESC, phys);
    w64(v, COMMON_QUEUE_DRIVER, phys + avail_off);
    w64(v, COMMON_QUEUE_DEVICE, phys + used_off);

    *irq = false;

    if (msix_entry >= 0) {
        w16(v, COMMON_QUEUE_MSIX_VECTOR, msix_entry);
        *irq = r16(v, COMMON_QUEUE_MSIX_VECTOR) != MSIX_NO_VECTOR;
    }

    vq->notify = (volatile uint16_t *)(v->notify + r16(v, COMMON_QUEUE_NOTIFY_OFF) * v->notify_mult);

    w16(v, COMMON_QUEUE_ENABLE, 1);
    return 0;
}

void virtio_driver_ok(struct virtio_dev *v) {
    w8(v, COMMON_STATUS, r8(v, COMMON_STATUS) | STATUS_DRIVER_OK);
}

void virtqueue_kick(struct virtio_dev *v, struct virtqueue *vq, unsigned index) {
    (void)v;

    __asm__ volatile ("mfence" ::: "memory"); /* the descriptors and the index first */
    *vq->notify = index;
}
