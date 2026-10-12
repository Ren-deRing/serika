#include <serika/errno.h>
#include <serika/intc.h>
#include <serika/irq.h>
#include <serika/lock.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/pmap.h>
#include <serika/printk.h>

#include <asm/acpi.h>

#define IOAPIC_MAX     4
#define IOAPIC_DEFAULT 0xFEC00000UL /* where the pc puts the first one, if the tables do not say */

#define IOREGSEL 0x00
#define IOWIN    0x10

#define IOAPIC_VER   0x01
#define IOAPIC_REDIR 0x10

#define REDIR_POL_LOW (1U << 13)
#define REDIR_LEVEL   (1U << 15)

#define IRQ_VECTOR_BASE 0x30 /* a gsi's vector is this plus its number */

struct ioapic {
    volatile uint32_t *regs;
    uint32_t           gsi_base;
    uint32_t           nr; /* redirection entries */
};

static struct ioapic ioapics[IOAPIC_MAX];
static int           nr_ioapics;
static spinlock_t    ioapic_lock = SPINLOCK_INITIALIZER; /* regsel and win are a pair */

/* isa irqs 0-15 that moved */
static struct {
    uint32_t gsi;
    uint16_t flags;
} iso[16];

static uint32_t ioapic_read(struct ioapic *io, uint32_t reg) {
    io->regs[IOREGSEL / 4] = reg;
    return io->regs[IOWIN / 4];
}

static void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t val) {
    io->regs[IOREGSEL / 4] = reg;
    io->regs[IOWIN / 4] = val;
}

static void ioapic_add(uintptr_t phys, uint32_t gsi_base) {
    if (nr_ioapics == IOAPIC_MAX) return;

    if (pmap_map(pmap_kernel(), P2V(phys), phys, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_UC) != 0)
        panic("ioapic: unable to map 0x%lx -- out of memory?", phys);

    struct ioapic *io = &ioapics[nr_ioapics++];

    io->regs     = (volatile uint32_t *)P2V(phys);
    io->gsi_base = gsi_base;
    io->nr       = ((ioapic_read(io, IOAPIC_VER) >> 16) & 0xff) + 1;
}

void ioapic_init(void) {
    for (int i = 0; i < 16; i++) iso[i].gsi = i;

    acpi_madt_t *madt = (acpi_madt_t *)acpi_find_table("APIC");

    if (madt) {
        const uint8_t *p = (const uint8_t *)(madt + 1);
        const uint8_t *end = (const uint8_t *)madt + madt->header.length;

        while (p + 2 <= end && p[1] >= 2) {
            if (p[0] == ACPI_MADT_TYPE_IOAPIC) {
                const acpi_madt_ioapic_t *e = (const acpi_madt_ioapic_t *)p;
                ioapic_add(e->address, e->gsi_base);
            } else if (p[0] == ACPI_MADT_TYPE_ISO) {
                const acpi_madt_iso_t *e = (const acpi_madt_iso_t *)p;

                if (e->bus == 0 && e->source < 16) {
                    iso[e->source].gsi = e->gsi;
                    iso[e->source].flags = e->flags;
                }
            }

            p += p[1];
        }
    }

    if (!nr_ioapics) {
        printk("ioapic: none in the tables -- assuming one at 0x%lx\n", IOAPIC_DEFAULT);
        ioapic_add(IOAPIC_DEFAULT, 0);
    }

    for (int i = 0; i < nr_ioapics; i++)
        printk("ioapic: %d at gsi %u, %u entries\n", i, ioapics[i].gsi_base, ioapics[i].nr);
}

/* an isa irq: edge and active high unless the tables say otherwise */
int irq_request(uint32_t isa_irq, irq_handler_t handler, void *data) {
    extern void register_handler(uint8_t vector, irq_handler_t handler, void *data);

    if (isa_irq >= 16) return -EINVAL;

    uint32_t gsi = iso[isa_irq].gsi;
    uint16_t polarity = iso[isa_irq].flags & 3;
    uint16_t trigger = (iso[isa_irq].flags >> 2) & 3;

    struct ioapic *io = NULL;

    for (int i = 0; i < nr_ioapics; i++)
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].nr) io = &ioapics[i];

    if (!io) return -ENODEV;

    uint8_t vector = IRQ_VECTOR_BASE + gsi;
    register_handler(vector, handler, data);

    uint32_t low = vector;
    if (polarity == 3) low |= REDIR_POL_LOW;
    if (trigger == 3)  low |= REDIR_LEVEL;

    uint32_t entry = IOAPIC_REDIR + 2 * (gsi - io->gsi_base);

    uint64_t flags = spin_lock_irqsave(&ioapic_lock);

    ioapic_write(io, entry + 1, (intc_get_id() & 0xff) << 24); /* physical, to this cpu */
    ioapic_write(io, entry, low);                              /* and unmasked */

    spin_unlock_irqrestore(&ioapic_lock, flags);
    return 0;
}
