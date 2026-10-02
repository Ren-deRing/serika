#pragma once

#include <serika/page.h>

#include <stddef.h>

#define ORDER_TO_SIZE(order) (1ULL << (PAGE_SHIFT + (order)))
static inline int SIZE_TO_ORDER(size_t size) {
    size_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;

    if (pages <= 1) return 0;

    return (int)(64 - __builtin_clzll(pages - 1));
}

struct page* pfn_to_page(size_t pfn);
size_t page_to_pfn(struct page* pg);

void* alloc_pages(int order);
void free_pages(void* addr, int order);

void buddy_init();
