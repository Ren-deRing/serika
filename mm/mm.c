#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/pmap.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/vfs.h>

RB_GENERATE(vma_tree, vma, node, vma_cmp);

static struct page *frame(uintptr_t phys) {
    return pfn_to_page(PHYS_TO_PFN(phys));
}

/* one more user pmap maps it */
void frame_init(uintptr_t phys) {
    __atomic_store_n(&frame(phys)->refs, 1, 0); /* __ATOMIC_RELAXED */
}

void frame_get(uintptr_t phys) {
    __atomic_add_fetch(&frame(phys)->refs, 1, 0); /* __ATOMIC_RELAXED */
}

/* the last mapping frees it */
void frame_put(uintptr_t phys) {
    if (__atomic_sub_fetch(&frame(phys)->refs, 1, 4) == 0) free_pages((void *)phys, 0); /* __ATOMIC_ACQ_REL */
}

/* a piece of v: same rights, same file at the matching offset, one more reference on it */
static struct vma vma_piece(const struct vma *v, uintptr_t start, uintptr_t end) {
    if (v->file) fget(v->file);

    return (struct vma){ .start = start, .end = end, .prot = v->prot, .file = v->file, .off = v->off + (start - v->start),
                        .shared = v->shared };
}

/* out of the tree already */
static void vma_free(struct vma *v) {
    if (v->file) vfs_close(v->file);
    kfree(v);
}

/* a pte for a vma's prot. PROT_NONE stays the kernel's */
static uint32_t user_rights(uint32_t prot) {
    return prot ? prot | PROT_USER : 0;
}

static spinlock_t mm_list_lock = SPINLOCK_INITIALIZER;
static list_node  mm_list = LIST_HEAD_INIT(mm_list);

struct mm *mm_create(void) {
    struct mm *mm = kmalloc(sizeof(struct mm));
    if (!mm) return NULL;

    mm->pmap = pmap_create();
    if (!mm->pmap) {
        kfree(mm);
        return NULL;
    }

    RB_INIT(&mm->vmas);
    mutex_init(&mm->lock);
    mm->users = 1;

    mm->brk_start = mm->brk = mm->brk_end = 0;
    mm->mmap_next = MMAP_TOP;

    uint64_t flags = spin_lock_irqsave(&mm_list_lock);
    list_add_tail(&mm->all, &mm_list);
    spin_unlock_irqrestore(&mm_list_lock, flags);

    return mm;
}

void mm_get(struct mm *mm) {
    __atomic_add_fetch(&mm->users, 1, 0); /* __ATOMIC_RELAXED */
}

/* never on the mm the cpu is running on: pmap_destroy() skips the active pmap */
void mm_put(struct mm *mm) {
    if (__atomic_sub_fetch(&mm->users, 1, 4) != 0) return; /* __ATOMIC_ACQ_REL */

    /* off the list: a walk takes no reference of an mm with no users, so after this none has it */
    uint64_t flags = spin_lock_irqsave(&mm_list_lock);
    list_del(&mm->all);
    spin_unlock_irqrestore(&mm_list_lock, flags);

    struct vma *vma, *tmp;

    RB_FOREACH_SAFE(vma, vma_tree, &mm->vmas, tmp) {
        for (uintptr_t va = vma->start; va < vma->end; va += PAGE_SIZE) {
            uintptr_t phys = pmap_extract(mm->pmap, va);
            if (phys) frame_put(phys);
        }

        RB_REMOVE(vma_tree, &mm->vmas, vma);
        vma_free(vma);
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

/* mm lock held. a write to a page fork left shared: our own copy, or the frame itself once no one else maps it */
static uintptr_t cow_break(struct mm *mm, struct vma *vma, uintptr_t page, uintptr_t phys) {
    if (__atomic_load_n(&frame(phys)->refs, 2) == 1) { /* __ATOMIC_ACQUIRE */
        pmap_protect(mm->pmap, page, PAGE_SIZE, user_rights(vma->prot)); /* more rights: a stale entry only faults again */
        return phys;
    }

    uintptr_t copy = (uintptr_t)alloc_pages(0);
    if (!copy) return 0;

    memcpy((void *)P2V(copy), (void *)P2V(phys), PAGE_SIZE);
    frame(copy)->refs = 1;

    pmap_replace(mm->pmap, page, copy, user_rights(vma->prot));

    /* another of our threads may still read the shared frame through its tlb */
    pmap_shootdown(mm->pmap, page, PAGE_SIZE);
    frame_put(phys);

    return copy;
}

/* mm lock held. the frame at page, faulted in, and ours alone if we are about to write */
static uintptr_t vma_page(struct mm *mm, struct vma *vma, uintptr_t page, bool write) {
    bool writable = false;
    uintptr_t phys = pmap_lookup(mm->pmap, page, &writable);

    /* the file's own page, mapped in place: nothing is copied, and a write to a read only entry just gets its right */
    if (vma->shared) {
        uint32_t rights = user_rights(vma->prot) | (vma->file->inode->ops->map_wc ? PROT_WC : 0);

        if (phys) {
            if (write && !writable && (vma->prot & PROT_WRITE)) pmap_protect(mm->pmap, page, PAGE_SIZE, rights);
            return phys;
        }

        phys = vfs_getpage(vma->file->inode, (vma->off + (page - vma->start)) >> PAGE_SHIFT);
        if (!phys) return 0;

        if (pmap_map(mm->pmap, page, phys, PAGE_SIZE, rights) != 0) {
            frame_put(phys);
            return 0;
        }

        return phys;
    }

    if (phys) {
        if (write && !writable && (vma->prot & PROT_WRITE)) return cow_break(mm, vma, page, phys);
        return phys; /* another thread faulted it in first */
    }

    phys = (uintptr_t)alloc_pages(0);
    if (!phys) return 0;

    memset((void *)P2V(phys), 0, PAGE_SIZE);
    frame(phys)->refs = 1;

    /* a private copy of the file's page. past its end it stays zero. mm->lock, then the inode's */
    if (vma->file) {
        uint64_t pos = vma->off + (page - vma->start);

        if (kernel_read(vma->file, (void *)P2V(phys), PAGE_SIZE, &pos) < 0) {
            frame(phys)->refs = 0;
            free_pages((void *)phys, 0);
            return 0;
        }
    }

    if (pmap_map(mm->pmap, page, phys, PAGE_SIZE, user_rights(vma->prot)) != 0) {
        frame(phys)->refs = 0;
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

    *vma = (struct vma){ .start = addr, .end = addr + size, .prot = prot };

    mutex_lock(&mm->lock);

    bool overlaps = vma_overlaps(mm, vma->start, vma->end);
    if (!overlaps) RB_INSERT(vma_tree, &mm->vmas, vma);

    mutex_unlock(&mm->lock);

    if (overlaps) {
        kfree(vma);
        return -EEXIST;
    }

    return 0;
}

/* access is PROT_READ, PROT_WRITE or PROT_EXEC */
int mm_fault(struct mm *mm, uintptr_t addr, uint32_t access) {
    int ret = 0;

    mutex_lock(&mm->lock);

    struct vma *vma = vma_find(mm, addr);

    if (!vma || (access & ~vma->prot))
        ret = -EFAULT;
    else if (!vma_page(mm, vma, ALIGN_DOWN(addr, PAGE_SIZE), access & PROT_WRITE))
        ret = -ENOMEM;

    mutex_unlock(&mm->lock);
    return ret;
}

/* need 0: prot is not checked, for loading code into read-only vmas */
static int mm_access(struct mm *mm, uintptr_t addr, void *buf, size_t size, bool to_mm, uint32_t need) {
    uint8_t *kbuf = buf;

    mutex_lock(&mm->lock);

    while (size) {
        struct vma *vma = vma_find(mm, addr);
        if (!vma || (need & ~vma->prot)) {
            mutex_unlock(&mm->lock);
            return -EFAULT;
        }

        /* the loader (need 0) writes into fresh pages: only a checked write breaks sharing */
        uintptr_t phys = vma_page(mm, vma, ALIGN_DOWN(addr, PAGE_SIZE), to_mm && (need & PROT_WRITE));
        if (!phys) {
            mutex_unlock(&mm->lock);
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

    mutex_unlock(&mm->lock);
    return 0;
}

int mm_copy_to(struct mm *mm, uintptr_t addr, const void *src, size_t size) {
    return mm_access(mm, addr, (void *)src, size, true, 0);
}

/* another task's memory, as that task could write it: clone's CLONE_CHILD_SETTID */
int mm_write(struct mm *mm, uintptr_t addr, const void *src, size_t size) {
    return mm_access(mm, addr, (void *)src, size, true, PROT_WRITE);
}

int copy_to_user(uintptr_t to, const void *from, size_t size) {
    return mm_access(current_task()->mm, to, (void *)from, size, true, PROT_WRITE);
}

int copy_from_user(void *to, uintptr_t from, size_t size) {
    return mm_access(current_task()->mm, from, to, size, false, PROT_READ);
}

/* without sleeping, for code under a spinlock: a word already in memory. -EFAULT if it is not
   there yet: copy_from_user() brings it, then try again */
int mm_read_nofault(struct mm *mm, uintptr_t addr, uint32_t *val) {
    if (addr & 3) return -EINVAL;

    uintptr_t phys = pmap_extract(mm->pmap, addr);
    if (!phys) return -EFAULT;

    *val = __atomic_load_n((uint32_t *)P2V(phys), 0); /* __ATOMIC_RELAXED */
    return 0;
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

    mutex_lock(&mm->lock);

    for (uintptr_t a = addr; a < end;) {
        struct vma *vma = vma_find(mm, a);
        if (!vma) {
            mutex_unlock(&mm->lock);
            kfree(head);
            kfree(tail);
            return -ENOMEM;
        }

        a = vma->end;
    }

    struct vma *vma = vma_find(mm, addr);
    if (vma->start < addr) {
        *head = vma_piece(vma, addr, vma->end);
        vma->end = addr;
        RB_INSERT(vma_tree, &mm->vmas, head);
        head = NULL;
    }

    vma = vma_find(mm, end - 1);
    if (vma->end > end) {
        *tail = vma_piece(vma, end, vma->end);
        vma->end = end;
        RB_INSERT(vma_tree, &mm->vmas, tail);
        tail = NULL;
    }

    for (uintptr_t a = addr; a < end;) {
        vma = vma_find(mm, a);
        vma->prot = prot;
        a = vma->end;
    }

    if (pmap_protect(mm->pmap, addr, size, prot | PROT_USER)) pmap_shootdown(mm->pmap, addr, size);

    mutex_unlock(&mm->lock);

    kfree(head);
    kfree(tail);
    return 0;
}

static int unmap_range(struct mm *mm, uintptr_t addr, uintptr_t end);

/* below the last one, or at addr with MAP_FIXED, replacing what was there. the address or -errno.
   file: MAP_PRIVATE of it from off, which takes its own reference */
long mm_mmap(struct mm *mm, uintptr_t addr, size_t size, uint32_t prot, uint32_t flags, struct file *file, uint64_t off) {
    bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);

    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;
    if (size == 0 || (off & (PAGE_SIZE - 1))) return -EINVAL;

    size = ALIGN_UP(size, PAGE_SIZE);
    if (size == 0) return -ENOMEM;

    if (fixed && ((addr & (PAGE_SIZE - 1)) || addr + size < addr || addr + size > USER_SPACE_END)) return -EINVAL;

    struct vma *vma = kmalloc(sizeof(struct vma));
    if (!vma) return -ENOMEM;

    long ret = 0;

    mutex_lock(&mm->lock);

    if (fixed) {
        if (vma_overlaps(mm, addr, addr + size)) {
            if (flags & MAP_FIXED_NOREPLACE) ret = -EEXIST;
            else                             ret = unmap_range(mm, addr, addr + size);
        }
    } else {
        addr = mm->mmap_next - size;

        if (size > mm->mmap_next || addr < mm->brk_end || vma_overlaps(mm, addr, mm->mmap_next)) ret = -ENOMEM;
        else mm->mmap_next = addr;
    }

    if (ret == 0) {
        if (file) fget(file);

        *vma = (struct vma){ .start = addr, .end = addr + size, .prot = prot, .file = file, .off = off,
                             .shared = file && (flags & MAP_SHARED) };
        RB_INSERT(vma_tree, &mm->vmas, vma);
    }

    mutex_unlock(&mm->lock);

    if (ret != 0) {
        kfree(vma);
        return ret;
    }

    return (long)addr;
}

/* the next mmap area, taken with no vma: the caller maps into it. exec puts ld.so there */
uintptr_t mm_take_area(struct mm *mm, size_t size) {
    uintptr_t addr = 0;

    size = ALIGN_UP(size, PAGE_SIZE);

    mutex_lock(&mm->lock);

    if (size && size <= mm->mmap_next && mm->mmap_next - size >= mm->brk_end) {
        addr = mm->mmap_next - size;
        mm->mmap_next = addr;
    }

    mutex_unlock(&mm->lock);
    return addr;
}

void mm_set_brk(struct mm *mm, uintptr_t start) {
    mutex_lock(&mm->lock);
    mm->brk_start = mm->brk = mm->brk_end = ALIGN_UP(start, PAGE_SIZE);
    mutex_unlock(&mm->lock);
}

/* mm lock held. vmas leave [start, end). one may split in two: *spare is used then */
static void vma_cut(struct mm *mm, uintptr_t start, uintptr_t end, struct vma **spare) {
    struct vma key = { .start = start };
    struct vma *vma = vma_find(mm, start);

    if (!vma) vma = RB_NFIND(vma_tree, &mm->vmas, &key);

    while (vma && vma->start < end) {
        struct vma *next = RB_NEXT(vma_tree, &mm->vmas, vma);

        if (vma->start < start && vma->end > end) {
            **spare = vma_piece(vma, end, vma->end);
            vma->end = start;
            RB_INSERT(vma_tree, &mm->vmas, *spare);
            *spare = NULL;
        } else if (vma->start < start) {
            vma->end = start;
        } else if (vma->end > end) {
            vma->off  += end - vma->start;
            vma->start = end; /* still between its neighbours: none of them overlap it */
        } else {
            RB_REMOVE(vma_tree, &mm->vmas, vma);
            vma_free(vma);
        }

        vma = next;
    }
}

/* frames freed per shootdown: one page of their addresses */
#define UNMAP_BATCH (PAGE_SIZE / sizeof(uintptr_t))

/* mm->lock held. the pages of [addr, end) go. frames are freed only after every cpu dropped them. frames: UNMAP_BATCH slots */
static void drop_pages(struct mm *mm, uintptr_t addr, uintptr_t end, uintptr_t *frames) {
    for (uintptr_t va = addr; va < end; va += UNMAP_BATCH * PAGE_SIZE) {
        size_t n = end - va;
        if (n > UNMAP_BATCH * PAGE_SIZE) n = UNMAP_BATCH * PAGE_SIZE;

        size_t count = pmap_clear(mm->pmap, va, n, frames, UNMAP_BATCH);
        if (!count) continue;

        pmap_shootdown(mm->pmap, va, n);

        for (size_t i = 0; i < count; i++) frame_put(frames[i]);
    }
}

/* a reference of an mm that still has users */
static bool mm_try_get(struct mm *mm) {
    uint32_t users = __atomic_load_n(&mm->users, 0); /* __ATOMIC_RELAXED */

    while (users) {
        if (__atomic_compare_exchange_n(&mm->users, &users, users + 1, false, 0, 0)) return true; /* relaxed */
    }

    return false;
}

/* the pages of the file from first to last (page aligned, offsets in it) leave the mappings that share them: the file
 * gave them up (a hole punched), and a mapping must not go on showing what is not there. a private mapping has its own
 * copies. no lock held: a fault takes the mm's and then the inode's */
void mm_unmap_file(struct inode *inode, uint64_t first, uint64_t last) {
    uintptr_t frames_phys = (uintptr_t)alloc_pages(0);
    if (!frames_phys) return;

    struct mm *prev = NULL;
    list_node *n;

    uint64_t flags = spin_lock_irqsave(&mm_list_lock);
    n = mm_list.next;

    for (;;) {
        struct mm *mm = NULL;

        for (; n != &mm_list; n = n->next) {
            struct mm *m = container_of(n, struct mm, all);

            if (mm_try_get(m)) {
                mm = m;
                break;
            }
        }

        spin_unlock_irqrestore(&mm_list_lock, flags);

        if (prev) mm_put(prev); /* not under the list's lock: the last one takes it */
        if (!mm) break;

        mutex_lock(&mm->lock);

        struct vma *vma;

        RB_FOREACH(vma, vma_tree, &mm->vmas) {
            if (!vma->shared || !vma->file || vma->file->inode != inode) continue;

            uint64_t lo = vma->off > first ? vma->off : first;
            uint64_t hi = vma->off + (vma->end - vma->start);

            if (hi > last) hi = last;
            if (lo >= hi) continue;

            uintptr_t va = vma->start + (lo - vma->off);

            drop_pages(mm, va, va + (hi - lo), (uintptr_t *)P2V(frames_phys));
        }

        mutex_unlock(&mm->lock);

        prev = mm;

        flags = spin_lock_irqsave(&mm_list_lock);
        n = mm->all.next; /* it is on the list: we hold a reference */
    }

    free_pages((void *)frames_phys, 0);
}

/* mm->lock held. page tables stay until mm_put() */
static int unmap_range(struct mm *mm, uintptr_t addr, uintptr_t end) {
    uintptr_t frames_phys = (uintptr_t)alloc_pages(0);
    struct vma *spare = kmalloc(sizeof(struct vma));

    if (!frames_phys || !spare) {
        free_pages((void *)frames_phys, 0);
        kfree(spare);
        return -ENOMEM;
    }

    vma_cut(mm, addr, end, &spare);

    /* no vma covers it now: a fault there fails, and mm->lock keeps new ones out */
    drop_pages(mm, addr, end, (uintptr_t *)P2V(frames_phys));

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

    mutex_lock(&mm->lock);
    int err = unmap_range(mm, addr, end);
    mutex_unlock(&mm->lock);

    return err;
}

/* madvise(MADV_DONTNEED): the vmas stay, their pages go. anonymous memory reads zero after, a file's reads the file */
int mm_discard(struct mm *mm, uintptr_t addr, size_t size) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;

    size = ALIGN_UP(size, PAGE_SIZE);
    if (size == 0) return 0;

    uintptr_t end = addr + size;
    if (end < addr || end > USER_SPACE_END) return -ENOMEM;

    uintptr_t frames_phys = (uintptr_t)alloc_pages(0);
    if (!frames_phys) return -ENOMEM;

    mutex_lock(&mm->lock);

    int err = 0;

    for (uintptr_t a = addr; a < end;) {
        struct vma *vma = vma_find(mm, a);
        if (!vma) {
            err = -ENOMEM;
            break;
        }

        a = vma->end;
    }

    if (!err) drop_pages(mm, addr, end, (uintptr_t *)P2V(frames_phys));

    mutex_unlock(&mm->lock);

    free_pages((void *)frames_phys, 0);
    return err;
}

/* mremap: a mapping made smaller, or bigger where it is if the address space after it is free, or, if the caller
 * lets it, moved. a moved mapping takes its pages along, not copies of them. one vma, and not the heap's */
long mm_remap(struct mm *mm, uintptr_t old, size_t old_size, size_t new_size, bool may_move) {
    if ((old & (PAGE_SIZE - 1)) || !old_size || !new_size) return -EINVAL;

    old_size = ALIGN_UP(old_size, PAGE_SIZE);
    new_size = ALIGN_UP(new_size, PAGE_SIZE);

    uintptr_t old_end = old + old_size;
    if (old_end < old || old_end > USER_SPACE_END || new_size > USER_SPACE_END) return -EINVAL;

    struct vma *moved = kmalloc(sizeof(struct vma));
    if (!moved) return -ENOMEM;

    long ret = old;

    mutex_lock(&mm->lock);

    struct vma *vma = vma_find(mm, old);

    if (!vma || vma->end < old_end || (vma->start < mm->brk_end && vma->end > mm->brk_start)) {
        ret = -EFAULT;
    } else if (new_size == old_size) {
        /* as it is */
    } else if (new_size < old_size) {
        ret = unmap_range(mm, old + new_size, old_end) ?: (long)old;
    } else if (vma->end == old_end && old + new_size <= USER_SPACE_END && !vma_overlaps(mm, old_end, old + new_size)) {
        vma->end = old + new_size;
    } else if (!may_move) {
        ret = -ENOMEM;
    } else if (new_size > mm->mmap_next || mm->mmap_next - new_size < mm->brk_end ||
               vma_overlaps(mm, mm->mmap_next - new_size, mm->mmap_next)) {
        ret = -ENOMEM;
    } else {
        uintptr_t to = mm->mmap_next - new_size;
        mm->mmap_next = to;

        if (vma->file) fget(vma->file);

        *moved = (struct vma){ .start = to, .end = to + new_size, .prot = vma->prot, .file = vma->file,
                               .off = vma->off + (old - vma->start), .shared = vma->shared };
        RB_INSERT(vma_tree, &mm->vmas, moved);
        moved = NULL;

        /* read only: a write faults, and cow_break() gives it back if the frame is ours alone */
        uint32_t rights = vma->shared ? user_rights(vma->prot) : user_rights((vma->prot & ~PROT_WRITE) | PROT_READ);

        for (size_t off = 0; off < old_size; off += PAGE_SIZE) {
            uintptr_t frame_phys;
            bool writable;

            if (!pmap_lookup(mm->pmap, old + off, &writable)) continue;

            pmap_clear(mm->pmap, old + off, PAGE_SIZE, &frame_phys, 1);

            if (pmap_map(mm->pmap, to + off, frame_phys, PAGE_SIZE, rights) != 0) frame_put(frame_phys);
        }

        pmap_shootdown(mm->pmap, old, old_size);

        ret = unmap_range(mm, old, old_end) ?: (long)to;
    }

    mutex_unlock(&mm->lock);

    kfree(moved);
    return ret;
}

/* mm->lock held. the heap's last vma grows, unless munmap() took that end */
static bool brk_grow(struct mm *mm, uintptr_t new_end) {
    if (new_end > mm->mmap_next || vma_overlaps(mm, mm->brk_end, new_end)) return false;

    struct vma *heap = mm->brk_end > mm->brk_start ? vma_find(mm, mm->brk_end - 1) : NULL;

    if (heap) {
        heap->end = new_end;
        return true;
    }

    heap = kmalloc(sizeof(struct vma));
    if (!heap) return false;

    *heap = (struct vma){ .start = mm->brk_end, .end = new_end, .prot = PROT_READ | PROT_WRITE };
    RB_INSERT(vma_tree, &mm->vmas, heap);

    return true;
}

uintptr_t mm_brk(struct mm *mm, uintptr_t brk) {
    mutex_lock(&mm->lock);

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
    mutex_unlock(&mm->lock);
    return brk;
}

/* fork: the same vmas, and every present page shared read-only until one side writes */
struct mm *mm_fork(struct mm *old) {
    struct mm *mm = mm_create();
    if (!mm) return NULL;

    bool shared = false, ok = true;

    mutex_lock(&old->lock);

    mm->brk_start = old->brk_start;
    mm->brk       = old->brk;
    mm->brk_end   = old->brk_end;
    mm->mmap_next = old->mmap_next;

    struct vma *vma;

    RB_FOREACH(vma, vma_tree, &old->vmas) {
        struct vma *copy = kmalloc(sizeof(struct vma));
        if (!copy) {
            ok = false;
            break;
        }

        *copy = vma_piece(vma, vma->start, vma->end);
        RB_INSERT(vma_tree, &mm->vmas, copy);

        /* both sides read-only: the first write to it copies. a shared mapping stays what it is, both write the same page */
        uint32_t rights = vma->shared ? user_rights(vma->prot) : user_rights(vma->prot & ~PROT_WRITE);

        if (!vma->shared && (vma->prot & PROT_WRITE) && pmap_protect(old->pmap, vma->start, vma->end - vma->start, rights))
            shared = true;

        for (uintptr_t va = vma->start; va < vma->end; va += PAGE_SIZE) {
            uintptr_t phys = pmap_extract(old->pmap, va);
            if (!phys) continue;

            if (pmap_map(mm->pmap, va, phys, PAGE_SIZE, rights) != 0) {
                ok = false;
                break;
            }

            frame_get(phys);
        }

        if (!ok) break;
    }

    /* our other threads may still hold writable entries */
    if (shared) pmap_shootdown(old->pmap, 0, USER_SPACE_END);

    mutex_unlock(&old->lock);

    /* what was shared stays read-only in old: alone again, it gets its rights back on a write */
    if (!ok) {
        mm_put(mm);
        return NULL;
    }

    return mm;
}
