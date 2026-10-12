#include <serika/boot.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/poll.h>
#include <serika/preempt.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/task.h>
#include <serika/vfs.h>

#include "drm_internal.h"

/* /dev/dri/card0: the display of the boot's framebuffer as a drm device (see docs/drm.md). it says it is vkms, the
 * virtual driver of linux, which is what it is, and which mesa takes to need no gpu */

#define DRM_NAME "vkms"
#define DRM_DATE "20240101"
#define DRM_DESC "Virtual Kernel Mode Setting (serika)"

#define DRM_CAP_DUMB_BUFFER            0x1
#define DRM_CAP_VBLANK_HIGH_CRTC       0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH   0x3
#define DRM_CAP_DUMB_PREFER_SHADOW     0x4
#define DRM_CAP_PRIME                  0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC    0x6
#define DRM_CAP_ASYNC_PAGE_FLIP        0x7
#define DRM_CAP_CURSOR_WIDTH           0x8
#define DRM_CAP_CURSOR_HEIGHT          0x9
#define DRM_CAP_ADDFB2_MODIFIERS       0x10
#define DRM_CAP_PAGE_FLIP_TARGET       0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT   0x12

#define DRM_CLIENT_CAP_STEREO_3D       1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC          3
#define DRM_CLIENT_CAP_ASPECT_RATIO    4

static int drm_open_file(struct file *f) {
    struct drm_file *file = kmalloc(sizeof(*file));
    if (!file) return -ENOMEM;

    memset(file, 0, sizeof(*file));
    mutex_init(&file->handle_lock);
    list_init(&file->handles);
    spin_lock_init(&file->ev_lock);
    list_init(&file->events);
    wait_queue_init(&file->ev_wq);

    f->priv = file;
    return 0;
}

static void drm_release_file(struct file *f) {
    struct drm_file *file = f->priv;

    drm_kms_release(file);
    drm_handles_release(file);
    kfree(file);
}

/* a string for the program: its length, and as much of it as its buffer has */
static long put_string(uint64_t ptr, uint64_t *len, const char *s) {
    uint64_t room = *len, n = strlen(s);

    *len = n;

    if (!ptr || room < n) return 0;

    return copy_to_user(ptr, s, n) != 0 ? -EFAULT : 0;
}

static long drm_version(uintptr_t arg) {
    struct drm_version v;

    if (copy_from_user(&v, arg, sizeof(v)) != 0) return -EFAULT;

    v.version_major = 1;
    v.version_minor = 0;
    v.version_patchlevel = 0;

    long err = put_string(v.name, &v.name_len, DRM_NAME);
    if (!err) err = put_string(v.date, &v.date_len, DRM_DATE);
    if (!err) err = put_string(v.desc, &v.desc_len, DRM_DESC);
    if (err) return err;

    return copy_to_user(arg, &v, sizeof(v)) != 0 ? -EFAULT : 0;
}

static long drm_get_unique(uintptr_t arg) {
    struct drm_unique u;

    if (copy_from_user(&u, arg, sizeof(u)) != 0) return -EFAULT;

    long err = put_string(u.unique, &u.unique_len, "serika");
    if (err) return err;

    return copy_to_user(arg, &u, sizeof(u)) != 0 ? -EFAULT : 0;
}

static long drm_get_cap(uintptr_t arg) {
    struct drm_get_cap c;

    if (copy_from_user(&c, arg, sizeof(c)) != 0) return -EFAULT;

    switch (c.capability) {
        case DRM_CAP_DUMB_BUFFER:          c.value = 1; break;
        case DRM_CAP_DUMB_PREFERRED_DEPTH: c.value = 24; break;
        case DRM_CAP_PRIME:                c.value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; break;
        case DRM_CAP_TIMESTAMP_MONOTONIC:  c.value = 1; break;
        case DRM_CAP_CRTC_IN_VBLANK_EVENT: c.value = 1; break;

        case DRM_CAP_VBLANK_HIGH_CRTC:
        case DRM_CAP_DUMB_PREFER_SHADOW:
        case DRM_CAP_ASYNC_PAGE_FLIP:
        case DRM_CAP_ADDFB2_MODIFIERS:
        case DRM_CAP_PAGE_FLIP_TARGET:
            c.value = 0;
            break;

        /* the cursor plane is not here: its size is for a program that has one */
        case DRM_CAP_CURSOR_WIDTH:
        case DRM_CAP_CURSOR_HEIGHT:
            c.value = 64;
            break;

        default: return -EINVAL;
    }

    return copy_to_user(arg, &c, sizeof(c)) != 0 ? -EFAULT : 0;
}

/* universal planes are taken; atomic is not here, and a program that asked goes on with the legacy calls */
static long drm_set_client_cap(struct drm_file *file, uintptr_t arg) {
    struct drm_set_client_cap c;

    if (copy_from_user(&c, arg, sizeof(c)) != 0) return -EFAULT;

    switch (c.capability) {
        case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
            if (c.value > 1) return -EINVAL;

            file->universal_planes = c.value;
            return 0;

        case DRM_CLIENT_CAP_ASPECT_RATIO:
            return c.value > 1 ? -EINVAL : 0;

        case DRM_CLIENT_CAP_STEREO_3D:
        case DRM_CLIENT_CAP_ATOMIC:
            return -EOPNOTSUPP;
    }

    return -EINVAL;
}

/* there is no one to be master against: everyone is, and the magic is only a number */
static long drm_get_magic(uintptr_t arg) {
    struct drm_auth a = { .magic = 1 };

    return copy_to_user(arg, &a, sizeof(a)) != 0 ? -EFAULT : 0;
}

static long drm_ioctl(struct file *f, unsigned long cmd, unsigned long arg) {
    struct drm_file *file = f->priv;

    switch (cmd) {
        case DRM_IOCTL_VERSION:             return drm_version(arg);
        case DRM_IOCTL_GET_UNIQUE:          return drm_get_unique(arg);
        case DRM_IOCTL_GET_MAGIC:           return drm_get_magic(arg);
        case DRM_IOCTL_AUTH_MAGIC:
        case DRM_IOCTL_SET_MASTER:
        case DRM_IOCTL_DROP_MASTER:         return 0;
        case DRM_IOCTL_GET_CAP:             return drm_get_cap(arg);
        case DRM_IOCTL_SET_CLIENT_CAP:      return drm_set_client_cap(file, arg);
        case DRM_IOCTL_GEM_CLOSE:           return drm_gem_close_ioctl(file, arg);
        case DRM_IOCTL_PRIME_HANDLE_TO_FD:  return drm_prime_handle_to_fd(file, arg);
        case DRM_IOCTL_PRIME_FD_TO_HANDLE:  return drm_prime_fd_to_handle(file, arg);
        case DRM_IOCTL_MODE_CREATE_DUMB:    return drm_create_dumb(file, arg);
        case DRM_IOCTL_MODE_MAP_DUMB:       return drm_map_dumb(file, arg);
        case DRM_IOCTL_MODE_DESTROY_DUMB:   return drm_destroy_dumb(file, arg);
    }

    return drm_kms_ioctl(file, cmd, arg);
}

/* events, whole ones: a flip has completed. wait for one unless the file is O_NONBLOCK */
static long drm_read(struct file *f, void *buf, size_t n) {
    struct drm_file *file = f->priv;
    size_t done = 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&file->ev_lock);

        while (!list_empty(&file->events)) {
            struct drm_event_node *en = list_first(&file->events, struct drm_event_node, node);

            if (n - done < en->ev.base.length) break;

            memcpy((char *)buf + done, &en->ev, en->ev.base.length);
            done += en->ev.base.length;

            list_del(&en->node);
            spin_unlock_irqrestore(&file->ev_lock, flags);
            kfree(en);
            flags = spin_lock_irqsave(&file->ev_lock);
        }

        spin_unlock_irqrestore(&file->ev_lock, flags);

        if (done) return done;
        if (n < sizeof(struct drm_event_vblank)) return -EINVAL;
        if (f->flags & O_NONBLOCK) return -EAGAIN;

        int err = wait_event_interruptible(file->ev_wq, !list_empty(&file->events));
        if (err) return err;
    }
}

static uint32_t drm_poll(struct file *f, struct poll_table *pt) {
    struct drm_file *file = f->priv;

    poll_wait(pt, &file->ev_wq);

    return list_empty(&file->events) ? 0 : POLLIN;
}

static const struct inode_ops drm_ops = {
    .open_file    = drm_open_file,
    .release_file = drm_release_file,
    .ioctl_file   = drm_ioctl,
    .poll_file    = drm_poll,
    .getpage      = drm_getpage,
    .stream       = true,
    .stream_read  = drm_read,
};

void drm_devices_create(void) {
    const struct framebuffer *fb = &g_bootinfo.fb;

    /* what a buffer is: xrgb8888 */
    if (!fb->fb_addr || fb->bpp != 32 || fb->red_shift != 16 || fb->green_shift != 8 || fb->blue_shift != 0) {
        printk("drm: no framebuffer in xrgb8888 -- no /dev/dri/card0\n");
        return;
    }

    drm_kms_init();

    struct inode *inode = inode_alloc(&drm_ops, S_IFCHR | 0666);
    if (!inode) panic("drm: unable to create /dev/dri/card0 -- out of memory?");

    inode->rdev = 226 << 8; /* 226:0 */

    if (vfs_mkdir("/dev/dri", 0755) != 0) panic("drm: unable to create /dev/dri -- out of memory?");

    int err = vfs_link_inode("/dev/dri/card0", inode);
    if (err) panic("drm: unable to create /dev/dri/card0 -- error %d", err);

    iput(inode);

    printk("drm: card0, %ux%u\n", fb->width, fb->height);
}
