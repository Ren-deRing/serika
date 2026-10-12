#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/syscall.h>

#include "drm_internal.h"

/* buffers: dumb ones, which is all this device has. a program makes one (CREATE_DUMB), maps it through the file at
 * the offset MAP_DUMB gives, and may pass it to another as a dma-buf. the pages are frames with a reference each,
 * so a mapping that outlives the buffer keeps what it maps */

static spinlock_t bo_lock = SPINLOCK_INITIALIZER; /* the list of buffers and the next offset. a leaf */
static list_node  drm_bos = LIST_HEAD_INIT(drm_bos);
static uint64_t   next_mmap_page = 0x100000; /* 4 GiB on: where a buffer is in the file is nothing a program knows */

void drm_bo_get(struct drm_bo *bo) {
    __atomic_add_fetch(&bo->refs, 1, __ATOMIC_RELAXED);
}

void drm_bo_put(struct drm_bo *bo) {
    if (__atomic_sub_fetch(&bo->refs, 1, __ATOMIC_ACQ_REL) != 0) return;

    /* out of the list first, under the lock a lookup holds while it takes a page: after this nobody finds the pages */
    uint64_t flags = spin_lock_irqsave(&bo_lock);
    list_del(&bo->node);
    spin_unlock_irqrestore(&bo_lock, flags);

    for (size_t i = 0; i < bo->npages; i++)
        if (bo->pages[i]) frame_put(bo->pages[i]);

    free_pages((void *)V2P((uintptr_t)bo->pages), SIZE_TO_ORDER(bo->npages * sizeof(uintptr_t)));
    kfree(bo);
}

struct drm_bo *drm_bo_create(uint32_t width, uint32_t height, uint32_t bpp) {
    uint64_t pitch = ALIGN_UP((uint64_t)width * ((bpp + 7) / 8), 64);
    uint64_t size = ALIGN_UP(pitch * height, PAGE_SIZE);

    if (!width || !height || width > 16384 || height > 16384 || size > DRM_BO_MAX) return NULL;

    struct drm_bo *bo = kmalloc(sizeof(*bo));
    if (!bo) return NULL;

    memset(bo, 0, sizeof(*bo));
    bo->refs   = 1;
    bo->width  = width;
    bo->height = height;
    bo->pitch  = pitch;
    bo->bpp    = bpp;
    bo->size   = size;
    bo->npages = size >> PAGE_SHIFT;

    int order = SIZE_TO_ORDER(bo->npages * sizeof(uintptr_t));
    uintptr_t phys = (uintptr_t)alloc_pages(order);

    if (!phys) {
        kfree(bo);
        return NULL;
    }

    bo->pages = (uintptr_t *)P2V(phys);
    memset(bo->pages, 0, ORDER_TO_SIZE(order));

    for (size_t i = 0; i < bo->npages; i++) {
        uintptr_t page = (uintptr_t)alloc_pages(0);

        if (!page) {
            for (size_t j = 0; j < i; j++) frame_put(bo->pages[j]);

            free_pages((void *)phys, order);
            kfree(bo);
            return NULL;
        }

        memset((void *)P2V(page), 0, PAGE_SIZE);
        frame_init(page);
        bo->pages[i] = page;
    }

    uint64_t flags = spin_lock_irqsave(&bo_lock);

    bo->mmap_page = next_mmap_page;
    next_mmap_page += bo->npages + 1; /* a page between, so a mapping past the end of one is not the next's */
    list_add_tail(&bo->node, &drm_bos);

    spin_unlock_irqrestore(&bo_lock, flags);
    return bo;
}

/* the page of the file at index, the mapping's reference taken. the lock holds the buffer while it is */
uintptr_t drm_getpage(struct inode *inode, uint64_t index) {
    (void)inode;

    uintptr_t phys = 0;
    uint64_t flags = spin_lock_irqsave(&bo_lock);

    for (list_node *n = drm_bos.next; n != &drm_bos; n = n->next) {
        struct drm_bo *bo = container_of(n, struct drm_bo, node);

        if (index >= bo->mmap_page && index < bo->mmap_page + bo->npages) {
            phys = bo->pages[index - bo->mmap_page];
            frame_get(phys);
            break;
        }
    }

    spin_unlock_irqrestore(&bo_lock, flags);
    return phys;
}

/* bytes of the buffer from off, as many as there are up to n */
size_t drm_bo_read(struct drm_bo *bo, size_t off, void *dst, size_t n) {
    size_t done = 0;

    if (off >= bo->size) return 0;
    if (n > bo->size - off) n = bo->size - off;

    while (done < n) {
        size_t in_page = (off + done) & (PAGE_SIZE - 1);
        size_t c = PAGE_SIZE - in_page;

        if (c > n - done) c = n - done;

        memcpy((uint8_t *)dst + done, (void *)P2V(bo->pages[(off + done) >> PAGE_SHIFT] + in_page), c);
        done += c;
    }

    return done;
}

/* ---- handles: the numbers a file knows its buffers by ---- */

int drm_handle_create(struct drm_file *file, struct drm_bo *bo, uint32_t *handle) {
    struct drm_handle *h = kmalloc(sizeof(*h));
    if (!h) return -ENOMEM;

    drm_bo_get(bo);
    h->bo = bo;

    mutex_lock(&file->handle_lock);
    h->handle = ++file->next_handle;
    list_add_tail(&h->node, &file->handles);
    mutex_unlock(&file->handle_lock);

    *handle = h->handle;
    return 0;
}

struct drm_bo *drm_handle_lookup(struct drm_file *file, uint32_t handle) {
    struct drm_bo *bo = NULL;

    mutex_lock(&file->handle_lock);

    for (list_node *n = file->handles.next; n != &file->handles; n = n->next) {
        struct drm_handle *h = container_of(n, struct drm_handle, node);

        if (h->handle == handle) {
            bo = h->bo;
            drm_bo_get(bo);
            break;
        }
    }

    mutex_unlock(&file->handle_lock);
    return bo;
}

int drm_handle_close(struct drm_file *file, uint32_t handle) {
    struct drm_handle *found = NULL;

    mutex_lock(&file->handle_lock);

    for (list_node *n = file->handles.next; n != &file->handles; n = n->next) {
        struct drm_handle *h = container_of(n, struct drm_handle, node);

        if (h->handle == handle) {
            list_del(&h->node);
            found = h;
            break;
        }
    }

    mutex_unlock(&file->handle_lock);

    if (!found) return -EINVAL;

    drm_bo_put(found->bo);
    kfree(found);
    return 0;
}

void drm_handles_release(struct drm_file *file) {
    while (!list_empty(&file->handles)) {
        struct drm_handle *h = list_first(&file->handles, struct drm_handle, node);

        list_del(&h->node);
        drm_bo_put(h->bo);
        kfree(h);
    }
}

/* the handle this file already has for the buffer, else a new one */
static int handle_for(struct drm_file *file, struct drm_bo *bo, uint32_t *handle) {
    mutex_lock(&file->handle_lock);

    for (list_node *n = file->handles.next; n != &file->handles; n = n->next) {
        struct drm_handle *h = container_of(n, struct drm_handle, node);

        if (h->bo == bo) {
            *handle = h->handle;
            mutex_unlock(&file->handle_lock);
            return 0;
        }
    }

    mutex_unlock(&file->handle_lock);
    return drm_handle_create(file, bo, handle);
}

/* ---- dumb buffers ---- */

long drm_create_dumb(struct drm_file *file, uintptr_t arg) {
    struct drm_mode_create_dumb a;

    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
    if (a.bpp != 8 && a.bpp != 16 && a.bpp != 24 && a.bpp != 32) return -EINVAL;

    struct drm_bo *bo = drm_bo_create(a.width, a.height, a.bpp);
    if (!bo) return -ENOMEM;

    int err = drm_handle_create(file, bo, &a.handle);

    a.pitch = bo->pitch;
    a.size  = bo->size;

    drm_bo_put(bo); /* the handle has its own */

    if (err) return err;

    if (copy_to_user(arg, &a, sizeof(a)) != 0) {
        drm_handle_close(file, a.handle);
        return -EFAULT;
    }

    return 0;
}

long drm_map_dumb(struct drm_file *file, uintptr_t arg) {
    struct drm_mode_map_dumb a;

    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    struct drm_bo *bo = drm_handle_lookup(file, a.handle);
    if (!bo) return -ENOENT;

    a.offset = bo->mmap_page << PAGE_SHIFT;
    drm_bo_put(bo);

    return copy_to_user(arg, &a, sizeof(a)) != 0 ? -EFAULT : 0;
}

long drm_destroy_dumb(struct drm_file *file, uintptr_t arg) {
    struct drm_mode_destroy_dumb a;

    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    return drm_handle_close(file, a.handle);
}

long drm_gem_close_ioctl(struct drm_file *file, uintptr_t arg) {
    struct drm_gem_close a;

    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    return drm_handle_close(file, a.handle);
}

/* ---- dma-bufs: a buffer as a file, to pass to another program or to import ---- */

#define DMA_BUF_IOCTL_SYNC 0x40086200 /* _IOW('b', 0, struct dma_buf_sync): the pages are plain memory, nothing to flush */

static long dmabuf_ioctl(struct inode *inode, unsigned long cmd, unsigned long arg) {
    (void)inode; (void)arg;

    return cmd == DMA_BUF_IOCTL_SYNC ? 0 : -ENOTTY;
}

/* the same pages as the buffer's own mapping */
static uintptr_t dmabuf_getpage(struct inode *inode, uint64_t index) {
    struct drm_bo *bo = inode->priv;

    if (index >= bo->npages) return 0;

    frame_get(bo->pages[index]);
    return bo->pages[index];
}

static void dmabuf_evict(struct inode *inode) {
    drm_bo_put(inode->priv);
}

static const struct inode_ops dmabuf_ops = {
    .ioctl   = dmabuf_ioctl,
    .getpage = dmabuf_getpage,
    .evict   = dmabuf_evict,
};

long drm_prime_handle_to_fd(struct drm_file *file, uintptr_t arg) {
    struct drm_prime_handle a;

    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
    if (a.flags & ~(O_CLOEXEC | O_RDWR)) return -EINVAL;

    struct drm_bo *bo = drm_handle_lookup(file, a.handle);
    if (!bo) return -ENOENT;

    struct inode *inode = inode_alloc(&dmabuf_ops, S_IFREG | 0600);

    if (!inode) {
        drm_bo_put(bo);
        return -ENOMEM;
    }

    inode->priv = bo; /* the lookup's reference is the dma-buf's now. evict drops it */
    inode->size = bo->size;

    int fd = anon_fd(inode, O_RDWR, a.flags & O_CLOEXEC);
    if (fd < 0) return fd;

    a.fd = fd;

    if (copy_to_user(arg, &a, sizeof(a)) != 0) {
        sys_close(fd);
        return -EFAULT;
    }

    return 0;
}

long drm_prime_fd_to_handle(struct drm_file *file, uintptr_t arg) {
    struct drm_prime_handle a;

    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    struct file *f = fd_get(a.fd);
    if (!f) return -EBADF;

    long err = 0;

    if (f->inode->ops != &dmabuf_ops) err = -EINVAL; /* only ours */
    else                              err = handle_for(file, f->inode->priv, &a.handle);

    vfs_close(f);

    if (err) return err;

    return copy_to_user(arg, &a, sizeof(a)) != 0 ? -EFAULT : 0;
}
