#include <serika/boot.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/timer.h>

#include <asm/arch.h>

#include "drm_internal.h"

/* the display: a crtc, an encoder, a connector and a primary plane, one of each. what they show is the boot's
 * framebuffer, at the mode the boot set. there is no scanout hardware: setting a crtc, flipping and marking dirty copy
 * the buffer into it. a flip's event comes at the next vblank, which is a timer at 60 Hz */

#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#define DRM_MODE_PAGE_FLIP_ASYNC 0x02
#define DRM_MODE_FB_MODIFIERS    (1 << 1)

#define DRM_MODE_TYPE_PREFERRED  (1 << 3)
#define DRM_MODE_TYPE_DRIVER     (1 << 6)
#define DRM_MODE_FLAG_NHSYNC     (1 << 1)
#define DRM_MODE_FLAG_PVSYNC     (1 << 2)

#define DRM_MODE_CONNECTED       1
#define DRM_MODE_SUBPIXEL_UNKNOWN 1
#define DRM_MODE_CONNECTOR_VIRTUAL 15
#define DRM_MODE_ENCODER_VIRTUAL 5

#define DRM_MODE_PROP_IMMUTABLE  (1 << 2)
#define DRM_MODE_PROP_ENUM       (1 << 3)

#define DRM_MODE_OBJECT_ANY       0
#define DRM_MODE_OBJECT_CRTC      0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0
#define DRM_MODE_OBJECT_ENCODER   0xe0e0e0e0
#define DRM_MODE_OBJECT_PLANE     0xeeeeeeee

#define DRM_PLANE_TYPE_PRIMARY 1

#define DRM_EVENT_FLIP_COMPLETE 0x02

#define DRM_MODE_DPMS_ON  0
#define DRM_MODE_DPMS_OFF 3

#define VBLANK_HZ 60
#define NSEC_PER_SEC 1000000000ULL

struct drm_dev drm_dev;

static const uint32_t plane_formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };

static const struct { uint64_t value; const char *name; } dpms_enum[] = {
    { 0, "On" }, { 1, "Standby" }, { 2, "Suspend" }, { 3, "Off" },
};

static const struct { uint64_t value; const char *name; } plane_type_enum[] = {
    { 0, "Overlay" }, { 1, "Primary" }, { 2, "Cursor" },
};

void drm_kms_init(void) {
    struct drm_dev *d = &drm_dev;
    const struct framebuffer *fb = &g_bootinfo.fb;

    mutex_init(&d->lock);
    list_init(&d->fbs);
    d->next_fb_id = 100;

    /* blanking intervals of a reduced mode: made up from the size, as nothing scans it out */
    uint32_t w = fb->width, h = fb->height;
    struct drm_mode_modeinfo *m = &d->mode;

    memset(m, 0, sizeof(*m));
    m->hdisplay    = w;
    m->hsync_start = w + 48;
    m->hsync_end   = w + 80;
    m->htotal      = w + 160;
    m->vdisplay    = h;
    m->vsync_start = h + 3;
    m->vsync_end   = h + 9;
    m->vtotal      = h + 23;
    m->vrefresh    = VBLANK_HZ;
    m->clock       = (uint64_t)m->htotal * m->vtotal * VBLANK_HZ / 1000;
    m->flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC;
    m->type        = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;

    /* "1280x800" */
    char *p = m->name;
    uint32_t parts[2] = { w, h };

    for (int i = 0; i < 2; i++) {
        char digits[12];
        int n = 0;

        for (uint32_t v = parts[i]; v || !n; v /= 10) digits[n++] = '0' + v % 10;
        while (n) *p++ = digits[--n];
        if (i == 0) *p++ = 'x';
    }
}

/* ---- showing a buffer ---- */

/* the buffer to the framebuffer, from (x, y) in it, as much as the mode and the buffer have. lock held */
static void present(struct drm_dev *d) {
    struct drm_fb *fb = d->crtc_fb;
    const struct framebuffer *out = &g_bootinfo.fb;

    if (!fb || d->dpms != DRM_MODE_DPMS_ON) return;

    uint32_t w = d->mode.hdisplay, h = d->mode.vdisplay;

    if (d->crtc_x >= fb->width || d->crtc_y >= fb->height) return;
    if (w > fb->width - d->crtc_x) w = fb->width - d->crtc_x;
    if (h > fb->height - d->crtc_y) h = fb->height - d->crtc_y;

    for (uint32_t y = 0; y < h; y++) {
        size_t from = (size_t)(d->crtc_y + y) * fb->pitch + (size_t)d->crtc_x * 4;

        drm_bo_read(fb->bo, from, (uint8_t *)out->fb_addr + (size_t)y * out->pitch, (size_t)w * 4);
    }
}

static void fb_free(struct drm_fb *fb) {
    drm_bo_put(fb->bo);
    kfree(fb);
}

static struct drm_fb *fb_find(uint32_t id) {
    for (list_node *n = drm_dev.fbs.next; n != &drm_dev.fbs; n = n->next) {
        struct drm_fb *fb = container_of(n, struct drm_fb, node);

        if (fb->id == id) return fb;
    }

    return NULL;
}

/* a framebuffer of the buffer. lock held */
static long fb_add(struct drm_file *file, uint32_t handle, uint32_t width, uint32_t height, uint32_t pitch,
                   uint32_t offset, uint32_t format, uint32_t *id) {
    if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) return -EINVAL;
    if (!width || !height || offset) return -EINVAL;

    struct drm_bo *bo = drm_handle_lookup(file, handle);
    if (!bo) return -ENOENT;

    /* 32 bits a pixel, and all of it inside the buffer */
    if (bo->bpp != 32 || pitch < (uint64_t)width * 4 || pitch & 3 || (uint64_t)pitch * height > bo->size) {
        drm_bo_put(bo);
        return -EINVAL;
    }

    struct drm_fb *fb = kmalloc(sizeof(*fb));
    if (!fb) {
        drm_bo_put(bo);
        return -ENOMEM;
    }

    fb->owner  = file;
    fb->bo     = bo; /* the lookup's reference is the framebuffer's */
    fb->width  = width;
    fb->height = height;
    fb->pitch  = pitch;
    fb->format = format;
    fb->id     = drm_dev.next_fb_id++;

    list_add_tail(&fb->node, &drm_dev.fbs);

    *id = fb->id;
    return 0;
}

/* what the crtc shows. a framebuffer that was closed while it was shown goes now that it is not. lock held */
static void crtc_set_fb(struct drm_dev *d, struct drm_fb *fb) {
    struct drm_fb *old = d->crtc_fb;

    d->crtc_fb = fb;

    if (old && old != fb && old->closed) fb_free(old);
}

/* a framebuffer that goes while it is shown leaves the crtc without one. lock held */
static void fb_remove(struct drm_fb *fb) {
    list_del(&fb->node);

    if (drm_dev.crtc_fb == fb) drm_dev.crtc_fb = NULL;

    fb_free(fb);
}

/* the program lets go of it and the screen does not: it stays up until the crtc shows another. lock held */
static void fb_close(struct drm_fb *fb) {
    if (drm_dev.crtc_fb != fb) {
        fb_remove(fb);
        return;
    }

    list_del(&fb->node);
    fb->owner  = NULL;
    fb->closed = true;
}

/* ---- vblank and flips ---- */

static uint64_t vblank_period(void) {
    return this_cpu_read(tsc_freq_hz) / VBLANK_HZ;
}

static void flip_fire(struct timer *t) {
    (void)t;

    struct drm_dev *d = &drm_dev;
    struct drm_file *file = d->flip_owner;
    struct drm_event_node *en = d->flip_event;
    uint64_t freq = this_cpu_read(tsc_freq_hz), now = rdtsc();

    d->flip_event = NULL;

    /* free for the next flip before the program can hear about this one */
    __atomic_store_n(&d->flip_pending, false, __ATOMIC_RELEASE);

    if (!en) return;

    en->ev.base.type   = DRM_EVENT_FLIP_COMPLETE;
    en->ev.base.length = sizeof(en->ev);
    en->ev.user_data   = d->flip_user_data;
    en->ev.tv_sec      = now / freq;
    en->ev.tv_usec     = (now % freq) * 1000000 / freq;
    en->ev.sequence    = now / vblank_period();
    en->ev.crtc_id     = DRM_CRTC_ID;

    spin_lock(&file->ev_lock);
    list_add_tail(&en->node, &file->events);
    spin_unlock(&file->ev_lock);

    wake_up(&file->ev_wq);
}

/* a file is going: a flip it asked for is called off. lock held */
static void flip_cancel(struct drm_file *file) {
    struct drm_dev *d = &drm_dev;

    if (d->flip_owner != file) return;

    timer_del(&d->flip_timer); /* the tick has no part of it after this */

    kfree(d->flip_event);
    d->flip_event = NULL;
    d->flip_owner = NULL;
    __atomic_store_n(&d->flip_pending, false, __ATOMIC_RELEASE);
}

void drm_kms_release(struct drm_file *file) {
    struct drm_dev *d = &drm_dev;

    mutex_lock(&d->lock);

    flip_cancel(file);

    for (list_node *n = d->fbs.next; n != &d->fbs;) {
        struct drm_fb *fb = container_of(n, struct drm_fb, node);

        n = n->next;
        if (fb->owner == file) fb_remove(fb);
    }

    mutex_unlock(&d->lock);

    while (!list_empty(&file->events)) {
        struct drm_event_node *en = list_first(&file->events, struct drm_event_node, node);

        list_del(&en->node);
        kfree(en);
    }
}

/* ---- the ioctls ---- */

/* an array to the program: as much as fits, and the count there is, in the pointer's own count */
static long put_array(uint64_t ptr, uint32_t *count, const void *items, uint32_t n, size_t size) {
    uint32_t room = *count;

    *count = n;

    if (!ptr || room < n || !n) return 0;

    return copy_to_user(ptr, items, (size_t)n * size) != 0 ? -EFAULT : 0;
}

static long get_resources(struct drm_file *file, uintptr_t arg) {
    struct drm_mode_card_res r;

    if (copy_from_user(&r, arg, sizeof(r)) != 0) return -EFAULT;

    uint32_t crtc = DRM_CRTC_ID, conn = DRM_CONNECTOR_ID, enc = DRM_ENCODER_ID;
    uint32_t fbs[32], nfbs = 0;

    mutex_lock(&drm_dev.lock);

    for (list_node *n = drm_dev.fbs.next; n != &drm_dev.fbs && nfbs < 32; n = n->next) {
        struct drm_fb *fb = container_of(n, struct drm_fb, node);

        if (fb->owner == file) fbs[nfbs++] = fb->id;
    }

    mutex_unlock(&drm_dev.lock);

    long err = put_array(r.fb_id_ptr, &r.count_fbs, fbs, nfbs, 4);
    if (!err) err = put_array(r.crtc_id_ptr, &r.count_crtcs, &crtc, 1, 4);
    if (!err) err = put_array(r.connector_id_ptr, &r.count_connectors, &conn, 1, 4);
    if (!err) err = put_array(r.encoder_id_ptr, &r.count_encoders, &enc, 1, 4);
    if (err) return err;

    r.min_width  = r.min_height = 0;
    r.max_width  = r.max_height = 16384;

    return copy_to_user(arg, &r, sizeof(r)) != 0 ? -EFAULT : 0;
}

static void crtc_fill(struct drm_mode_crtc *c) {
    struct drm_dev *d = &drm_dev;

    c->set_connectors_ptr = 0;
    c->count_connectors   = 0;
    c->crtc_id            = DRM_CRTC_ID;
    c->fb_id              = d->crtc_fb ? d->crtc_fb->id : 0;
    c->x                  = d->crtc_x;
    c->y                  = d->crtc_y;
    c->gamma_size         = 0;
    c->mode_valid         = d->crtc_fb != NULL;

    if (c->mode_valid) c->mode = d->mode;
    else               memset(&c->mode, 0, sizeof(c->mode));
}

static long get_crtc(uintptr_t arg) {
    struct drm_mode_crtc c;

    if (copy_from_user(&c, arg, sizeof(c)) != 0) return -EFAULT;
    if (c.crtc_id != DRM_CRTC_ID) return -ENOENT;

    mutex_lock(&drm_dev.lock);
    crtc_fill(&c);
    mutex_unlock(&drm_dev.lock);

    return copy_to_user(arg, &c, sizeof(c)) != 0 ? -EFAULT : 0;
}

static long set_crtc(uintptr_t arg) {
    struct drm_mode_crtc c;
    struct drm_dev *d = &drm_dev;

    if (copy_from_user(&c, arg, sizeof(c)) != 0) return -EFAULT;
    if (c.crtc_id != DRM_CRTC_ID) return -ENOENT;

    /* the connectors it is to drive: the one we have, or none to turn it off */
    for (uint32_t i = 0; i < c.count_connectors; i++) {
        uint32_t id;

        if (i >= 8 || copy_from_user(&id, c.set_connectors_ptr + i * 4, 4) != 0) return -EFAULT;
        if (id != DRM_CONNECTOR_ID) return -ENOENT;
    }

    mutex_lock(&d->lock);

    long err = 0;

    if (!c.fb_id) {
        crtc_set_fb(d, NULL);
    } else {
        struct drm_fb *fb = fb_find(c.fb_id);

        if (!fb) err = -ENOENT;
        else if (!c.mode_valid || !c.count_connectors) err = -EINVAL;
        else if (c.mode.hdisplay != d->mode.hdisplay || c.mode.vdisplay != d->mode.vdisplay) err = -EINVAL; /* one mode */
        else if (c.x >= fb->width || c.y >= fb->height ||
                 fb->width - c.x < d->mode.hdisplay || fb->height - c.y < d->mode.vdisplay) err = -ENOSPC;
        else {
            crtc_set_fb(d, fb);
            d->crtc_x  = c.x;
            d->crtc_y  = c.y;

            present(d);
        }
    }

    mutex_unlock(&d->lock);
    return err;
}

static long get_encoder(uintptr_t arg) {
    struct drm_mode_get_encoder e;

    if (copy_from_user(&e, arg, sizeof(e)) != 0) return -EFAULT;
    if (e.encoder_id != DRM_ENCODER_ID) return -ENOENT;

    e.encoder_type    = DRM_MODE_ENCODER_VIRTUAL;
    e.crtc_id         = DRM_CRTC_ID;
    e.possible_crtcs  = 1;
    e.possible_clones = 0;

    return copy_to_user(arg, &e, sizeof(e)) != 0 ? -EFAULT : 0;
}

static long get_connector(uintptr_t arg) {
    struct drm_mode_get_connector c;
    struct drm_dev *d = &drm_dev;

    if (copy_from_user(&c, arg, sizeof(c)) != 0) return -EFAULT;
    if (c.connector_id != DRM_CONNECTOR_ID) return -ENOENT;

    uint32_t enc = DRM_ENCODER_ID, prop = DRM_PROP_DPMS;
    uint64_t value = d->dpms;
    uint32_t props_room = c.count_props;

    long err = put_array(c.modes_ptr, &c.count_modes, &d->mode, 1, sizeof(d->mode));
    if (!err) err = put_array(c.encoders_ptr, &c.count_encoders, &enc, 1, 4);
    if (!err) err = put_array(c.props_ptr, &c.count_props, &prop, 1, 4);

    /* the values go along with the ids: asked for in the same call */
    if (!err && c.prop_values_ptr && props_room >= 1) err = copy_to_user(c.prop_values_ptr, &value, 8) != 0 ? -EFAULT : 0;
    if (err) return err;

    c.encoder_id        = DRM_ENCODER_ID;
    c.connector_type    = DRM_MODE_CONNECTOR_VIRTUAL;
    c.connector_type_id = 1;
    c.connection        = DRM_MODE_CONNECTED;
    c.mm_width          = 0; /* no size: a screen of nothing */
    c.mm_height         = 0;
    c.subpixel          = DRM_MODE_SUBPIXEL_UNKNOWN;

    return copy_to_user(arg, &c, sizeof(c)) != 0 ? -EFAULT : 0;
}

/* the primary plane is listed to a program that asked for the universal planes: else it has only the overlays */
static long get_plane_resources(struct drm_file *file, uintptr_t arg) {
    struct drm_mode_get_plane_res r;

    if (copy_from_user(&r, arg, sizeof(r)) != 0) return -EFAULT;

    uint32_t plane = DRM_PLANE_ID;
    long err = put_array(r.plane_id_ptr, &r.count_planes, &plane, file->universal_planes ? 1 : 0, 4);

    if (err) return err;
    return copy_to_user(arg, &r, sizeof(r)) != 0 ? -EFAULT : 0;
}

static long get_plane(struct drm_file *file, uintptr_t arg) {
    struct drm_mode_get_plane p;

    if (copy_from_user(&p, arg, sizeof(p)) != 0) return -EFAULT;
    if (p.plane_id != DRM_PLANE_ID || !file->universal_planes) return -ENOENT;

    mutex_lock(&drm_dev.lock);
    p.crtc_id = drm_dev.crtc_fb ? DRM_CRTC_ID : 0;
    p.fb_id   = drm_dev.crtc_fb ? drm_dev.crtc_fb->id : 0;
    mutex_unlock(&drm_dev.lock);

    p.possible_crtcs = 1;
    p.gamma_size     = 0;

    long err = put_array(p.format_type_ptr, &p.count_format_types, plane_formats, 2, 4);
    if (err) return err;

    return copy_to_user(arg, &p, sizeof(p)) != 0 ? -EFAULT : 0;
}

/* the properties of an object: the ids and the values */
static long obj_get_properties(uintptr_t arg) {
    struct drm_mode_obj_get_properties o;

    if (copy_from_user(&o, arg, sizeof(o)) != 0) return -EFAULT;

    uint32_t prop = 0;
    uint64_t value = 0;
    uint32_t n = 0;

    /* a program that does not know what the object is (wlroots does not) says any: it is what the id is */
    if (o.obj_type == DRM_MODE_OBJECT_ANY) {
        switch (o.obj_id) {
            case DRM_CRTC_ID:      o.obj_type = DRM_MODE_OBJECT_CRTC; break;
            case DRM_ENCODER_ID:   o.obj_type = DRM_MODE_OBJECT_ENCODER; break;
            case DRM_CONNECTOR_ID: o.obj_type = DRM_MODE_OBJECT_CONNECTOR; break;
            case DRM_PLANE_ID:     o.obj_type = DRM_MODE_OBJECT_PLANE; break;
            default:               return -ENOENT;
        }
    }

    if (o.obj_type == DRM_MODE_OBJECT_CONNECTOR && o.obj_id == DRM_CONNECTOR_ID) {
        prop = DRM_PROP_DPMS;
        value = drm_dev.dpms;
        n = 1;
    } else if (o.obj_type == DRM_MODE_OBJECT_PLANE && o.obj_id == DRM_PLANE_ID) {
        prop = DRM_PROP_TYPE;
        value = DRM_PLANE_TYPE_PRIMARY;
        n = 1;
    } else if (!((o.obj_type == DRM_MODE_OBJECT_CRTC && o.obj_id == DRM_CRTC_ID) ||
                 (o.obj_type == DRM_MODE_OBJECT_ENCODER && o.obj_id == DRM_ENCODER_ID))) {
        return -ENOENT;
    }

    uint32_t room = o.count_props;
    long err = put_array(o.props_ptr, &o.count_props, &prop, n, 4);

    if (!err && o.prop_values_ptr && room >= 1 && n)
        err = copy_to_user(o.prop_values_ptr, &value, 8) != 0 ? -EFAULT : 0;

    if (err) return err;
    return copy_to_user(arg, &o, sizeof(o)) != 0 ? -EFAULT : 0;
}

static long get_property(uintptr_t arg) {
    struct drm_mode_get_property p;

    if (copy_from_user(&p, arg, sizeof(p)) != 0) return -EFAULT;

    const char *name;
    uint32_t flags, n;
    bool dpms = p.prop_id == DRM_PROP_DPMS;

    if (dpms) {
        name  = "DPMS";
        flags = DRM_MODE_PROP_ENUM;
        n     = 4;
    } else if (p.prop_id == DRM_PROP_TYPE) {
        name  = "type";
        flags = DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE;
        n     = 3;
    } else {
        return -ENOENT;
    }

    struct drm_mode_property_enum e[4];
    uint64_t values[4];

    for (uint32_t i = 0; i < n; i++) {
        e[i].value = dpms ? dpms_enum[i].value : plane_type_enum[i].value;
        memset(e[i].name, 0, sizeof(e[i].name));
        strcpy(e[i].name, dpms ? dpms_enum[i].name : plane_type_enum[i].name);
        values[i] = e[i].value;
    }

    long err = put_array(p.values_ptr, &p.count_values, values, n, 8);
    if (!err) err = put_array(p.enum_blob_ptr, &p.count_enum_blobs, e, n, sizeof(e[0]));
    if (err) return err;

    memset(p.name, 0, sizeof(p.name));
    strcpy(p.name, name);
    p.flags = flags;

    return copy_to_user(arg, &p, sizeof(p)) != 0 ? -EFAULT : 0;
}

/* only the connector's dpms is not fixed */
static long set_property(uint32_t obj_id, uint32_t prop_id, uint64_t value) {
    if (obj_id != DRM_CONNECTOR_ID || prop_id != DRM_PROP_DPMS || value > DRM_MODE_DPMS_OFF) return -EINVAL;

    mutex_lock(&drm_dev.lock);
    drm_dev.dpms = value;
    present(&drm_dev); /* on shows it again */
    mutex_unlock(&drm_dev.lock);
    return 0;
}

static long obj_set_property(uintptr_t arg) {
    struct drm_mode_obj_set_property o;

    if (copy_from_user(&o, arg, sizeof(o)) != 0) return -EFAULT;

    return set_property(o.obj_id, o.prop_id, o.value);
}

static long connector_set_property(uintptr_t arg) {
    struct drm_mode_connector_set_property o;

    if (copy_from_user(&o, arg, sizeof(o)) != 0) return -EFAULT;

    return set_property(o.connector_id, o.prop_id, o.value);
}

static long add_fb(struct drm_file *file, uintptr_t arg) {
    struct drm_mode_fb_cmd a;

    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;
    if (a.bpp != 32 || (a.depth != 24 && a.depth != 32)) return -EINVAL;

    mutex_lock(&drm_dev.lock);
    long err = fb_add(file, a.handle, a.width, a.height, a.pitch, 0, a.depth == 32 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888, &a.fb_id);
    mutex_unlock(&drm_dev.lock);

    if (err) return err;
    return copy_to_user(arg, &a, sizeof(a)) != 0 ? -EFAULT : 0;
}

static long add_fb2(struct drm_file *file, uintptr_t arg) {
    struct drm_mode_fb_cmd2 a;

    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    /* one plane, linear */
    if (a.handles[1] || a.pitches[1] || a.offsets[1]) return -EINVAL;
    if ((a.flags & DRM_MODE_FB_MODIFIERS) && a.modifier[0] != 0 && a.modifier[0] != 0x00ffffffffffffffULL) return -EINVAL;

    mutex_lock(&drm_dev.lock);
    long err = fb_add(file, a.handles[0], a.width, a.height, a.pitches[0], a.offsets[0], a.pixel_format, &a.fb_id);
    mutex_unlock(&drm_dev.lock);

    if (err) return err;
    return copy_to_user(arg, &a, sizeof(a)) != 0 ? -EFAULT : 0;
}

static long rm_fb(struct drm_file *file, uintptr_t arg) {
    uint32_t id;

    if (copy_from_user(&id, arg, sizeof(id)) != 0) return -EFAULT;

    mutex_lock(&drm_dev.lock);

    struct drm_fb *fb = fb_find(id);
    long err = !fb ? -ENOENT : fb->owner != file ? -ENOENT : 0;

    if (!err) fb_remove(fb);

    mutex_unlock(&drm_dev.lock);
    return err;
}

/* struct drm_mode_closefb */
static long close_fb(struct drm_file *file, uintptr_t arg) {
    uint32_t a[2];

    if (copy_from_user(a, arg, sizeof(a)) != 0) return -EFAULT;
    if (a[1]) return -EINVAL; /* flags: none are defined */

    mutex_lock(&drm_dev.lock);

    struct drm_fb *fb = fb_find(a[0]);
    long err = !fb ? -ENOENT : fb->owner != file ? -ENOENT : 0;

    if (!err) fb_close(fb);

    mutex_unlock(&drm_dev.lock);
    return err;
}

static long get_fb(struct drm_file *file, uintptr_t arg) {
    struct drm_mode_fb_cmd a;

    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    mutex_lock(&drm_dev.lock);

    struct drm_fb *fb = fb_find(a.fb_id);
    long err = fb ? drm_handle_create(file, fb->bo, &a.handle) : -ENOENT;

    if (!err) {
        a.width  = fb->width;
        a.height = fb->height;
        a.pitch  = fb->pitch;
        a.bpp    = 32;
        a.depth  = fb->format == DRM_FORMAT_ARGB8888 ? 32 : 24;
    }

    mutex_unlock(&drm_dev.lock);

    if (err) return err;
    return copy_to_user(arg, &a, sizeof(a)) != 0 ? -EFAULT : 0;
}

static long dirty_fb(uintptr_t arg) {
    struct drm_mode_fb_dirty_cmd a;

    if (copy_from_user(&a, arg, sizeof(a)) != 0) return -EFAULT;

    mutex_lock(&drm_dev.lock);

    struct drm_fb *fb = fb_find(a.fb_id);

    if (fb && fb == drm_dev.crtc_fb) present(&drm_dev); /* the whole of it, whatever the clips are */

    mutex_unlock(&drm_dev.lock);
    return fb ? 0 : -ENOENT;
}

/* the buffer is shown now, and the event comes at the next vblank: a program waits for it to draw the next */
static long page_flip(struct drm_file *file, uintptr_t arg) {
    struct drm_mode_crtc_page_flip f;
    struct drm_dev *d = &drm_dev;

    if (copy_from_user(&f, arg, sizeof(f)) != 0) return -EFAULT;
    if (f.crtc_id != DRM_CRTC_ID) return -ENOENT;
    if (f.flags & ~(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC)) return -EINVAL;

    struct drm_event_node *en = NULL;

    if (f.flags & DRM_MODE_PAGE_FLIP_EVENT) {
        en = kmalloc(sizeof(*en));
        if (!en) return -ENOMEM;
    }

    mutex_lock(&d->lock);

    long err = 0;
    struct drm_fb *fb = fb_find(f.fb_id);

    if (!fb) err = -ENOENT;
    else if (!d->crtc_fb) err = -EINVAL; /* nothing set to flip from */
    else if (fb->width < d->mode.hdisplay || fb->height < d->mode.vdisplay) err = -EINVAL;
    else if (__atomic_load_n(&d->flip_pending, __ATOMIC_ACQUIRE)) err = -EBUSY;

    if (!err) {
        crtc_set_fb(d, fb);
        d->crtc_x = d->crtc_y = 0;

        present(d);

        if (en) {
            uint64_t period = vblank_period(), now = rdtsc();

            d->flip_owner     = file;
            d->flip_event     = en;
            d->flip_user_data = f.user_data;
            en = NULL;

            __atomic_store_n(&d->flip_pending, true, __ATOMIC_RELEASE);

            d->flip_timer.fn      = flip_fire;
            d->flip_timer.expires = (now / period + 1) * period;
            timer_add(&d->flip_timer);
        }
    }

    mutex_unlock(&d->lock);

    kfree(en);
    return err;
}

#define DRM_MODE_CURSOR_BO   0x01
#define DRM_MODE_CURSOR_MOVE 0x02

/* there is no hardware cursor: hiding it and moving it are done (a program does both after a modeset, and takes
 * a failure of them for a failure of the modeset), and a picture for it is not taken */
static long cursor(unsigned long cmd, uintptr_t arg) {
    struct drm_mode_cursor c;

    /* cursor2 is the same and two more numbers */
    if (copy_from_user(&c, arg, sizeof(c)) != 0) return -EFAULT;
    if (c.crtc_id != DRM_CRTC_ID) return -ENOENT;

    (void)cmd;

    if ((c.flags & DRM_MODE_CURSOR_BO) && c.handle) return -ENXIO;
    return 0;
}

long drm_kms_ioctl(struct drm_file *file, unsigned long cmd, uintptr_t arg) {
    switch (cmd) {
        case DRM_IOCTL_MODE_GETRESOURCES:     return get_resources(file, arg);
        case DRM_IOCTL_MODE_GETCRTC:          return get_crtc(arg);
        case DRM_IOCTL_MODE_SETCRTC:          return set_crtc(arg);
        case DRM_IOCTL_MODE_GETENCODER:       return get_encoder(arg);
        case DRM_IOCTL_MODE_GETCONNECTOR:     return get_connector(arg);
        case DRM_IOCTL_MODE_GETPLANERESOURCES: return get_plane_resources(file, arg);
        case DRM_IOCTL_MODE_GETPLANE:         return get_plane(file, arg);
        case DRM_IOCTL_MODE_OBJ_GETPROPERTIES: return obj_get_properties(arg);
        case DRM_IOCTL_MODE_GETPROPERTY:      return get_property(arg);
        case DRM_IOCTL_MODE_OBJ_SETPROPERTY:  return obj_set_property(arg);
        case DRM_IOCTL_MODE_SETPROPERTY:      return connector_set_property(arg);
        case DRM_IOCTL_MODE_ADDFB:            return add_fb(file, arg);
        case DRM_IOCTL_MODE_ADDFB2:           return add_fb2(file, arg);
        case DRM_IOCTL_MODE_RMFB:             return rm_fb(file, arg);
        case DRM_IOCTL_MODE_CLOSEFB:          return close_fb(file, arg);
        case DRM_IOCTL_MODE_GETFB:            return get_fb(file, arg);
        case DRM_IOCTL_MODE_DIRTYFB:          return dirty_fb(arg);
        case DRM_IOCTL_MODE_PAGE_FLIP:        return page_flip(file, arg);

        /* no cursor and no overlay: a program that has a software cursor and the primary plane only */
        case DRM_IOCTL_MODE_CURSOR:
        case DRM_IOCTL_MODE_CURSOR2:          return cursor(cmd, arg);
        case DRM_IOCTL_MODE_SETPLANE:         return -EINVAL;
    }

    return -ENOTTY;
}
