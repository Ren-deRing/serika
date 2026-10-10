#include <serika/boot.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/pmap.h>

#include <asm/arch.h>

/* 2MiB pages where aligned, 4KiB at the edges */
static int map_hhdm(struct pmap *pmap, uintptr_t phys, size_t size, uint32_t flags) {
    uintptr_t end = phys + size;
    uintptr_t huge_start = ALIGN_UP(phys, PAGE_SIZE_HUGE);
    uintptr_t huge_end = ALIGN_DOWN(end, PAGE_SIZE_HUGE);

    if (huge_start >= huge_end) return pmap_map(pmap, P2V(phys), phys, size, flags);

    if (huge_start > phys &&
        pmap_map(pmap, P2V(phys), phys, huge_start - phys, flags) != 0) return -1;

    if (pmap_map(pmap, P2V(huge_start), huge_start, huge_end - huge_start, flags | PROT_HUGE) != 0) return -1;

    if (end > huge_end &&
        pmap_map(pmap, P2V(huge_end), huge_end, end - huge_end, flags) != 0) return -1;

    return 0;
}

static void map_image(struct pmap *pmap, uintptr_t start, uintptr_t end, uint32_t flags) {
    uintptr_t phys = g_bootinfo.kernel.phys_base + (start - g_bootinfo.kernel.virt_base);

    if (pmap_map(pmap, start, phys, end - start, flags) != 0)
        panic("kmap: unable to map kernel image 0x%lx-0x%lx -- out of memory?", start, end);
}

static void check_mapped(const char *what, uintptr_t va) {
    if (!pmap_extract(pmap_kernel(), va))
        panic("kmap: %s (0x%lx) is not mapped in the kernel pmap -- check kmap_init()", what, va);
}

/* after pmap_init() */
void kmap_init(void) {
    extern char __rodata_start[], __data_start[], __bss_end[];

    struct pmap *pmap = pmap_kernel();

    /* kernel image */
    map_image(pmap, g_bootinfo.kernel.virt_base, (uintptr_t)__rodata_start, PROT_READ | PROT_EXEC);
    map_image(pmap, (uintptr_t)__rodata_start, (uintptr_t)__data_start, PROT_READ);
    map_image(pmap, (uintptr_t)__data_start, ALIGN_UP((uintptr_t)__bss_end, PAGE_SIZE), PROT_READ | PROT_WRITE);

    /* direct map */
    for (uint64_t i = 0; i < g_bootinfo.mem.length; i++) {
        struct mregion *r = &g_bootinfo.mmap[i];
        uint32_t flags = PROT_READ | PROT_WRITE;

        switch (r->type) {
            case MMAP_FRAMEBUFFER: flags |= PROT_WC; break;
            case MMAP_FREE:
            case MMAP_ACPI_RECLAIMABLE:
            case MMAP_ACPI_NVS:
            case MMAP_BOOTLOADER_RECLAIM: break;
            default: continue;
        }

        if (map_hhdm(pmap, r->base, r->length, flags) != 0)
            panic("kmap: unable to map memory 0x%lx+0x%lx -- out of memory?", r->base, r->length);
    }

    check_mapped("kernel text", (uintptr_t)kmap_init);
    check_mapped("kernel data", (uintptr_t)&g_bootinfo);
    check_mapped("kernel bss end", (uintptr_t)__bss_end - 1);
    check_mapped("current stack", (uintptr_t)__builtin_frame_address(0));
    check_mapped("page tables", (uintptr_t)pmap->pm_root_virt);

    pmap_activate(pmap);
    flush_tlb_all(); /* drops */
}
