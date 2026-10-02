#pragma once

#include <serika/list.h>
#include <serika/slab.h>

#define PROT_NONE   0x00
#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04
#define PROT_USER   0x08
#define PROT_WC     0x10
#define PROT_GLOBAL 0x20
#define PROT_HUGE   0x40

#define PAGE_SHIFT 12
#define PAGE_SIZE  (1ULL << PAGE_SHIFT)

#define PFN_TO_PHYS(pfn)     (pfn << PAGE_SHIFT)
#define PHYS_TO_PFN(addr)    (addr >> PAGE_SHIFT)
#define BUDDY_POOL_START_PFN 0x8000ULL

#define PAGE_SHIFT_HUGE 21
#define PAGE_SIZE_HUGE  (1ULL << PAGE_SHIFT_HUGE)

#define MAX_BUDDY_ORDER 11

#define ALIGN_UP(addr, align)   (((addr) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(addr, align) ((addr) & ~((align) - 1))

#define P2V(addr) (addr + HHDM_OFFSET)
#define V2P(addr) (addr - HHDM_OFFSET)

#define HHDM_OFFSET 0xFFFF800000000000ULL

struct page {
    list_node   page_list;
    bool        is_free;

    struct slab slab;
};
