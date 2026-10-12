#pragma once

#include "drm_uapi.h"

#include <serika/list.h>
#include <serika/lock.h>
#include <serika/mutex.h>
#include <serika/timer.h>
#include <serika/vfs.h>
#include <serika/wait.h>

#include <stdbool.h>
#include <stddef.h>

/* ids of the objects, which share one number space as in linux. properties and framebuffers have theirs apart */
#define DRM_CRTC_ID      33
#define DRM_ENCODER_ID   34
#define DRM_CONNECTOR_ID 35
#define DRM_PLANE_ID     36

#define DRM_PROP_DPMS    1
#define DRM_PROP_TYPE    2

#define DRM_FORMAT_XRGB8888 0x34325258 /* 'X' 'R' '2' '4' */
#define DRM_FORMAT_ARGB8888 0x34325241 /* 'A' 'R' '2' '4' */

#define DRM_PRIME_CAP_IMPORT 1
#define DRM_PRIME_CAP_EXPORT 2

#define DRM_BO_MAX (256UL << 20) /* a buffer */

/* linux's ioctl numbers: direction, size of the argument, 'd' and the number */
#define DRM_IOC(dir, nr, size) (((dir) << 30) | ((unsigned)(size) << 16) | ('d' << 8) | (nr))
#define DRM_IO(nr)             DRM_IOC(0u, nr, 0)
#define DRM_IOR(nr, type)      DRM_IOC(2u, nr, sizeof(type))
#define DRM_IOW(nr, type)      DRM_IOC(1u, nr, sizeof(type))
#define DRM_IOWR(nr, type)     DRM_IOC(3u, nr, sizeof(type))

#define DRM_IOCTL_VERSION                DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_UNIQUE             DRM_IOWR(0x01, struct drm_unique)
#define DRM_IOCTL_GET_MAGIC              DRM_IOR(0x02, struct drm_auth)
#define DRM_IOCTL_GEM_CLOSE              DRM_IOW(0x09, struct drm_gem_close)
#define DRM_IOCTL_GET_CAP                DRM_IOWR(0x0c, struct drm_get_cap)
#define DRM_IOCTL_SET_CLIENT_CAP         DRM_IOW(0x0d, struct drm_set_client_cap)
#define DRM_IOCTL_AUTH_MAGIC             DRM_IOW(0x11, struct drm_auth)
#define DRM_IOCTL_SET_MASTER             DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER            DRM_IO(0x1f)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD     DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE     DRM_IOWR(0x2e, struct drm_prime_handle)

#define DRM_IOCTL_MODE_GETRESOURCES      DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC           DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC           DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_CURSOR            DRM_IOWR(0xA3, struct drm_mode_cursor)
#define DRM_IOCTL_MODE_GETENCODER        DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR      DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETPROPERTY       DRM_IOWR(0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_SETPROPERTY       DRM_IOWR(0xAB, struct drm_mode_connector_set_property)
#define DRM_IOCTL_MODE_GETFB             DRM_IOWR(0xAD, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_ADDFB             DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB              DRM_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_PAGE_FLIP         DRM_IOWR(0xB0, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_MODE_DIRTYFB           DRM_IOWR(0xB1, struct drm_mode_fb_dirty_cmd)
#define DRM_IOCTL_MODE_CREATE_DUMB       DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB          DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB      DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_GETPLANERESOURCES DRM_IOWR(0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE          DRM_IOWR(0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_SETPLANE          DRM_IOWR(0xB7, struct drm_mode_set_plane)
#define DRM_IOCTL_MODE_ADDFB2            DRM_IOWR(0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES DRM_IOWR(0xB9, struct drm_mode_obj_get_properties)
#define DRM_IOCTL_MODE_OBJ_SETPROPERTY   DRM_IOWR(0xBA, struct drm_mode_obj_set_property)
#define DRM_IOCTL_MODE_CURSOR2           DRM_IOWR(0xBB, struct drm_mode_cursor2)

/* a buffer: pages of its own, not contiguous. mapped by a program through the fake offset, shared as a dma-buf */
struct drm_bo {
    uint32_t    refs;      /* atomic. a handle, a dma-buf, a framebuffer */
    uint32_t    width, height, pitch, bpp;
    size_t      size;      /* bytes, whole pages */
    size_t      npages;
    uintptr_t  *pages;     /* phys of each. buddy memory */
    uint64_t    mmap_page; /* index of its first page in the file, where MAP_DUMB sent the program */
    list_node   node;      /* drm_bos, bo_lock */
};

struct drm_handle {
    list_node      node;
    uint32_t       handle;
    struct drm_bo *bo;
};

struct drm_fb {
    list_node         node;
    uint32_t          id;
    struct drm_file  *owner;
    struct drm_bo    *bo;
    uint32_t          width, height, pitch, format;
};

struct drm_event_node {
    list_node               node;
    struct drm_event_vblank ev;
};

/* what each open of the device keeps (f->priv) */
struct drm_file {
    struct mutex      handle_lock;
    list_node         handles;
    uint32_t          next_handle;

    spinlock_t        ev_lock; /* events. the vblank timer queues them */
    list_node         events;
    struct wait_queue ev_wq;

    bool              universal_planes;
};

/* the one device */
struct drm_dev {
    struct mutex lock; /* the modeset state and the framebuffers. ioctls take it */

    struct drm_mode_modeinfo mode;

    struct drm_fb *crtc_fb; /* what is shown. NULL: nothing set */
    uint32_t       crtc_x, crtc_y;
    uint32_t       dpms;

    list_node      fbs;
    uint32_t       next_fb_id;

    /* a flip waits for the next vblank, and then its event goes to the file. one at a time */
    struct timer            flip_timer;
    bool                    flip_pending; /* atomic */
    struct drm_file        *flip_owner;
    struct drm_event_node  *flip_event;
    uint64_t                flip_user_data;
};

extern struct drm_dev drm_dev;

/* gem.c */
struct drm_bo *drm_bo_create(uint32_t width, uint32_t height, uint32_t bpp);
void drm_bo_get(struct drm_bo *bo);
void drm_bo_put(struct drm_bo *bo);
int  drm_handle_create(struct drm_file *file, struct drm_bo *bo, uint32_t *handle); /* takes a reference of the bo */
struct drm_bo *drm_handle_lookup(struct drm_file *file, uint32_t handle);            /* referenced. NULL: none */
int  drm_handle_close(struct drm_file *file, uint32_t handle);
void drm_handles_release(struct drm_file *file);
long drm_create_dumb(struct drm_file *file, uintptr_t arg);
long drm_map_dumb(struct drm_file *file, uintptr_t arg);
long drm_destroy_dumb(struct drm_file *file, uintptr_t arg);
long drm_gem_close_ioctl(struct drm_file *file, uintptr_t arg);
long drm_prime_handle_to_fd(struct drm_file *file, uintptr_t arg);
long drm_prime_fd_to_handle(struct drm_file *file, uintptr_t arg);
uintptr_t drm_getpage(struct inode *inode, uint64_t index);
size_t drm_bo_read(struct drm_bo *bo, size_t off, void *dst, size_t n); /* bytes copied out of the pages */

/* kms.c */
void drm_kms_init(void);
void drm_kms_release(struct drm_file *file); /* its framebuffers, and a flip it was waiting for */
long drm_kms_ioctl(struct drm_file *file, unsigned long cmd, uintptr_t arg); /* -ENOTTY: not a mode ioctl */
