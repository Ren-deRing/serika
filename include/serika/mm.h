#pragma once

#include <serika/lock.h>
#include <serika/mutex.h>
#include <serika/pmap.h>

#include <deps/tree.h>
#include <stddef.h>
#include <stdint.h>

#define USER_SPACE_END 0x0000800000000000ULL
#define MMAP_TOP       0x00007f0000000000ULL /* mmap grows down from here, below the stack */

#define MAP_SHARED          0x000001
#define MAP_PRIVATE         0x000002
#define MAP_FIXED           0x000010
#define MAP_ANONYMOUS       0x000020
#define MAP_FIXED_NOREPLACE 0x100000

struct file;

struct vma {
    uintptr_t    start;
    uintptr_t    end;  /* exclusive */
    uint32_t     prot; /* PROT_READ, PROT_WRITE, PROT_EXEC */
    struct file *file; /* MAP_PRIVATE of a file, our reference. NULL: anonymous */
    uint64_t     off;  /* where start is in it */

    RB_ENTRY(vma) node;
};

RB_HEAD(vma_tree, vma);

struct mm {
    struct pmap     *pmap;
    struct vma_tree  vmas;  /* never overlap */
    struct mutex     lock;  /* vmas, brk, mmap_next, and faults filling the pmap. may sleep: shootdowns */
    uint32_t         users;

    uintptr_t        brk_start;
    uintptr_t        brk;       /* break */
    uintptr_t        brk_end;   /* end of the heap vma */
    uintptr_t        mmap_next; /* the next mmap ends here. */
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
int mm_write(struct mm *mm, uintptr_t addr, const void *src, size_t size);
struct mm *mm_fork(struct mm *old);
int mm_protect(struct mm *mm, uintptr_t addr, size_t size, uint32_t prot);
int mm_unmap(struct mm *mm, uintptr_t addr, size_t size);
int mm_discard(struct mm *mm, uintptr_t addr, size_t size);
long mm_mmap(struct mm *mm, uintptr_t addr, size_t size, uint32_t prot, uint32_t flags, struct file *file, uint64_t off);
uintptr_t mm_take_area(struct mm *mm, size_t size);
void mm_set_brk(struct mm *mm, uintptr_t start);
uintptr_t mm_brk(struct mm *mm, uintptr_t brk);

int copy_to_user(uintptr_t to, const void *from, size_t size);
int copy_from_user(void *to, uintptr_t from, size_t size);
long strncpy_from_user(char *to, uintptr_t from, size_t max); /* length, nul not counted */
int mm_read_nofault(struct mm *mm, uintptr_t addr, uint32_t *val);
