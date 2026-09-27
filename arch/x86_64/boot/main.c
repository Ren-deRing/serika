#include <deps/limine.h>
#include <serika/boot.h>
#include <stdint.h>

bootinfo_t g_bootinfo;

#define MAX_MMAP_ENTRIES 512
#define MAX_CORE_ENTRIES 256

static mregion_t  g_mmap_storage[MAX_MMAP_ENTRIES];
static coreinfo_t g_core_storage[MAX_CORE_ENTRIES];

struct limine_mp_info;

extern void start_ap(coreinfo_t* info);
void limine_ap_entry(struct limine_mp_info* info);

extern void start_init();

static volatile struct limine_memmap_request memmap_req = { .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0 };
static volatile struct limine_framebuffer_request fb_req = { .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0 };
static volatile struct limine_executable_file_request kfile_req = { .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID, .revision = 0 };
static volatile struct limine_executable_address_request kaddr_req = { .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0 };
static volatile struct limine_hhdm_request hhdm_req = { .id = LIMINE_HHDM_REQUEST_ID, .revision = 0 };
static volatile struct limine_mp_request smp_req = { .id = LIMINE_MP_REQUEST_ID, .revision = 0 };
static volatile struct limine_rsdp_request rsdp_req = { .id = LIMINE_RSDP_REQUEST_ID, .revision = 0 };
// static volatile struct limine_module_request module_req = { .id = LIMINE_MODULE_REQUEST_ID, .revision = 0 };

mtype_t convert_memtype(uint64_t limine_type) {
    switch (limine_type) {
        case LIMINE_MEMMAP_USABLE:                 return MMAP_FREE;
        case LIMINE_MEMMAP_RESERVED:               return MMAP_RESERVED;
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return MMAP_ACPI_RECLAIMABLE;
        case LIMINE_MEMMAP_ACPI_NVS:               return MMAP_ACPI_NVS;
        case LIMINE_MEMMAP_BAD_MEMORY:             return MMAP_BAD_MEMORY;
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return MMAP_BOOTLOADER_RECLAIM;
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return MMAP_KERNEL_AND_MODULES;
        case LIMINE_MEMMAP_FRAMEBUFFER:            return MMAP_FRAMEBUFFER;
        default:                                   return MMAP_RESERVED;
    }
}

void boot_entry(void) {
    // TODO: verify

    struct limine_memmap_response *mmap = memmap_req.response;
    struct limine_framebuffer *fb = fb_req.response->framebuffers[0];
    struct limine_file *kernel_file = kfile_req.response->executable_file;
    struct limine_executable_address_response *kernel_address = kaddr_req.response;
    struct limine_mp_response *smp = smp_req.response;

    uint64_t hhdm_offset = hhdm_req.response->offset;
    uint64_t rsdp_address = (uintptr_t)rsdp_req.response->address;

    /* Memory */
    g_bootinfo.mem.length = (mmap->entry_count > MAX_MMAP_ENTRIES) ? MAX_MMAP_ENTRIES : mmap->entry_count;
    
    for (uint64_t i = 0; i < g_bootinfo.mem.length; i++) {
        g_mmap_storage[i].base   = mmap->entries[i]->base;
        g_mmap_storage[i].length = mmap->entries[i]->length;
        g_mmap_storage[i].type   = convert_memtype(mmap->entries[i]->type);

        uint64_t end_addr = mmap->entries[i]->base + mmap->entries[i]->length;
        if (end_addr > g_bootinfo.mem.max_phys_addr) {
            g_bootinfo.mem.max_phys_addr = end_addr;
        }
    }
    g_bootinfo.mmap = g_mmap_storage;
    g_bootinfo.mem.hhdm_offset = hhdm_offset;

    /* Graphics */
    g_bootinfo.fb.fb_addr = fb->address;
    g_bootinfo.fb.width   = fb->width;
    g_bootinfo.fb.height  = fb->height;
    g_bootinfo.fb.pitch   = fb->pitch;
    g_bootinfo.fb.bpp     = fb->bpp;

    /* Kernel Binery */
    g_bootinfo.kernel.phys_base = kernel_address->physical_base;
    g_bootinfo.kernel.virt_base = kernel_address->virtual_base;
    g_bootinfo.kernel.file_ptr  = kernel_file->address;
    g_bootinfo.kernel.file_size = kernel_file->size;

    /* Multi-Processor */
    if (smp_req.response) {
        g_bootinfo.smp.total_cores = (smp->cpu_count > MAX_CORE_ENTRIES) ? MAX_CORE_ENTRIES : smp->cpu_count;
        g_bootinfo.smp.bsp_hw_id   = smp->bsp_lapic_id;

        for (uint32_t i = 0; i < g_bootinfo.smp.total_cores; i++) {
            g_core_storage[i].logic_id       = i;
            g_core_storage[i].hw_id          = smp->cpus[i]->lapic_id;
            g_core_storage[i].boot_stack_ptr = (void*)smp->cpus[i]->extra_argument;
        }
        g_bootinfo.smp.cores = g_core_storage;

        for (uint32_t i = 0; i < g_bootinfo.smp.total_cores; i++) {
            if (smp->cpus[i]->lapic_id == smp->bsp_lapic_id) continue;
            smp->cpus[i]->extra_argument = (uintptr_t)&g_core_storage[i];
            __sync_synchronize();
            smp->cpus[i]->goto_address = limine_ap_entry; 
        }
    }

    /* Initrd */
    // if (module_req.response && module_req.response->module_count > 0) {
    //     struct limine_file *initrd_file = module_req.response->modules[0];
        
    //     g_bootinfo.initrd.phys_base = (uintptr_t)initrd_file->address - hhdm_offset;
    //     g_bootinfo.initrd.virt_base = (uintptr_t)initrd_file->address;
    //     g_bootinfo.initrd.size      = initrd_file->size;
    // }

    /* System Tables */
    g_bootinfo.rsdp_address = rsdp_address;

    start_init();
}

void limine_ap_entry(struct limine_mp_info* info) {
    coreinfo_t* core = (coreinfo_t*)info->extra_argument;
    start_ap(core);
}