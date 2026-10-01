#include <serika/buddy.h>
#include <serika/list.h>
#include <serika/lock.h>
#include <serika/page.h>
#include <serika/slab.h>

#include <stddef.h>

#define KMALLOC_CACHE_COUNT 12
static struct kmem_cache kmalloc_caches[] = {
    { .size = 8 },
    { .size = 16 },
    { .size = 32 },
    { .size = 64 },
    { .size = 96 },
    { .size = 128 },
    { .size = 192 },
    { .size = 256 },
    { .size = 512 },
    { .size = 1024 },
    { .size = 2048 },
    { .size = 4096 },
};

void* slab_alloc(struct kmem_cache *cache) {
    (void)cache;
    return NULL;
}

void slab_init() {
    for (int i = 0; i<KMALLOC_CACHE_COUNT; i++) {
        struct kmem_cache *cache = &kmalloc_caches[i];

        spin_lock_init(&cache->lock);

        list_init(&cache->empty);
        list_init(&cache->full);
        list_init(&cache->partial);

        struct page *page = alloc_pages(0);

        page->slab.free_head = NULL;
        page->slab.inuse = 0;
        page->slab.objects = PAGE_SIZE / cache->size;
        page->slab.cache = cache;

        list_add(&cache->empty, &page->page_list);
    }
}