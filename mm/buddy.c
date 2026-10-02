#include <serika/boot.h>
#include <serika/buddy.h>
#include <serika/list.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/string.h>

#include <stdint.h>

typedef struct {
    list_node free_list;
    size_t    free_count;
} buddy_order;

typedef struct {
    buddy_order  orders[MAX_BUDDY_ORDER];
    struct page *mmap;
    uintptr_t    mmap_phys;
    size_t       mmap_size;
    size_t       total_pages;
    uintptr_t    mem_start;
    uintptr_t    mem_end;
    size_t       free_pages;
} buddy_t;


buddy_t g_buddy;

struct page* pfn_to_page(size_t pfn) {
    return &g_buddy.mmap[pfn];
}

size_t page_to_pfn(struct page* pg) {
    return (size_t)(pg - g_buddy.mmap);
}

void* alloc_pages(int order) {
    if (order < 0 || order >= MAX_BUDDY_ORDER) return NULL;

    /* Find free page */
    for (int curr_order = order; curr_order < MAX_BUDDY_ORDER; curr_order++) {
        if (list_empty(&g_buddy.orders[curr_order].free_list)) continue;
        /* Free page is found */

        /* Delete from freelist */
        list_node* node = g_buddy.orders[curr_order].free_list.next;
        list_del(node);
        g_buddy.orders[curr_order].free_count--;

        size_t pfn = page_to_pfn((struct page*)node);

        /* Divide into buddies until the target order is reached */
        while (curr_order > order) {
            curr_order--;

            /* Find buddy */
            size_t buddy_pfn = pfn ^ (1ULL << curr_order);
            struct page* buddy = pfn_to_page(buddy_pfn);

            /* Set buddy to available (free) */
            buddy->is_free = true;
            list_add(&buddy->page_list, &g_buddy.orders[curr_order].free_list);
            g_buddy.orders[curr_order].free_count++;
        }

        /* Set page to in use */
        struct page* page = pfn_to_page(pfn);
        page->is_free = false;

        g_buddy.free_pages -= (1ULL << order);

        return (void*)PFN_TO_PHYS(pfn);
    }

    /* OOM */
    return NULL;
}

void free_pages(void* addr, int order) {
    if (addr == NULL || order < 0 || order >= MAX_BUDDY_ORDER) return;

    /* Find free target page's pfn */
    size_t pfn = PHYS_TO_PFN((uintptr_t)addr);
    int curr_order = order;

    /* Merge until no buddies left to merge with */
    while (curr_order < MAX_BUDDY_ORDER - 1) {
        /* Find buddy */
        size_t buddy_pfn = pfn ^ (1ULL << curr_order);
        struct page* buddy = pfn_to_page(buddy_pfn);

        /* Buddy is in use; cannot merge */
        if (!buddy->is_free) break;

        /* Delete buddy */
        list_del(&buddy->page_list);
        buddy->is_free = false;
        g_buddy.orders[curr_order].free_count--;

        pfn = pfn & buddy_pfn;
        curr_order++;
    }

    /* Set final page to available (free) */
    struct page* page = pfn_to_page(pfn);
    page->is_free = true;
    list_add_tail(&page->page_list, &g_buddy.orders[curr_order].free_list);
    g_buddy.orders[curr_order].free_count++;

    g_buddy.free_pages += (1ULL << order);
}

void buddy_init() {
    g_buddy.free_pages = 0;
    
    for (int i = 0; i < MAX_BUDDY_ORDER; i++) {
        g_buddy.orders[i].free_count = 0;
        list_init(&g_buddy.orders[i].free_list);
    }

    /* Calculate buddy map size */
    g_buddy.total_pages = ALIGN_UP(g_bootinfo.mem.max_phys_addr, PAGE_SIZE) / PAGE_SIZE;
    size_t array_size = ALIGN_UP(g_buddy.total_pages * sizeof(struct page), PAGE_SIZE);

    /* Find suitable region for map */
    struct mregion *mmap = g_bootinfo.mmap;
    uint32_t length = g_bootinfo.mem.length;
    uintptr_t array_phys = 0;

    for (uint32_t i = 0; i < length; i++) {
        if (mmap[i].type == MMAP_FREE) {
            uintptr_t start_addr = ALIGN_UP(mmap[i].base, PAGE_SIZE);
            uintptr_t end_addr = ALIGN_DOWN(mmap[i].base + mmap[i].length, PAGE_SIZE);

            /* Skip the first 1MB of memory */
            if (start_addr < 0x100000ULL) {
                start_addr = 0x100000ULL;
            }

            if (end_addr > start_addr && (end_addr - start_addr) >= array_size) {
                array_phys = start_addr;
                break;
            }
        }
    }

    if (!array_phys || array_phys == 0) panic("buddy: no suitable region for buddy memory map.");
    uintptr_t array_end = array_phys + array_size;

    g_buddy.mmap_phys = array_phys;
    g_buddy.mmap_size = array_size;
    g_buddy.mmap = (struct page*)(array_phys + HHDM_OFFSET);

    memset(g_buddy.mmap, 0, array_size);

    uintptr_t pool_start = PFN_TO_PHYS(BUDDY_POOL_START_PFN);

    for (uint32_t i = 0; i < length; i++) {
        if (mmap[i].type == MMAP_FREE) {
            uintptr_t start_addr = ALIGN_UP(mmap[i].base, PAGE_SIZE);
            uintptr_t end_addr = ALIGN_DOWN(mmap[i].base + mmap[i].length, PAGE_SIZE);

            /* Skip the first 1MB of memory and memory map area */
            if (start_addr < 0x100000ULL) start_addr = 0x100000ULL;
            if (array_phys < start_addr && start_addr < array_end) start_addr = array_end;
            if (array_phys < end_addr && end_addr < array_end) end_addr = array_phys;
            if (start_addr > end_addr) continue; /* can't be. */

            uintptr_t curr_addr = start_addr;
            while (curr_addr < end_addr) {
                /* Do not touch frames */
                if (curr_addr >= pool_start) {
                    curr_addr = end_addr;
                    break;
                }

                /* 
                    Skip the memory map area and pool_start
                    array_phys =< curr_addr < array_end
                */
                if (curr_addr >= array_phys && curr_addr < array_end) {
                    curr_addr = array_end;
                    continue;
                }
                uintptr_t limit = end_addr;
                /* curr_addr < array_phys < end_addr */
                if (curr_addr < array_phys && array_phys < end_addr) limit = array_phys;
                if (limit > pool_start) limit = pool_start; /* limit <= pool_start */

                size_t remain = limit - curr_addr;
                int target_order = 0;

                for (int order = MAX_BUDDY_ORDER - 1; order >= 0; order--) {
                    size_t block_size = ORDER_TO_SIZE(order);
                    if (remain >= block_size && (curr_addr & (block_size - 1)) == 0) {
                        target_order = order; /* if address is aligned to block */
                        break;                /* and remaining size is large enough */
                    }
                }

                size_t pfn = PHYS_TO_PFN(curr_addr);
                struct page* pg = pfn_to_page(pfn);

                pg->is_free = true;
                list_add_tail(&pg->page_list, &g_buddy.orders[target_order].free_list);
                g_buddy.orders[target_order].free_count++;
                g_buddy.free_pages += 1ULL << target_order;

                curr_addr += ORDER_TO_SIZE(target_order);
            }
        }
    }
}
