#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/intc.h>
#include <serika/net.h>
#include <serika/page.h>
#include <serika/pci.h>
#include <serika/printk.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/task.h>

#include <asm/arch.h>

/* e1000e: the Intel 82574L gigabit controller (QEMU's "e1000e"). one queue pair of 256 legacy descriptors each, a buffer of
 * 2 KiB to a descriptor, and an interrupt as a message (MSI). the rings are not touched by the hardware beyond what the
 * descriptors say: the driver copies a frame out of the receive buffer into an nbuf and a frame into the transmit buffer */

#define E1000E_VENDOR 0x8086
#define E1000E_DEVICE 0x10d3

#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_ICR     0x00c0
#define REG_IMS     0x00d0
#define REG_IMC     0x00d8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_RXDCTL  0x2828
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_TXDCTL  0x3828
#define REG_MTA     0x5200
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

#define CTRL_ASDE   (1U << 5)
#define CTRL_SLU    (1U << 6)
#define CTRL_RST    (1U << 26)

#define STATUS_LU   (1U << 1)

#define INT_TXDW    (1U << 0)
#define INT_LSC     (1U << 2)
#define INT_RXDMT0  (1U << 4)
#define INT_RXO     (1U << 6)
#define INT_RXT0    (1U << 7)

#define RCTL_EN     (1U << 1)
#define RCTL_BAM    (1U << 15)        /* broadcast */
#define RCTL_SECRC  (1U << 26)        /* the checksum is taken off. the buffer is 2 KiB with the other bits 0 */

#define TCTL_EN     (1U << 1)
#define TCTL_PSP    (1U << 3)
#define TCTL_CT     (0xfU << 4)
#define TCTL_COLD   (0x40U << 12)

#define TIPG_VALUE  0x0060200a

#define XDCTL_GRAN  (1U << 24)        /* in descriptors */
#define XDCTL_ENABLE (1U << 25)
#define XDCTL_WTHRESH1 (1U << 16)

#define RAH_AV      (1U << 31)

#define RING        256
#define BUF_SIZE    2048
#define RING_PAGES  (RING * BUF_SIZE / PAGE_SIZE)   /* the buffers of a ring */
#define RING_ORDER  7                                /* ... 128 pages */

/* legacy descriptors */
struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

#define DESC_DD    (1U << 0)
#define RX_EOP     (1U << 1)
#define TX_EOP     (1U << 0)
#define TX_IFCS    (1U << 1)
#define TX_RS      (1U << 3)

struct e1000e {
    volatile uint8_t *regs;
    struct netdev     dev;

    struct rx_desc *rx;               /* the rings and buffers, in the direct map */
    struct tx_desc *tx;
    uint8_t        *rx_buf, *tx_buf;
    uintptr_t       rx_phys, tx_phys; /* of the descriptors */
    uintptr_t       rx_buf_phys, tx_buf_phys;

    uint32_t        rx_next;          /* the next descriptor the hardware completes */
    uint32_t        tx_tail;          /* the next to give it */
};

static inline uint32_t rd(struct e1000e *e, unsigned off) {
    return *(volatile uint32_t *)(e->regs + off);
}

static inline void wr(struct e1000e *e, unsigned off, uint32_t val) {
    *(volatile uint32_t *)(e->regs + off) = val;
}

static void e1000e_irq(struct trapframe *tf, void *data) {
    (void)tf;

    struct e1000e *e = data;

    rd(e, REG_ICR);                   /* what caused it: read to clear. poll() looks at the rings and the link itself */
    net_schedule(&e->dev);
    intc_eoi();
}

static void e1000e_poll(struct netdev *dev) {
    struct e1000e *e = dev->priv;

    if (rd(e, REG_STATUS) & STATUS_LU) dev->flags |= NETDEV_LINK;
    else dev->flags &= ~NETDEV_LINK;

    for (;;) {
        volatile struct rx_desc *d = &e->rx[e->rx_next];

        if (!(d->status & DESC_DD)) break;

        __asm__ volatile ("" ::: "memory");

        /* a frame is one descriptor (the buffer holds the longest there is), whole and without an error */
        if ((d->status & RX_EOP) && !d->errors && d->length <= dev->mtu + ETH_HLEN) {
            struct nbuf *n = nbuf_alloc();

            if (n) {
                memcpy(nbuf_put(n, d->length), e->rx_buf + e->rx_next * BUF_SIZE, d->length);
                net_rx(dev, n);
            }
        }

        d->status = 0;
        wr(e, REG_RDT, e->rx_next);
        e->rx_next = (e->rx_next + 1) % RING;
    }
}

static int e1000e_xmit(struct netdev *dev, struct nbuf *n) {
    struct e1000e *e = dev->priv;
    volatile struct tx_desc *d = &e->tx[e->tx_tail];
    int err = 0;

    if (!(d->status & DESC_DD)) {
        err = -ENOBUFS;                /* the ring is full: the frame is dropped */
    } else if (n->len > BUF_SIZE) {
        err = -EMSGSIZE;
    } else {
        uint32_t len = n->len < 60 ? 60 : n->len;      /* the least a frame is, without the checksum */
        uint8_t *buf = e->tx_buf + e->tx_tail * BUF_SIZE;

        memcpy(buf, n->data, n->len);
        memset(buf + n->len, 0, len - n->len);

        d->length = len;
        d->cmd    = TX_EOP | TX_IFCS | TX_RS;
        d->status = 0;

        __asm__ volatile ("mfence" ::: "memory");

        e->tx_tail = (e->tx_tail + 1) % RING;
        wr(e, REG_TDT, e->tx_tail);
    }

    nbuf_free(n);
    return err;
}

/* wait (busy, up to a second) until the bit of a register is clear */
static bool wait_clear(struct e1000e *e, unsigned off, uint32_t bit) {
    uint64_t limit = rdtsc() + this_cpu_read(tsc_freq_hz);

    while (rd(e, off) & bit) {
        if (rdtsc() > limit) return false;

        __asm__ volatile ("pause");
    }

    return true;
}

static void delay_ms(uint32_t ms) {
    uint64_t limit = rdtsc() + (uint64_t)ms * this_cpu_read(tsc_freq_hz) / 1000;

    while (rdtsc() < limit) __asm__ volatile ("pause");
}

static void e1000e_probe(struct pci_dev *pci) {
    static int next;

    pci_enable(pci);

    struct e1000e *e = kmalloc(sizeof(*e));
    if (!e) goto fail;

    memset(e, 0, sizeof(*e));

    e->regs = pci_map_bar(pci, 0);
    if (!e->regs) goto fail_free;

    /* a reset, with every interrupt off */
    wr(e, REG_IMC, ~0U);
    wr(e, REG_RCTL, 0);
    wr(e, REG_TCTL, 0);
    wr(e, REG_CTRL, rd(e, REG_CTRL) | CTRL_RST);
    delay_ms(10);

    if (!wait_clear(e, REG_CTRL, CTRL_RST)) goto fail_free;

    wr(e, REG_IMC, ~0U);
    rd(e, REG_ICR);

    /* the address the controller has (the NVM gave it, and QEMU is told it) */
    uint32_t ral = rd(e, REG_RAL0), rah = rd(e, REG_RAH0);

    e->dev.mac[0] = ral;
    e->dev.mac[1] = ral >> 8;
    e->dev.mac[2] = ral >> 16;
    e->dev.mac[3] = ral >> 24;
    e->dev.mac[4] = rah;
    e->dev.mac[5] = rah >> 8;

    if (!(ral | (rah & 0xffff))) {
        static const uint8_t fallback[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

        memcpy(e->dev.mac, fallback, 6);
        wr(e, REG_RAL0, e->dev.mac[0] | e->dev.mac[1] << 8 | e->dev.mac[2] << 16 | (uint32_t)e->dev.mac[3] << 24);
        wr(e, REG_RAH0, e->dev.mac[4] | e->dev.mac[5] << 8);
    }

    wr(e, REG_RAH0, rd(e, REG_RAH0) | RAH_AV);

    for (unsigned i = 0; i < 128; i++) wr(e, REG_MTA + i * 4, 0);

    /* the rings: a page of descriptors each, and the buffers behind them */
    void *rx = alloc_pages(0), *tx = alloc_pages(0);
    void *rxb = alloc_pages(RING_ORDER), *txb = alloc_pages(RING_ORDER);

    if (!rx || !tx || !rxb || !txb) {
        if (rx) free_pages(rx, 0);
        if (tx) free_pages(tx, 0);
        if (rxb) free_pages(rxb, RING_ORDER);
        if (txb) free_pages(txb, RING_ORDER);
        goto fail_free;
    }

    e->rx_phys = (uintptr_t)rx;       e->rx = (struct rx_desc *)P2V(e->rx_phys);
    e->tx_phys = (uintptr_t)tx;       e->tx = (struct tx_desc *)P2V(e->tx_phys);
    e->rx_buf_phys = (uintptr_t)rxb;  e->rx_buf = (uint8_t *)P2V(e->rx_buf_phys);
    e->tx_buf_phys = (uintptr_t)txb;  e->tx_buf = (uint8_t *)P2V(e->tx_buf_phys);

    memset(e->rx, 0, PAGE_SIZE);
    memset(e->tx, 0, PAGE_SIZE);

    for (unsigned i = 0; i < RING; i++) {
        e->rx[i].addr   = e->rx_buf_phys + i * BUF_SIZE;
        e->tx[i].addr   = e->tx_buf_phys + i * BUF_SIZE;
        e->tx[i].status = DESC_DD;                     /* free */
    }

    wr(e, REG_RDBAL, e->rx_phys);
    wr(e, REG_RDBAH, e->rx_phys >> 32);
    wr(e, REG_RDLEN, RING * sizeof(struct rx_desc));
    wr(e, REG_RDH, 0);
    wr(e, REG_RDT, RING - 1);
    wr(e, REG_RXDCTL, XDCTL_GRAN | XDCTL_ENABLE);

    wr(e, REG_TDBAL, e->tx_phys);
    wr(e, REG_TDBAH, e->tx_phys >> 32);
    wr(e, REG_TDLEN, RING * sizeof(struct tx_desc));
    wr(e, REG_TDH, 0);
    wr(e, REG_TDT, 0);
    wr(e, REG_TXDCTL, XDCTL_GRAN | XDCTL_ENABLE | XDCTL_WTHRESH1);

    wr(e, REG_TIPG, TIPG_VALUE);
    wr(e, REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    wr(e, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    wr(e, REG_CTRL, rd(e, REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    strcpy(e->dev.name, "eth0");
    e->dev.name[3] += next++;

    e->dev.mtu   = 1500;
    e->dev.priv  = e;
    e->dev.xmit  = e1000e_xmit;
    e->dev.poll  = e1000e_poll;

    /* the handler is there before the controller can raise it */
    bool irq = pci_msi_request(pci, e1000e_irq, e) == 0;

    if (!irq) {
        printk("e1000e: no msi\n");
        goto fail_free;
    }

    wr(e, REG_IMS, INT_RXT0 | INT_RXDMT0 | INT_RXO | INT_LSC | INT_TXDW);

    netdev_register(&e->dev);
    net_schedule(&e->dev);            /* the link */

    printk("e1000e: %s %02x:%02x:%02x:%02x:%02x:%02x, msi\n", e->dev.name, e->dev.mac[0], e->dev.mac[1], e->dev.mac[2],
           e->dev.mac[3], e->dev.mac[4], e->dev.mac[5]);
    return;

fail_free:
    kfree(e);
fail:
    printk("e1000e: %02x:%02x.%x unusable\n", pci->bus, pci->slot, pci->fn);
}

void e1000e_init(void) {
    for (int i = 0; ; i++) {
        struct pci_dev *pci = pci_find(E1000E_VENDOR, E1000E_DEVICE, i);
        if (!pci) break;

        e1000e_probe(pci);
    }
}
