#include <serika/errno.h>
#include <serika/intc.h>
#include <serika/irq.h>
#include <serika/lock.h>
#include <serika/page.h>
#include <serika/pci.h>
#include <serika/pmap.h>
#include <serika/printk.h>

#include <asm/io.h>

#define CONFIG_ADDRESS 0xCF8
#define CONFIG_DATA    0xCFC

#define PCI_MAX 64 /* devices kept */

static struct pci_dev devs[PCI_MAX];
static int            ndevs;
static spinlock_t     config_lock = SPINLOCK_INITIALIZER; /* the address and data ports are a pair */

static uint32_t config_addr(uint8_t bus, uint8_t slot, uint8_t fn, uint16_t off) {
    return 0x80000000U | (uint32_t)bus << 16 | (uint32_t)slot << 11 | (uint32_t)fn << 8 | (off & 0xfc);
}

static uint32_t cfg_read32(uint8_t bus, uint8_t slot, uint8_t fn, uint16_t off) {
    uint64_t flags = spin_lock_irqsave(&config_lock);

    outl(CONFIG_ADDRESS, config_addr(bus, slot, fn, off));
    uint32_t val = inl(CONFIG_DATA);

    spin_unlock_irqrestore(&config_lock, flags);
    return val;
}

static void cfg_write32(uint8_t bus, uint8_t slot, uint8_t fn, uint16_t off, uint32_t val) {
    uint64_t flags = spin_lock_irqsave(&config_lock);

    outl(CONFIG_ADDRESS, config_addr(bus, slot, fn, off));
    outl(CONFIG_DATA, val);

    spin_unlock_irqrestore(&config_lock, flags);
}

uint32_t pci_read32(const struct pci_dev *d, uint16_t off) {
    return cfg_read32(d->bus, d->slot, d->fn, off);
}

uint16_t pci_read16(const struct pci_dev *d, uint16_t off) {
    return pci_read32(d, off) >> ((off & 2) * 8);
}

uint8_t pci_read8(const struct pci_dev *d, uint16_t off) {
    return pci_read32(d, off) >> ((off & 3) * 8);
}

void pci_write32(const struct pci_dev *d, uint16_t off, uint32_t val) {
    cfg_write32(d->bus, d->slot, d->fn, off, val);
}

void pci_write16(const struct pci_dev *d, uint16_t off, uint16_t val) {
    unsigned shift = (off & 2) * 8;
    uint32_t old = pci_read32(d, off & ~3);

    pci_write32(d, off & ~3, (old & ~(0xffffU << shift)) | (uint32_t)val << shift);
}

static void scan_bus(uint8_t bus);

static void scan_function(uint8_t bus, uint8_t slot, uint8_t fn) {
    uint32_t id = cfg_read32(bus, slot, fn, PCI_VENDOR_ID);
    if ((id & 0xffff) == 0xffff) return;

    uint32_t class = cfg_read32(bus, slot, fn, 0x08);

    if (ndevs < PCI_MAX) {
        struct pci_dev *d = &devs[ndevs++];

        d->bus      = bus;
        d->slot     = slot;
        d->fn       = fn;
        d->vendor   = id & 0xffff;
        d->device   = id >> 16;
        d->class    = class >> 24;
        d->subclass = class >> 16;
        d->prog_if  = class >> 8;

        printk("pci: %02x:%02x.%x %04x:%04x class %02x%02x\n", bus, slot, fn, d->vendor, d->device, d->class,
               d->subclass);
    }

    /* a bridge to another bus */
    if ((class >> 24) == 0x06 && ((class >> 16) & 0xff) == 0x04) {
        uint8_t secondary = (cfg_read32(bus, slot, fn, 0x18) >> 8) & 0xff;

        if (secondary > bus) scan_bus(secondary); /* not numbered by the firmware: nothing behind it we can reach */
    }
}

static void scan_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        if ((cfg_read32(bus, slot, 0, PCI_VENDOR_ID) & 0xffff) == 0xffff) continue;

        bool multi = (cfg_read32(bus, slot, 0, 0x0c) >> 16) & 0x80;

        for (uint8_t fn = 0; fn < (multi ? 8 : 1); fn++) scan_function(bus, slot, fn);
    }
}

void pci_init(void) {
    scan_bus(0);
}

struct pci_dev *pci_find(uint16_t vendor, uint16_t device, int nth) {
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].vendor != vendor || devs[i].device != device) continue;

        if (nth-- == 0) return &devs[i];
    }

    return NULL;
}

uint8_t pci_find_cap(const struct pci_dev *d, uint8_t id, uint8_t prev) {
    if (!(pci_read16(d, 0x06) & 0x10)) return 0; /* status: has a capability list */

    uint8_t at = prev ? pci_read8(d, prev + 1) : pci_read8(d, PCI_CAP_PTR);

    /* the list is in the device: a loop in it must not hang us */
    for (int guard = 0; at >= 0x40 && guard < 48; guard++) {
        if (pci_read8(d, at) == id) return at;

        at = pci_read8(d, at + 1);
    }

    return 0;
}

void pci_enable(struct pci_dev *d) {
    pci_write16(d, PCI_COMMAND, pci_read16(d, PCI_COMMAND) | PCI_CMD_MEMORY | PCI_CMD_MASTER);
}

volatile uint8_t *pci_map_bar(struct pci_dev *d, int bar) {
    if (bar < 0 || bar >= PCI_BARS) return NULL;
    if (d->bar[bar]) return d->bar[bar];

    uint16_t reg = 0x10 + 4 * bar;
    uint32_t low = pci_read32(d, reg);

    if (low & 1) return NULL; /* io space */

    bool wide = ((low >> 1) & 3) == 2;
    uint64_t phys = low & ~0xfUL;

    if (wide) {
        if (bar + 1 >= PCI_BARS) return NULL;

        phys |= (uint64_t)pci_read32(d, reg + 4) << 32;
    }

    if (!phys) return NULL; /* the firmware gave it nothing */

    /* the size is what the bits that stick are: with the decode off meanwhile */
    uint16_t cmd = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, cmd & ~PCI_CMD_MEMORY);

    pci_write32(d, reg, ~0U);
    uint64_t mask = pci_read32(d, reg) & ~0xfUL;
    pci_write32(d, reg, low);

    if (wide) {
        uint32_t high = pci_read32(d, reg + 4);

        pci_write32(d, reg + 4, ~0U);
        mask |= (uint64_t)pci_read32(d, reg + 4) << 32;
        pci_write32(d, reg + 4, high);
    } else {
        mask |= 0xffffffff00000000UL;
    }

    pci_write16(d, PCI_COMMAND, cmd);

    uint64_t size = ~mask + 1;
    if (!size || size > (1UL << 30)) return NULL;

    size = ALIGN_UP(size, PAGE_SIZE);

    if (pmap_map(pmap_kernel(), P2V(phys), phys, size, PROT_READ | PROT_WRITE | PROT_UC) != 0) return NULL;

    d->bar[bar] = (volatile uint8_t *)P2V(phys);
    return d->bar[bar];
}

#define MSIX_CONTROL  2 /* from the capability */
#define MSIX_TABLE    4

#define MSIX_ENABLE   0x8000
#define MSIX_FUNC_MASK 0x4000

int pci_msix_request(struct pci_dev *d, unsigned entry, irq_handler_t handler, void *data) {
    uint8_t cap = pci_find_cap(d, PCI_CAP_MSIX, 0);
    if (!cap) return -EOPNOTSUPP;

    uint16_t control = pci_read16(d, cap + MSIX_CONTROL);
    if (entry > (control & 0x7ff)) return -EINVAL;

    if (!d->msix_table) {
        uint32_t table = pci_read32(d, cap + MSIX_TABLE);
        volatile uint8_t *bar = pci_map_bar(d, table & 7);
        if (!bar) return -ENOMEM;

        d->msix_table = (volatile uint32_t *)(bar + (table & ~7U));

        /* on, with everything masked until an entry is set up */
        pci_write16(d, cap + MSIX_CONTROL, control | MSIX_ENABLE | MSIX_FUNC_MASK);
    }

    uint64_t addr;
    uint32_t msg;

    int err = irq_request_msi(handler, data, &addr, &msg);
    if (err) return err;

    volatile uint32_t *e = d->msix_table + 4 * entry;

    e[0] = (uint32_t)addr;
    e[1] = addr >> 32;
    e[2] = msg;
    e[3] = 0; /* unmasked */

    pci_write16(d, cap + MSIX_CONTROL, (control | MSIX_ENABLE) & ~MSIX_FUNC_MASK);
    return 0;
}
