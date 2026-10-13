#include <serika/block.h>
#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/intc.h>
#include <serika/mutex.h>
#include <serika/page.h>
#include <serika/pci.h>
#include <serika/preempt.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/task.h>
#include <serika/virtio.h>
#include <serika/wait.h>

#include <asm/arch.h>

#define VIRTIO_BLK_T_IN 0

#define STATUS_PENDING 0xff /* the device writes 0 for done */

/* the first part of a request: what the device reads, then (at +16) the status it writes */
struct blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

#define REQ_STATUS_OFF 16

/* one request at a time: a disk read is the whole wait, and the queue of the device is not the bottleneck here */
struct vblk {
    struct virtio_dev *vdev;
    struct virtqueue   vq;
    struct mutex       lock;
    struct wait_queue  wq;
    bool               irq;

    uint8_t           *req;      /* a page: header and status */
    uintptr_t          req_phys;

    struct blkdev      dev;
};

static void vblk_irq(struct trapframe *tf, void *data) {
    (void)tf;

    struct vblk *b = data;

    wake_up(&b->wq);
    intc_eoi();
}

static bool vblk_done(struct vblk *b) {
    return b->vq.used->idx != b->vq.last_used;
}

static int vblk_read(struct blkdev *dev, uint64_t sector, void *buf, size_t count) {
    struct vblk *b = dev->priv;
    struct virtqueue *vq = &b->vq;

    mutex_lock(&b->lock);

    struct blk_req_hdr *hdr = (struct blk_req_hdr *)b->req;
    volatile uint8_t *status = b->req + REQ_STATUS_OFF;

    hdr->type     = VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector   = sector;
    *status       = STATUS_PENDING;

    vq->desc[0] = (struct vring_desc){ b->req_phys, sizeof(*hdr), VRING_DESC_F_NEXT, 1 };
    vq->desc[1] = (struct vring_desc){ V2P((uintptr_t)buf), count * SECTOR_SIZE, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 2 };
    vq->desc[2] = (struct vring_desc){ b->req_phys + REQ_STATUS_OFF, 1, VRING_DESC_F_WRITE, 0 };

    vq->avail->ring[vq->avail_idx % vq->size] = 0;
    __asm__ volatile ("" ::: "memory");
    vq->avail->idx = ++vq->avail_idx;

    virtqueue_kick(b->vdev, vq, 0);

    int err = 0;

    if (b->irq) {
        wait_event(b->wq, vblk_done(b));
    } else {
        uint64_t limit = rdtsc() + 10 * this_cpu_read(tsc_freq_hz);

        while (!vblk_done(b)) {
            if (rdtsc() > limit) {
                err = -EIO;
                break;
            }

            __asm__ volatile ("pause");
        }
    }

    if (!err) {
        vq->last_used++;
        __asm__ volatile ("" ::: "memory");

        if (*status != 0) err = -EIO;
    }

    mutex_unlock(&b->lock);
    return err;
}

static void vblk_probe(struct pci_dev *pci, int index) {
    struct vblk *b = kmalloc(sizeof(*b));
    if (!b) return;

    memset(b, 0, sizeof(*b));
    mutex_init(&b->lock);
    wait_queue_init(&b->wq);

    b->vdev = virtio_attach(pci);
    if (!b->vdev) goto fail;

    if (virtio_negotiate(b->vdev, 0) != 0) goto fail;

    void *page = alloc_pages(0);
    if (!page) goto fail;

    b->req_phys = (uintptr_t)page;
    b->req      = (uint8_t *)P2V(b->req_phys);

    /* msi-x if the device has it: the entry is set up before the queue can raise it */
    int entry = pci_msix_request(pci, 0, vblk_irq, b) == 0 ? 0 : -1;

    if (virtio_queue_setup(b->vdev, 0, &b->vq, entry, &b->irq) != 0) goto fail;

    virtio_driver_ok(b->vdev);

    uint32_t low = *(volatile uint32_t *)b->vdev->device, high = *(volatile uint32_t *)(b->vdev->device + 4);

    b->dev.sectors = (uint64_t)high << 32 | low;
    b->dev.priv    = b;
    b->dev.read    = vblk_read;

    strcpy(b->dev.name, "vda");
    b->dev.name[2] += index;

    if (blk_register(&b->dev) != 0) goto fail;

    printk("virtio-blk: %s %lu sectors (%lu MiB), %s\n", b->dev.name, b->dev.sectors, b->dev.sectors >> 11,
           b->irq ? "msi-x" : "polled");
    return;

fail:
    printk("virtio-blk: %02x:%02x.%x unusable\n", pci->bus, pci->slot, pci->fn);
}

void virtio_blk_init(void) {
    int n = 0;

    for (int i = 0; ; i++) {
        struct pci_dev *pci = pci_find(VIRTIO_VENDOR, VIRTIO_ID_BLK, i);
        if (!pci) break;

        vblk_probe(pci, n++);
    }

    for (int i = 0; ; i++) {
        struct pci_dev *pci = pci_find(VIRTIO_VENDOR, VIRTIO_ID_BLK_OLD, i);
        if (!pci) break;

        vblk_probe(pci, n++);
    }
}
