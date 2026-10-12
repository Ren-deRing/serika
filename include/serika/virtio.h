#pragma once

#include <serika/pci.h>

#include <stdint.h>

#define VIRTIO_VENDOR      0x1af4
#define VIRTIO_ID_BLK_OLD  0x1001 /* transitional. it has the modern registers too */
#define VIRTIO_ID_BLK      0x1042

#define VIRTIO_F_VERSION_1 (1ULL << 32)

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2 /* the device writes it */

struct vring_desc {
    uint64_t addr; /* physical */
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

/* the rings of one queue in a page of our own, the device reads and writes it */
struct virtqueue {
    uint16_t            size;
    uint16_t            avail_idx; /* ours */
    uint16_t            last_used; /* how far we have taken */
    struct vring_desc  *desc;
    struct vring_avail *avail;
    volatile struct vring_used *used;
    volatile uint16_t  *notify;
};

struct virtio_dev {
    struct pci_dev    *pci;
    volatile uint8_t  *common;
    volatile uint8_t  *notify;
    uint32_t           notify_mult;
    volatile uint8_t  *device; /* the device-specific configuration */
};

/* resets the device and finds its registers. NULL if they are not there (a legacy-only device) */
struct virtio_dev *virtio_attach(struct pci_dev *pci);
int  virtio_negotiate(struct virtio_dev *v, uint64_t wanted); /* VERSION_1 and what of `wanted` it has. -EINVAL: refused */

/* msix_entry: the vector of the table that signals this queue. false in *irq: there is none, poll the used ring */
int  virtio_queue_setup(struct virtio_dev *v, unsigned index, struct virtqueue *vq, int msix_entry, bool *irq);
void virtio_driver_ok(struct virtio_dev *v);

void virtqueue_kick(struct virtio_dev *v, struct virtqueue *vq, unsigned index); /* the head is in avail already */

void virtio_blk_init(void); /* a block device for each virtio disk, after pci_init() */
