#include <serika/boot.h>
#include <serika/page.h>
#include <serika/pmap.h>
#include <serika/panic.h>
#include <serika/string.h>

#include <asm/acpi.h>

static acpi_xsdt_t *g_xsdt = NULL;

static bool validate_checksum(acpi_sdt_header_t *header) {
    uint8_t sum = 0;
    uint8_t *ptr = (uint8_t *)header;
    for (size_t i = 0; i < header->length; i++) {
        sum += ptr[i];
    }
    return sum == 0;
}

acpi_sdt_header_t *acpi_find_table(const char signature[4]) {
    if (!g_xsdt) return NULL;

    size_t entries = (g_xsdt->header.length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);

    for (size_t i = 0; i < entries; i++) {
        uintptr_t table_phys = g_xsdt->tables[i];
        uintptr_t table_virt = (uintptr_t)P2V(table_phys);
        
        acpi_sdt_header_t *header = (acpi_sdt_header_t *)table_virt;

        if (memcmp(header->signature, signature, 4) == 0) {
            if (validate_checksum(header)) {
                return header;
            }
        }
    }

    return NULL;
}

void acpi_init() {
    uintptr_t rsdp_virt = g_bootinfo.rsdp_address;
    uintptr_t rsdp_phys = (uintptr_t)V2P((void*) rsdp_virt);

    pmap_map(pmap_kernel(), rsdp_virt, rsdp_phys, sizeof(acpi_rsdp_t), PROT_READ);

    acpi_rsdp_t *rsdp = (acpi_rsdp_t *)rsdp_virt;

    uint8_t sum = 0;
    for (size_t i = 0; i < sizeof(acpi_rsdp_t); i++) {
        sum += ((uint8_t *)rsdp)[i];
    }

    if (sum != 0) panic("acpi: RSDP checksum validation failed.");

    uintptr_t xsdt_phys = rsdp->xsdt_address;
    uintptr_t xsdt_virt = (uintptr_t)P2V(xsdt_phys);

    pmap_map(pmap_kernel(), xsdt_virt, xsdt_phys, sizeof(acpi_sdt_header_t), PROT_READ);
    
    acpi_sdt_header_t *xsdt_hdr = (acpi_sdt_header_t *)xsdt_virt;
    pmap_map(pmap_kernel(), xsdt_virt, xsdt_phys, xsdt_hdr->length, PROT_READ);

    if (!validate_checksum(xsdt_hdr)) panic("acpi: XSDT checksum validation failed.");

    g_xsdt = (acpi_xsdt_t *)xsdt_virt;
}
