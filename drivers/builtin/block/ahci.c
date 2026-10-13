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
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/task.h>
#include <serika/wait.h>

#include <asm/arch.h>

/* ahci: the sata controller of the chipset (q35 has one). a disk behind a port is read and written with commands
 * that the controller takes from a list in memory and does by dma. one command at a time, in slot 0 */

#define PCI_CLASS_STORAGE 0x01
#define PCI_SUB_SATA      0x06
#define PCI_IF_AHCI       0x01

#define AHCI_BAR 5

/* the controller */
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS  0x08
#define HBA_PI  0x0c

#define CAP_S64A (1U << 31)

#define GHC_IE   (1U << 1)
#define GHC_AE   (1U << 31)

/* a port, from 0x100 and 0x80 each */
#define PORT_BASE 0x100
#define PORT_SIZE 0x80

#define PX_CLB  0x00
#define PX_CLBU 0x04
#define PX_FB   0x08
#define PX_FBU  0x0c
#define PX_IS   0x10
#define PX_IE   0x14
#define PX_CMD  0x18
#define PX_TFD  0x20
#define PX_SIG  0x24
#define PX_SSTS 0x28
#define PX_SERR 0x30
#define PX_SACT 0x34
#define PX_CI   0x38

#define CMD_ST  (1U << 0)
#define CMD_FRE (1U << 4)
#define CMD_FR  (1U << 14)
#define CMD_CR  (1U << 15)

#define IS_DHRS  (1U << 0)       /* the device's register FIS: a command that moves no data is done */
#define IS_FATAL 0x78000000U     /* host bus, interface and task file errors */
#define IS_TFES  (1U << 30)

#define TFD_ERR  (1U << 0)
#define TFD_DRQ  (1U << 3)
#define TFD_BSY  (1U << 7)

#define SSTS_DET 0xf
#define SSTS_IPM 0xf00
#define SSTS_UP  0x103           /* a device, and the phy is up: DET 3, IPM 1 */

#define SIG_ATA  0x00000101

/* the commands */
#define ATA_READ_DMA_EXT  0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_EXT     0xea
#define ATA_IDENTIFY      0xec

/* a port gets a page: the list of commands (slot 0 is the one used), where the device's FISes land, the table of
 * the command and a place for what IDENTIFY says */
#define LIST_OFF  0x000
#define FIS_OFF   0x400
#define TABLE_OFF 0x800
#define IDENT_OFF 0xc00

struct cmd_header {
    uint32_t flags;      /* the length of the FIS in dwords, write, and the number of PRD entries in 31:16 */
    uint32_t prdbc;
    uint32_t table_lo;
    uint32_t table_hi;
    uint32_t reserved[4];
} __attribute__((packed));

#define CH_CFL(n)   (n)
#define CH_WRITE    (1U << 6)
#define CH_PRDTL(n) ((uint32_t)(n) << 16)

struct prd {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t reserved;
    uint32_t count;      /* bytes - 1; the top bit asks for an interrupt */
} __attribute__((packed));

#define PRD_MAX (4U << 20)
#define TABLE_PRD 0x80       /* in the table, after the 64 bytes of the FIS and the 16 of an ATAPI command */

struct ahci_port {
    volatile uint8_t *regs;
    struct mutex      lock;
    struct wait_queue wq;
    bool              irq;
    uint32_t          error;   /* the fatal bits of PxIS that the handler saw */

    uint8_t          *page;
    uintptr_t         page_phys;

    struct blkdev     dev;
};

struct ahci {
    volatile uint8_t *regs;
    struct ahci_port *ports[32];
};

static inline uint32_t rd(volatile uint8_t *base, unsigned off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void wr(volatile uint8_t *base, unsigned off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static void ahci_irq(struct trapframe *tf, void *data) {
    (void)tf;

    struct ahci *a = data;
    uint32_t is = rd(a->regs, HBA_IS);

    for (unsigned i = 0; i < 32; i++) {
        struct ahci_port *p = a->ports[i];
        if (!(is & (1U << i)) || !p) continue;

        uint32_t pis = rd(p->regs, PX_IS);

        wr(p->regs, PX_IS, pis);                 /* a bit of the port's is cleared first, then the controller's */
        if (pis & IS_FATAL) p->error |= pis & IS_FATAL;

        wake_up(&p->wq);
    }

    wr(a->regs, HBA_IS, is);
    intc_eoi();
}

/* wait until the bit is (or is not) set. false: it was not in time (one second) */
static bool wait_cmd(volatile uint8_t *regs, uint32_t bit, bool set) {
    uint64_t limit = rdtsc() + this_cpu_read(tsc_freq_hz);

    while (!!(rd(regs, PX_CMD) & bit) != set) {
        if (rdtsc() > limit) return false;

        __asm__ volatile ("pause");
    }

    return true;
}

/* the command engine and the FIS receive, off and on */
static bool port_stop(volatile uint8_t *regs) {
    wr(regs, PX_CMD, rd(regs, PX_CMD) & ~CMD_ST);
    if (!wait_cmd(regs, CMD_CR, false)) return false;

    wr(regs, PX_CMD, rd(regs, PX_CMD) & ~CMD_FRE);
    return wait_cmd(regs, CMD_FR, false);
}

static void port_start(volatile uint8_t *regs) {
    wr(regs, PX_SERR, ~0U);
    wr(regs, PX_IS, ~0U);

    wr(regs, PX_CMD, rd(regs, PX_CMD) | CMD_FRE);
    wr(regs, PX_CMD, rd(regs, PX_CMD) | CMD_ST);
}

static bool port_done(struct ahci_port *p) {
    return !(rd(p->regs, PX_CI) & 1) || p->error;
}

/* one command in slot 0, the port lock held. bytes at phys (0: no data). 0 or -EIO */
static int port_command(struct ahci_port *p, uint8_t command, uint64_t lba, uint32_t count, uintptr_t phys,
                        size_t bytes, bool write) {
    struct cmd_header *ch = (struct cmd_header *)(p->page + LIST_OFF);
    uint8_t *table = p->page + TABLE_OFF;
    uint8_t *fis = table;
    struct prd *prd = (struct prd *)(table + TABLE_PRD);

    /* the device must be ready: a command cannot be put to one that is busy */
    uint64_t limit = rdtsc() + this_cpu_read(tsc_freq_hz);

    while (rd(p->regs, PX_TFD) & (TFD_BSY | TFD_DRQ)) {
        if (rdtsc() > limit) return -EIO;

        __asm__ volatile ("pause");
    }

    memset(table, 0, TABLE_PRD + sizeof(*prd));

    fis[0]  = 0x27;                  /* register, host to device */
    fis[1]  = 0x80;                  /* a command, not the control register */
    fis[2]  = command;
    fis[4]  = lba;
    fis[5]  = lba >> 8;
    fis[6]  = lba >> 16;
    fis[7]  = 0x40;                  /* lba */
    fis[8]  = lba >> 24;
    fis[9]  = lba >> 32;
    fis[10] = lba >> 40;
    fis[12] = count;
    fis[13] = count >> 8;

    if (bytes) {
        prd->addr_lo = phys;
        prd->addr_hi = phys >> 32;
        prd->count   = bytes - 1;
    }

    ch->flags    = CH_CFL(5) | (write ? CH_WRITE : 0) | CH_PRDTL(bytes ? 1 : 0);
    ch->prdbc    = 0;
    ch->table_lo = p->page_phys + TABLE_OFF;
    ch->table_hi = (p->page_phys + TABLE_OFF) >> 32;

    p->error = 0;

    __asm__ volatile ("mfence" ::: "memory");
    wr(p->regs, PX_CI, 1);

    int err = 0;

    if (p->irq) {
        wait_event(p->wq, port_done(p));
    } else {
        limit = rdtsc() + 10 * this_cpu_read(tsc_freq_hz);

        while (!port_done(p)) {
            uint32_t pis = rd(p->regs, PX_IS);

            if (pis & IS_FATAL) {
                p->error = pis & IS_FATAL;
                break;
            }

            if (rdtsc() > limit) {
                err = -EIO;
                break;
            }

            __asm__ volatile ("pause");
        }

        wr(p->regs, PX_IS, rd(p->regs, PX_IS));
    }

    if (p->error || (rd(p->regs, PX_TFD) & TFD_ERR)) err = -EIO;

    if (err) {
        /* a failed command leaves the engine stopped or stuck: it is stopped and started again, which also drops the
         * command */
        printk("ahci: %s: command %02x failed (is %08x tfd %08x)\n", p->dev.name, command, p->error,
               rd(p->regs, PX_TFD));

        port_stop(p->regs);
        p->error = 0;
        port_start(p->regs);
    }

    return err;
}

static int ahci_transfer(struct blkdev *dev, uint64_t sector, void *buf, size_t count, bool write) {
    struct ahci_port *p = dev->priv;

    mutex_lock(&p->lock);
    int err = port_command(p, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, sector, count,
                           V2P((uintptr_t)buf), count * SECTOR_SIZE, write);
    mutex_unlock(&p->lock);
    return err;
}

static int ahci_read(struct blkdev *dev, uint64_t sector, void *buf, size_t count) {
    return ahci_transfer(dev, sector, buf, count, false);
}

static int ahci_write(struct blkdev *dev, uint64_t sector, const void *buf, size_t count) {
    return ahci_transfer(dev, sector, (void *)buf, count, true);
}

static int ahci_flush(struct blkdev *dev) {
    struct ahci_port *p = dev->priv;

    mutex_lock(&p->lock);
    int err = port_command(p, ATA_FLUSH_EXT, 0, 0, 0, 0, false);
    mutex_unlock(&p->lock);
    return err;
}

/* the sectors of the disk, from what IDENTIFY says. 0: not a disk we can use */
static uint64_t identify(struct ahci_port *p) {
    uintptr_t phys = p->page_phys + IDENT_OFF;
    const uint16_t *id = (const uint16_t *)(p->page + IDENT_OFF);

    memset(p->page + IDENT_OFF, 0, 512);

    if (port_command(p, ATA_IDENTIFY, 0, 0, phys, 512, false) != 0) return 0;

    if (!(id[83] & (1U << 10))) return 0;                /* no 48 bit lba */

    /* the logical sector is 512 bytes unless word 106 says it is not */
    if ((id[106] & 0xc000) == 0x4000 && (id[106] & (1U << 12))) return 0;

    return (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
}

static struct ahci_port *port_probe(volatile uint8_t *hba, unsigned n) {
    volatile uint8_t *regs = hba + PORT_BASE + n * PORT_SIZE;

    if ((rd(regs, PX_SSTS) & (SSTS_DET | SSTS_IPM)) != SSTS_UP) return NULL;
    if (rd(regs, PX_SIG) != SIG_ATA) return NULL;        /* an atapi drive, a port multiplier, ... */

    struct ahci_port *p = kmalloc(sizeof(*p));
    if (!p) return NULL;

    memset(p, 0, sizeof(*p));
    mutex_init(&p->lock);
    wait_queue_init(&p->wq);
    p->regs = regs;

    void *page = alloc_pages(0);
    if (!page) {
        kfree(p);
        return NULL;
    }

    p->page_phys = (uintptr_t)page;
    p->page      = (uint8_t *)P2V(p->page_phys);
    memset(p->page, 0, PAGE_SIZE);

    /* the firmware may have left the port running on memory of its own */
    if (!port_stop(regs)) {
        free_pages(page, 0);
        kfree(p);
        return NULL;
    }

    wr(regs, PX_CLB,  p->page_phys + LIST_OFF);
    wr(regs, PX_CLBU, (p->page_phys + LIST_OFF) >> 32);
    wr(regs, PX_FB,   p->page_phys + FIS_OFF);
    wr(regs, PX_FBU,  (p->page_phys + FIS_OFF) >> 32);
    wr(regs, PX_SACT, 0);

    port_start(regs);
    return p;
}

static void ahci_probe(struct pci_dev *pci) {
    static int next;

    pci_enable(pci);

    volatile uint8_t *hba = pci_map_bar(pci, AHCI_BAR);
    if (!hba) goto fail;

    /* the buffers are anywhere in memory, and the controller must reach them */
    if (!(rd(hba, HBA_CAP) & CAP_S64A)) goto fail;

    struct ahci *a = kmalloc(sizeof(*a));
    if (!a) goto fail;

    memset(a, 0, sizeof(*a));
    a->regs = hba;

    wr(hba, HBA_GHC, rd(hba, HBA_GHC) | GHC_AE);
    wr(hba, HBA_GHC, rd(hba, HBA_GHC) & ~GHC_IE);
    wr(hba, HBA_IS, ~0U);

    uint32_t implemented = rd(hba, HBA_PI);

    for (unsigned n = 0; n < 32; n++) {
        if (!(implemented & (1U << n))) continue;

        struct ahci_port *p = port_probe(hba, n);
        if (!p) continue;

        a->ports[n] = p;
    }

    /* the message goes to the handler before the controller can raise it */
    bool irq = pci_msi_request(pci, ahci_irq, a) == 0;

    for (unsigned n = 0; n < 32; n++) {
        struct ahci_port *p = a->ports[n];
        if (!p) continue;

        p->irq = irq;
        wr(p->regs, PX_IE, irq ? IS_DHRS | IS_FATAL : 0);
    }

    if (irq) wr(hba, HBA_GHC, rd(hba, HBA_GHC) | GHC_IE);

    for (unsigned n = 0; n < 32; n++) {
        struct ahci_port *p = a->ports[n];
        if (!p) continue;

        uint64_t sectors;

        mutex_lock(&p->lock);
        sectors = identify(p);
        mutex_unlock(&p->lock);

        if (!sectors) {
            printk("ahci: port %u: not a disk we can use\n", n);
            a->ports[n] = NULL;
            continue;
        }

        p->dev.sectors = sectors;
        p->dev.priv    = p;
        p->dev.read    = ahci_read;
        p->dev.write   = ahci_write;
        p->dev.flush   = ahci_flush;

        strcpy(p->dev.name, "sda");
        p->dev.name[2] += next++;

        if (blk_register(&p->dev) != 0) {
            a->ports[n] = NULL;
            continue;
        }

        printk("ahci: %s %lu sectors (%lu MiB), %s\n", p->dev.name, sectors, sectors >> 11, irq ? "msi" : "polled");
    }

    return;

fail:
    printk("ahci: %02x:%02x.%x unusable\n", pci->bus, pci->slot, pci->fn);
}

void ahci_init(void) {
    for (int i = 0; ; i++) {
        struct pci_dev *pci = pci_find_class(PCI_CLASS_STORAGE, PCI_SUB_SATA, PCI_IF_AHCI, i);
        if (!pci) break;

        ahci_probe(pci);
    }
}
