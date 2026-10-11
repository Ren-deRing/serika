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
    mutex_init(&mm->map_mutex);
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

    mutex_lock(&mm->map_mutex);
    uint64_t f = spin_lock_irqsave(&mm->lock);

    bool overlaps = vma_overlaps(mm, vma->start, vma->end);
    if (!overlaps) RB_INSERT(vma_tree, &mm->vmas, vma);

    spin_unlock_irqrestore(&mm->lock, f);
    mutex_unlock(&mm->map_mutex);

    if (overlaps) {
        kfree(vma);
        return -EEXIST;
    }

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

/* page by page: nothing past the nul's page is touched. -ENAMETOOLONG if max has no nul */
long strncpy_from_user(char *to, uintptr_t from, size_t max) {
    size_t done = 0;

    while (done < max) {
        size_t n = PAGE_SIZE - ((from + done) & (PAGE_SIZE - 1));
        if (n > max - done) n = max - done;

        int err = copy_from_user(to + done, from + done, n);
        if (err) return err;

        for (size_t i = 0; i < n; i++)
            if (to[done + i] == '\0') return done + i;

        done += n;
    }

    return -ENAMETOOLONG;
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

    mutex_lock(&mm->map_mutex);
    uint64_t f = spin_lock_irqsave(&mm->lock);

    for (uintptr_t a = addr; a < end;) {
        struct vma *vma = vma_find(mm, a);
        if (!vma) {
            spin_unlock_irqrestore(&mm->lock, f);
            mutex_unlock(&mm->map_mutex);
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

    mutex_unlock(&mm->map_mutex);

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

    mutex_lock(&mm->map_mutex);
    uint64_t f = spin_lock_irqsave(&mm->lock);

    uintptr_t addr = mm->mmap_next - size;
    bool fits = size <= mm->mmap_next && addr >= mm->brk_end && !vma_overlaps(mm, addr, mm->mmap_next);

    if (fits) {
        *vma = (struct vma){ .start = addr, .end = mm->mmap_next, .prot = prot };
        RB_INSERT(vma_tree, &mm->vmas, vma);
        mm->mmap_next = addr;
    }

    spin_unlock_irqrestore(&mm->lock, f);
    mutex_unlock(&mm->map_mutex);

    if (!fits) {
        kfree(vma);
        return -ENOMEM;
    }

    return (long)addr;
}

void mm_set_brk(struct mm *mm, uintptr_t start) {
    mutex_lock(&mm->map_mutex);
    uint64_t f = spin_lock_irqsave(&mm->lock);
    mm->brk_start = mm->brk = mm->brk_end = ALIGN_UP(start, PAGE_SIZE);
    spin_unlock_irqrestore(&mm->lock, f);
    mutex_unlock(&mm->map_mutex);
}

/* mm lock held. vmas leave [start, end). one may split in two: *spare is used then */
static void vma_cut(struct mm *mm, uintptr_t start, uintptr_t end, struct vma **spare) {
    struct vma key = { .start = start };
    struct vma *vma = vma_find(mm, start);

    if (!vma) vma = RB_NFIND(vma_tree, &mm->vmas, &key);

    while (vma && vma->start < end) {
        struct vma *next = RB_NEXT(vma_tree, &mm->vmas, vma);

        if (vma->start < start && vma->end > end) {
            **spare = (struct vma){ .start = end, .end = vma->end, .prot = vma->prot };
            vma->end = start;
            RB_INSERT(vma_tree, &mm->vmas, *spare);
            *spare = NULL;
        } else if (vma->start < start) {
            vma->end = start;
        } else if (vma->end > end) {
            vma->start = end; /* still between its neighbours: none of them overlap it */
        } else {
            RB_REMOVE(vma_tree, &mm->vmas, vma);
            kfree(vma);
        }

        vma = next;
    }
}

/* frames freed per shootdown: one page of their addresses */
#define UNMAP_BATCH (PAGE_SIZE / sizeof(uintptr_t))

/* map_mutex held. frames are freed only after every cpu dropped them. page tables stay until mm_put() */
static int unmap_range(struct mm *mm, uintptr_t addr, uintptr_t end) {
    uintptr_t frames_phys = (uintptr_t)alloc_pages(0);
    struct vma *spare = kmalloc(sizeof(struct vma));

    if (!frames_phys || !spare) {
        free_pages((void *)frames_phys, 0);
        kfree(spare);
        return -ENOMEM;
    }

    uintptr_t *frames = (uintptr_t *)P2V(frames_phys);

    uint64_t f = spin_lock_irqsave(&mm->lock);
    vma_cut(mm, addr, end, &spare);
    spin_unlock_irqrestore(&mm->lock, f);

    /* no vma covers it now: a fault there fails, and map_mutex keeps new ones out */
    for (uintptr_t va = addr; va < end; va += UNMAP_BATCH * PAGE_SIZE) {
        size_t n = end - va;
        if (n > UNMAP_BATCH * PAGE_SIZE) n = UNMAP_BATCH * PAGE_SIZE;

        size_t count = pmap_clear(mm->pmap, va, n, frames, UNMAP_BATCH);
        if (!count) continue;

        pmap_shootdown(mm->pmap, va, n);

        for (size_t i = 0; i < count; i++) free_pages((void *)frames[i], 0);
    }

    free_pages((void *)frames_phys, 0);
    kfree(spare);
    return 0;
}

int mm_unmap(struct mm *mm, uintptr_t addr, size_t size) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;

    size = ALIGN_UP(size, PAGE_SIZE);
    if (size == 0) return -EINVAL;

    uintptr_t end = addr + size;
    if (end < addr || end > USER_SPACE_END) return -EINVAL;

    mutex_lock(&mm->map_mutex);
    int err = unmap_range(mm, addr, end);
    mutex_unlock(&mm->map_mutex);

    return err;
}

/* map_mutex held. the heap's last vma grows, unless munmap() took that end */
static bool brk_grow(struct mm *mm, uintptr_t new_end) {
    bool ok = false;

    uint64_t f = spin_lock_irqsave(&mm->lock);

    if (new_end > mm->mmap_next || vma_overlaps(mm, mm->brk_end, new_end)) goto out;

    struct vma *heap = mm->brk_end > mm->brk_start ? vma_find(mm, mm->brk_end - 1) : NULL;

    if (heap) {
        heap->end = new_end;
    } else {
        heap = kmalloc(sizeof(struct vma));
        if (!heap) goto out;

        *heap = (struct vma){ .start = mm->brk_end, .end = new_end, .prot = PROT_READ | PROT_WRITE };
        RB_INSERT(vma_tree, &mm->vmas, heap);
    }

    ok = true;

out:
    spin_unlock_irqrestore(&mm->lock, f);
    return ok;
}

/* linux's: the new break, or the old one if it can't move */
uintptr_t mm_brk(struct mm *mm, uintptr_t brk) {
    mutex_lock(&mm->map_mutex);

    uintptr_t new_end = ALIGN_UP(brk, PAGE_SIZE);

    if (brk < mm->brk_start || new_end < brk) goto out;

    if (new_end > mm->brk_end) {
        if (!brk_grow(mm, new_end)) goto out;
    } else if (new_end < mm->brk_end) {
        /* given back. growing again faults in zeroed pages, which malloc counts on */
        if (unmap_range(mm, new_end, mm->brk_end) != 0) goto out;
    }

    mm->brk_end = new_end;
    mm->brk = brk;

out:
    brk = mm->brk;
    mutex_unlock(&mm->map_mutex);
    return brk;
}
