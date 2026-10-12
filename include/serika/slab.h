#pragma once

#include <serika/list.h>
#include <serika/lock.h>

#include <stdint.h>

/* a cache of objects of one size. a slab is a page of them. see mm/slab.c */
struct kmem_cache {
    size_t     size;
    unsigned   objects;    /* in a slab */
    unsigned   index;      /* of the cpus' array of caches */

    spinlock_t lock;       /* partial, nr_partial and the on_list of its slabs. the only lock: no slab op under it calls the buddy */
    list_node  partial;    /* slabs nobody owns that have free objects */
    unsigned   nr_partial;
};

/* in the page of the slab */
struct slab {
    uint64_t           state; /* freelist head, objects in use and frozen in one word: slab.c */
    struct kmem_cache *cache; /* NULL: not a slab */
    bool               on_list; /* partial. cache lock */
};

void *slab_alloc(struct kmem_cache *cache);
void  slab_free(void *obj);

void *kmalloc(size_t size);
void  kfree(void *ptr);

void *kmalloc_aligned(size_t align, size_t size);

void slab_init();
