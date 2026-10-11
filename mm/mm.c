#include <serika/buddy.h>
#include <serika/cpu.h>
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

    mm->brk_start = mm->brk = mm->brk_end = 0;
    mm->mmap_next = MMAP_TOP;

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

/* need 0: prot is not checked, for loading code into read-only vmas */
static int mm_access(struct mm *mm, uintptr_t addr, void *buf, size_t size, bool to_mm, uint32_t need) {
    uint8_t *kbuf = buf;

    uint64_t f = spin_lock_irqsave(&mm->lock);

    while (size) {
        struct vma *vma = vma_find(mm, addr);
        if (!vma || (need & ~vma->prot)) {
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

        if (to_mm) memcpy((void *)P2V(phys + off), kbuf, n);
        else       memcpy(kbuf, (void *)P2V(phys + off), n);

        addr += n;
        kbuf += n;
        size -= n;
    }

    spin_unlock_irqrestore(&mm->lock, f);
    return 0;
}

int mm_copy_to(struct mm *mm, uintptr_t addr, const void *src, size_t size) {
    return mm_access(mm, addr, (void *)src, size, true, 0);
}

int copy_to_user(uintptr_t to, const void *from, size_t size) {
    return mm_access(current_task()->mm, to, (void *)from, size, true, PROT_WRITE);
}

int copy_from_user(void *to, uintptr_t from, size_t size) {
    return mm_access(current_task()->mm, from, to, size, false, PROT_READ);
}

/* the range must be mapped. */
int mm_protect(struct mm *mm, uintptr_t addr, size_t size, uint32_t prot) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;

    size = ALIGN_UP(size, PAGE_SIZE);
    if (size == 0) return 0;

    uintptr_t end = addr + size;
    if (end < addr || end > USER_SPACE_END) return -ENOMEM;

    struct vma *head = kmalloc(sizeof(struct vma));
    struct vma *tail = kmalloc(sizeof(struct vma));

    if (!head || !tail) {
        kfree(head);
        kfree(tail);
        return -ENOMEM;
    }

    uint64_t f = spin_lock_irqsave(&mm->lock);

    for (uintptr_t a = addr; a < end;) {
        struct vma *vma = vma_find(mm, a);
        if (!vma) {
            spin_unlock_irqrestore(&mm->lock, f);
            kfree(head);
            kfree(tail);
            return -ENOMEM;
        }

        a = vma->end;
    }

    struct vma *vma = vma_find(mm, addr);
    if (vma->start < addr) {
        *head = (struct vma){ .start = addr, .end = vma->end, .prot = vma->prot };
        vma->end = addr;
        RB_INSERT(vma_tree, &mm->vmas, head);
        head = NULL;
    }

    vma = vma_find(mm, end - 1);
    if (vma->end > end) {
        *tail = (struct vma){ .start = end, .end = vma->end, .prot = vma->prot };
        vma->end = end;
        RB_INSERT(vma_tree, &mm->vmas, tail);
        tail = NULL;
    }

    for (uintptr_t a = addr; a < end;) {
        vma = vma_find(mm, a);
        vma->prot = prot;
        a = vma->end;
    }

    bool changed = pmap_protect(mm->pmap, addr, size, prot | PROT_USER);

    spin_unlock_irqrestore(&mm->lock, f);

    if (changed) pmap_shootdown(mm->pmap, addr, size);

    kfree(head);
    kfree(tail);
    return 0;
}

/* anonymous, below the last one. the address or -errno */
long mm_mmap(struct mm *mm, size_t size, uint32_t prot) {
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;
    if (size == 0) return -EINVAL;

    size = ALIGN_UP(size, PAGE_SIZE);
    if (size == 0) return -ENOMEM;

    struct vma *vma = kmalloc(sizeof(struct vma));
    if (!vma) return -ENOMEM;

    uint64_t f = spin_lock_irqsave(&mm->lock);

    uintptr_t addr = mm->mmap_next - size;

    if (size > mm->mmap_next || addr < mm->brk_end || vma_overlaps(mm, addr, mm->mmap_next)) {
        spin_unlock_irqrestore(&mm->lock, f);
        kfree(vma);
        return -ENOMEM;
    }

    *vma = (struct vma){ .start = addr, .end = mm->mmap_next, .prot = prot };
    RB_INSERT(vma_tree, &mm->vmas, vma);
    mm->mmap_next = addr;

    spin_unlock_irqrestore(&mm->lock, f);
    return (long)addr;
}

void mm_set_brk(struct mm *mm, uintptr_t start) {
    uint64_t f = spin_lock_irqsave(&mm->lock);
    mm->brk_start = mm->brk = mm->brk_end = ALIGN_UP(start, PAGE_SIZE);
    spin_unlock_irqrestore(&mm->lock, f);
}

uintptr_t mm_brk(struct mm *mm, uintptr_t brk) {
    uint64_t f = spin_lock_irqsave(&mm->lock);

    uintptr_t old_end = ALIGN_UP(mm->brk, PAGE_SIZE);
    uintptr_t new_end = ALIGN_UP(brk, PAGE_SIZE);

    if (brk < mm->brk_start || new_end < brk) goto out;

    if (new_end > mm->brk_end) {
        if (new_end > mm->mmap_next || vma_overlaps(mm, mm->brk_end, new_end)) goto out;

        if (mm->brk_end > mm->brk_start) {
            vma_find(mm, mm->brk_end - 1)->end = new_end;
        } else {
            struct vma *heap = kmalloc(sizeof(struct vma));
            if (!heap) goto out;

            *heap = (struct vma){ .start = mm->brk_start, .end = new_end, .prot = PROT_READ | PROT_WRITE };
            RB_INSERT(vma_tree, &mm->vmas, heap);
        }

        mm->brk_end = new_end;
    } else {
        /* the vma and its frames stay. zeroed: malloc trusts memory a growing break gives to be zero */
        for (uintptr_t va = new_end; va < old_end; va += PAGE_SIZE) {
            uintptr_t phys = pmap_extract(mm->pmap, va);
            if (phys) memset((void *)P2V(phys), 0, PAGE_SIZE);
        }
    }

    mm->brk = brk;

out:
    brk = mm->brk;
    spin_unlock_irqrestore(&mm->lock, f);
    return brk;
}
