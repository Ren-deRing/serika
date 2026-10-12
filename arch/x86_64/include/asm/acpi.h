#pragma once

#include <stdint.h>

typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    acpi_sdt_header_t header;
    uint64_t tables[];
} __attribute__((packed)) acpi_xsdt_t;

typedef struct {
    acpi_sdt_header_t header;
    uint32_t local_apic_address;
    uint32_t flags;
} __attribute__((packed)) acpi_madt_t;

typedef struct {
    uint8_t  type;
    uint8_t  length;
    uint8_t  processor_uid;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed)) acpi_madt_lapic_t;

typedef struct {
    uint8_t  type;
    uint8_t  length;
    uint16_t reserved;
    uint32_t processor_uid;
    uint32_t apic_id;
    uint32_t flags;
} __attribute__((packed)) acpi_madt_x2apic_t;

typedef struct {
    uint8_t  type;
    uint8_t  length;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t address;
    uint32_t gsi_base;
} __attribute__((packed)) acpi_madt_ioapic_t;

/* an isa irq that is not wired to the gsi of its number */
typedef struct {
    uint8_t  type;
    uint8_t  length;
    uint8_t  bus;
    uint8_t  source;
    uint32_t gsi;
    uint16_t flags; /* bits 0-1 polarity, 2-3 trigger. 0: the bus's */
} __attribute__((packed)) acpi_madt_iso_t;

#define ACPI_MADT_TYPE_IOAPIC       1
#define ACPI_MADT_TYPE_ISO          2
#define ACPI_MADT_TYPE_LOCAL_APIC   0
#define ACPI_MADT_TYPE_LOCAL_X2APIC 9
#define ACPI_MADT_ENTRY_ENABLED     (1u << 0)

void acpi_init(void);
acpi_sdt_header_t *acpi_find_table(const char signature[4]);
