#include <serika/buddy.h>
#include <serika/list.h>
#include <serika/lock.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/slab.h>

#define KMALLOC_CACHE_COUNT 12
static struct kmem_cache kmalloc_caches[KMALLOC_CACHE_COUNT] = {
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

struct kmem_cache *find_cache(size_t size) {
    for (size_t i = 0; i < KMALLOC_CACHE_COUNT; i++) {
        if (size <= kmalloc_caches[i].size)
            return &kmalloc_caches[i];
    }

    return NULL;
}

static void init_page(struct page *page, uintptr_t phys, struct kmem_cache *cache) {
    uint64_t base = P2V(phys);

    page->slab.free_head = (void *)base;
    page->slab.inuse = 0;
    page->slab.objects = PAGE_SIZE / cache->size;
    page->slab.cache = cache;

    for (size_t i = 0; i < page->slab.objects; i++) {
        void *obj = (void *)(base + i * cache->size);
        void *next = (void *)(base + (i + 1) * cache->size);

        if (i + 1 == page->slab.objects) next = NULL;

        *(void **)obj = next;
    }

    list_init(&page->page_list);

    list_add(&cache->empty, &page->page_list);
}

void* slab_alloc(struct kmem_cache *cache) {
    spin_lock(&cache->lock);

    struct page *page;

    if (!list_empty(&cache->partial)) {
        /* We already have slab that is in use */
        page = list_first(&cache->partial, struct page, page_list);
    } else if (!list_empty(&cache->empty)) {
        /* There's a fresh slab. */
        page = list_first(&cache->empty, struct page, page_list);
    } else {
        /* There are no slab at all. allocate new one. */
        uintptr_t phys = (uintptr_t)alloc_pages(0);
        if (!phys) {
            /* Failed to allocate page */
            spin_unlock(&cache->lock);
            return NULL;
        }
        page = pfn_to_page(PHYS_TO_PFN(phys));

        init_page(page, phys, cache);
    }

    void *obj = page->slab.free_head;

    page->slab.free_head = *(void **)obj;
    page->slab.inuse++;

    if (page->slab.inuse == page->slab.objects) {
        /* The slab is full */
        list_del(&page->page_list);
        list_add(&page->page_list, &cache->full);
    } else if (page->slab.inuse == 1) {
        /* The slab is being used for the first time */
        list_del(&page->page_list);
        list_add(&page->page_list, &cache->partial);
    }

    spin_unlock(&cache->lock);

    return obj;
}

void slab_free(void *obj) {
    struct page *page = pfn_to_page(PHYS_TO_PFN(V2P(((uintptr_t)obj & ~((uintptr_t)PAGE_SIZE - 1)))));
    struct slab *slab = &page->slab;
    struct kmem_cache *cache = slab->cache;

    spin_lock(&cache->lock);

    bool was_full = slab->inuse == slab->objects;

    *(void **)obj = slab->free_head;
    slab->free_head = obj;
    slab->inuse--;

    if (was_full) {
        list_del(&page->page_list);
        list_add(&page->page_list, &cache->partial);
    } else if (slab->inuse == 0) {
        list_del(&page->page_list);
        list_add(&page->page_list, &cache->empty);
    }

    spin_unlock(&cache->lock);
}

void *kmalloc(size_t size) {
    struct kmem_cache *cache = find_cache(size);

    if (!cache)
        return NULL;

    return slab_alloc(cache);
}

void kfree(void *ptr) {
    if (!ptr) return;

    slab_free(ptr);
}

void slab_init() {
    for (int i = 0; i<KMALLOC_CACHE_COUNT; i++) {
        struct kmem_cache *cache = &kmalloc_caches[i];

        spin_lock_init(&cache->lock);

        list_init(&cache->empty);
        list_init(&cache->full);
        list_init(&cache->partial);

        uintptr_t phys = (uintptr_t)alloc_pages(0);
        if (!phys)panic("slab: failed to allocate initial slab.");

        struct page *page = pfn_to_page(PHYS_TO_PFN(phys));

        init_page(page, phys, cache);
    }
}
