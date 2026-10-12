#pragma once

#include <serika/irq.h>

#include <stdint.h>

#define PCI_VENDOR_ID    0x00
#define PCI_COMMAND      0x04
#define PCI_CAP_PTR      0x34

#define PCI_CMD_MEMORY   0x2
#define PCI_CMD_MASTER   0x4

#define PCI_CAP_VENDOR   0x09 /* virtio's configuration structures are in these */
#define PCI_CAP_MSIX     0x11

#define PCI_BARS 6

/* what a scan finds. the mappings are made when asked for */
struct pci_dev {
    uint8_t  bus, slot, fn;
    uint16_t vendor, device;
    uint8_t  class, subclass, prog_if;

    volatile uint8_t *bar[PCI_BARS]; /* pci_map_bar(): NULL until then */

    volatile uint32_t *msix_table;   /* pci_msix_request() */
};

void pci_init(void); /* after the page tables: finds every device behind bus 0 */

struct pci_dev *pci_find(uint16_t vendor, uint16_t device, int nth); /* NULL: no such one */

uint8_t  pci_read8(const struct pci_dev *d, uint16_t off);
uint16_t pci_read16(const struct pci_dev *d, uint16_t off);
uint32_t pci_read32(const struct pci_dev *d, uint16_t off);
void     pci_write16(const struct pci_dev *d, uint16_t off, uint16_t val);
void     pci_write32(const struct pci_dev *d, uint16_t off, uint32_t val);

/* the offset of the next capability with this id after `prev` (0: from the first), 0 when there is none */
uint8_t pci_find_cap(const struct pci_dev *d, uint8_t id, uint8_t prev);

void pci_enable(struct pci_dev *d); /* memory space and bus mastering */

/* a memory bar mapped uncached, all of it. NULL if it is not one or there is no room */
volatile uint8_t *pci_map_bar(struct pci_dev *d, int bar);

/* entry of the device's msi-x table to the handler. 0 or -errno (-EOPNOTSUPP: a device without msi-x) */
int pci_msix_request(struct pci_dev *d, unsigned entry, irq_handler_t handler, void *data);
