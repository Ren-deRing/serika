#pragma once

#include <serika/page.h>

struct pmap {
    void *pm_root_virt;
    uintptr_t pm_root_phys;
    spinlock_t lock;
};

struct pmap* pmap_kernel(void);
struct pmap* pmap_create(void);

int pmap_map(struct pmap *pmap, uintptr_t virt, uintptr_t phys, size_t size, uint32_t flags);
void pmap_unmap(struct pmap *pmap, uintptr_t virt, size_t size);
uintptr_t pmap_extract(struct pmap *pmap, uintptr_t va);

void pmap_destroy(struct pmap *pmap);
void pmap_activate(struct pmap *pmap);

int pmap_init(void);
void kmap_init(void);
