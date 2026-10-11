#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/pmap.h>
#include <serika/slab.h>
#include <serika/string.h>

RB_GENERATE(vma_tree, vma, node, vma_cmp);

struct mm *mm_create(void) {
    struct mm *mm = kmalloc(sizeof(struct mm));
    if (!mm) return NULL;

    mm->pmap = pmap_create();
    if (!mm->pmap) {
        kfree(mm);
        return NULL;
    }

    RB_INIT(&mm->vmas);
    spin_lock_init(&mm->lock);
    mm->users = 1;

    return mm;
}

void mm_get(struct mm *mm) {
    __atomic_add_fetch(&mm->users, 1, 0); /* __ATOMIC_RELAXED */
}

/* never on the mm the cpu is running on: pmap_destroy() skips the active pmap */
void mm_put(struct mm *mm) {
    if (__atomic_sub_fetch(&mm->users, 1, 4) != 0) return; /* __ATOMIC_ACQ_REL */

    struct vma *vma, *tmp;

    RB_FOREACH_SAFE(vma, vma_tree, &mm->vmas, tmp) {
        for (uintptr_t va = vma->start; va < vma->end; va += PAGE_SIZE)
            free_pages((void *)pmap_extract(mm->pmap, va), 0);

        RB_REMOVE(vma_tree, &mm->vmas, vma);
        kfree(vma);
    }

    pmap_destroy(mm->pmap);
    kfree(mm);
}

/* mm lock held */
static struct vma *vma_find(struct mm *mm, uintptr_t addr) {
    struct vma *vma = RB_ROOT(&mm->vmas);

    while (vma) {
        if (addr < vma->start)     vma = RB_LEFT(vma, node);
        else if (addr >= vma->end) vma = RB_RIGHT(vma, node);
        else                       return vma;
    }

    return NULL;
}

/* mm lock held */
static bool vma_overlaps(struct mm *mm, uintptr_t start, uintptr_t end) {
    struct vma *vma = RB_ROOT(&mm->vmas);

    while (vma) {
        if (end <= vma->start)      vma = RB_LEFT(vma, node);
        else if (start >= vma->end) vma = RB_RIGHT(vma, node);
        else                        return true;
    }

    return false;
}

/* mm lock held. returns the frame, a zeroed one if it was not there */
static uintptr_t vma_populate(struct mm *mm, struct vma *vma, uintptr_t page) {
    uintptr_t phys = pmap_extract(mm->pmap, page);
    if (phys) return phys; /* another thread faulted it in first */

    phys = (uintptr_t)alloc_pages(0);
    if (!phys) return 0;

    memset((void *)P2V(phys), 0, PAGE_SIZE);

    if (pmap_map(mm->pmap, page, phys, PAGE_SIZE, vma->prot | PROT_USER) != 0) {
        free_pages((void *)phys, 0);
        return 0;
    }

    return phys;
}

/* only the vma. frames come on the first fault */
int mm_map(struct mm *mm, uintptr_t addr, size_t size, uint32_t prot) {
    if ((addr | size) & (PAGE_SIZE - 1)) return -EINVAL;
    if (!size || addr + size < addr || addr + size > USER_SPACE_END) return -EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;

    struct vma *vma = kmalloc(sizeof(struct vma));
    if (!vma) return -ENOMEM;

    vma->start = addr;
    vma->end   = addr + size;
    vma->prot  = prot;

    uint64_t f = spin_lock_irqsave(&mm->lock);

    if (vma_overlaps(mm, vma->start, vma->end)) {
        spin_unlock_irqrestore(&mm->lock, f);
        kfree(vma);
        return -EEXIST;
    }

    RB_INSERT(vma_tree, &mm->vmas, vma);

    spin_unlock_irqrestore(&mm->lock, f);
    return 0;
}

/* access is PROT_READ, PROT_WRITE or PROT_EXEC */
int mm_fault(struct mm *mm, uintptr_t addr, uint32_t access) {
    int ret = 0;

    uint64_t f = spin_lock_irqsave(&mm->lock);

    struct vma *vma = vma_find(mm, addr);

    if (!vma || (access & ~vma->prot))
        ret = -EFAULT;
    else if (!vma_populate(mm, vma, ALIGN_DOWN(addr, PAGE_SIZE)))
        ret = -ENOMEM;

    spin_unlock_irqrestore(&mm->lock, f);
    return ret;
}

/* ignores prot, for loading code into read-only vmas */
int mm_copy_to(struct mm *mm, uintptr_t addr, const void *src, size_t size) {
    const uint8_t *from = src;

    uint64_t f = spin_lock_irqsave(&mm->lock);

    while (size) {
        struct vma *vma = vma_find(mm, addr);
        if (!vma) {
            spin_unlock_irqrestore(&mm->lock, f);
            return -EFAULT;
        }

        uintptr_t phys = vma_populate(mm, vma, ALIGN_DOWN(addr, PAGE_SIZE));
        if (!phys) {
            spin_unlock_irqrestore(&mm->lock, f);
            return -ENOMEM;
        }

        size_t off = addr & (PAGE_SIZE - 1);
        size_t n   = PAGE_SIZE - off;
        if (n > size) n = size;

        memcpy((void *)P2V(phys + off), from, n);

        addr += n;
        from += n;
        size -= n;
    }

    spin_unlock_irqrestore(&mm->lock, f);
    return 0;
}
