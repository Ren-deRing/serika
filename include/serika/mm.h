#pragma once

#include <serika/lock.h>
#include <serika/pmap.h>

#include <deps/tree.h>
#include <stddef.h>
#include <stdint.h>

#define USER_SPACE_END 0x0000800000000000ULL

struct vma {
    uintptr_t start;
    uintptr_t end;  /* exclusive */
    uint32_t  prot; /* PROT_READ, PROT_WRITE, PROT_EXEC */

    RB_ENTRY(vma) node;
};

RB_HEAD(vma_tree, vma);

struct mm {
    struct pmap     *pmap;
    struct vma_tree  vmas;  /* never overlap */
    spinlock_t       lock;  /* vmas, and faults filling the pmap */
    uint32_t         users;
};

static inline int vma_cmp(struct vma *a, struct vma *b) {
    if (a->start < b->start)
        return (-1);
    if (a->start > b->start)
        return (1);

    return (0);
}

RB_PROTOTYPE(vma_tree, vma, node, vma_cmp);

struct mm *mm_create(void);
void mm_get(struct mm *mm);
void mm_put(struct mm *mm);

int mm_map(struct mm *mm, uintptr_t addr, size_t size, uint32_t prot);
int mm_fault(struct mm *mm, uintptr_t addr, uint32_t access);
int mm_copy_to(struct mm *mm, uintptr_t addr, const void *src, size_t size);
