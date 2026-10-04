#pragma once

#include <serika/list.h>
#include <serika/lock.h>

#include <stdint.h>

struct kmem_cache {
    size_t     size;
    spinlock_t lock; /* TODO: we will need percpu cache */

    list_node partial;
    list_node full;
    list_node empty;
};

struct slab {
    void     *free_head;
    uint16_t  inuse;
    uint16_t  objects;
    struct kmem_cache *cache;    
};

void *slab_alloc(struct kmem_cache *cache);
void  slab_free(void *obj);

void *kmalloc(size_t size);
void  kfree(void *ptr);

void *kmalloc_aligned(size_t align, size_t size);

void slab_init();
