#include <serika/errno.h>
#include <serika/intc.h>
#include <serika/irq.h>
#include <serika/lock.h>

#define MSI_VECTOR_FIRST 0x50 /* after the io apic's, which go up from 0x30 */
#define MSI_VECTOR_LAST  0xef

#define MSI_ADDR_BASE 0xfee00000UL /* the local apics' window. the destination id is in bits 12-19 */

static uint8_t    next_vector = MSI_VECTOR_FIRST;
static spinlock_t msi_lock = SPINLOCK_INITIALIZER;

int irq_request_msi(irq_handler_t handler, void *data, uint64_t *addr, uint32_t *msg) {
    extern void register_handler(uint8_t vector, irq_handler_t handler, void *data);

    uint64_t flags = spin_lock_irqsave(&msi_lock);

    int vector = next_vector <= MSI_VECTOR_LAST ? next_vector++ : -ENOSPC;

    spin_unlock_irqrestore(&msi_lock, flags);

    if (vector < 0) return vector;

    register_handler(vector, handler, data);

    *addr = MSI_ADDR_BASE | ((uint64_t)(intc_get_id() & 0xff) << 12);
    *msg  = vector; /* fixed delivery, edge */
    return 0;
}
