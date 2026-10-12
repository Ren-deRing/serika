#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "drmtest: line %d: %s (errno %d)\n", __LINE__, #c, errno); \
            return 1;                                                    \
        }                                                                \
    } while (0)

struct buf {
    uint32_t handle, pitch, fb;
    uint64_t size;
    uint32_t *map;
};

static uint64_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

static int make_buf(int fd, uint32_t w, uint32_t h, struct buf *b) {
    struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };

    CHECK(drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) == 0);
    CHECK(c.pitch >= w * 4 && c.size >= (uint64_t)c.pitch * h);

    struct drm_mode_map_dumb m = { .handle = c.handle };

    CHECK(drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) == 0);

    b->handle = c.handle;
    b->pitch  = c.pitch;
    b->size   = c.size;
    b->map    = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
    CHECK(b->map != MAP_FAILED);

    CHECK(drmModeAddFB(fd, w, h, 24, 32, c.pitch, c.handle, &b->fb) == 0);
    return 0;
}

static void paint(struct buf *b, uint32_t w, uint32_t h, uint32_t base) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            b->map[y * (b->pitch / 4) + x] = base ^ ((x * 255 / w) << 16 | (y * 255 / h) << 8 | ((x ^ y) & 0xff));
}

/* the pixel the screen has: /dev/fb0 is the same memory */
static uint32_t screen_px(int fb0, uint32_t pitch, uint32_t x, uint32_t y) {
    uint32_t px = 0;

    if (pread(fb0, &px, 4, (off_t)y * pitch + x * 4) != 4) return 0xdeadbeef;
    return px;
}

static int flipped;
static uint64_t flip_data;
static unsigned flip_crtc;

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, unsigned crtc, void *data) {
    (void)fd; (void)seq; (void)sec; (void)usec;
    flipped++;
    flip_data = (uintptr_t)data;
    flip_crtc = crtc;
}

static int wait_flip(int fd) {
    drmEventContext ctx = { .version = 3, .page_flip_handler2 = on_flip };
    struct pollfd p = { fd, POLLIN, 0 };

    int before = flipped;

    CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN));
    CHECK(drmHandleEvent(fd, &ctx) == 0);
    CHECK(flipped == before + 1);
    return 0;
}

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);

    int fb0 = open("/dev/fb0", O_RDONLY);
    CHECK(fb0 >= 0);

    /* who we are talking to */
    drmVersionPtr v = drmGetVersion(fd);
    CHECK(v && strcmp(v->name, "vkms") == 0);
    drmFreeVersion(v);

    uint64_t cap;
    CHECK(drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) == 0 && cap == 1);
    CHECK(drmGetCap(fd, DRM_CAP_PRIME, &cap) == 0 && cap == 3);
    CHECK(drmGetCap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap) == 0 && cap == 1);
    CHECK(drmGetCap(fd, 0x7777, &cap) == -1);

    CHECK(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0); /* legacy only */

    /* the planes are not listed until asked for */
    drmModePlaneResPtr pr = drmModeGetPlaneResources(fd);
    CHECK(pr && pr->count_planes == 0);
    drmModeFreePlaneResources(pr);

    CHECK(drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0);

    pr = drmModeGetPlaneResources(fd);
    CHECK(pr && pr->count_planes == 1);

    drmModePlanePtr plane = drmModeGetPlane(fd, pr->planes[0]);
    CHECK(plane && plane->count_formats == 2 && plane->possible_crtcs == 1);
    drmModeFreePlane(plane);

    /* its type is the primary's */
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, pr->planes[0], DRM_MODE_OBJECT_PLANE);
    CHECK(props && props->count_props == 1);

    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[0]);
    CHECK(prop && strcmp(prop->name, "type") == 0 && props->prop_values[0] == DRM_PLANE_TYPE_PRIMARY);
    CHECK(prop->flags & DRM_MODE_PROP_ENUM && prop->count_enums == 3);
    drmModeFreeProperty(prop);
    drmModeFreeObjectProperties(props);
    drmModeFreePlaneResources(pr);

    /* the display */
    drmModeResPtr res = drmModeGetResources(fd);
    CHECK(res && res->count_crtcs == 1 && res->count_connectors == 1 && res->count_encoders == 1);

    drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[0]);
    CHECK(conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes == 1 && conn->count_encoders == 1);
    CHECK(conn->connector_type == DRM_MODE_CONNECTOR_VIRTUAL);

    drmModeModeInfo mode = conn->modes[0];
    uint32_t w = mode.hdisplay, h = mode.vdisplay, crtc_id = res->crtcs[0], conn_id = conn->connector_id;

    printf("drmtest: %s, %ux%u\n", mode.name, w, h);

    drmModeEncoderPtr enc = drmModeGetEncoder(fd, conn->encoder_id);
    CHECK(enc && enc->encoder_type == DRM_MODE_ENCODER_VIRTUAL && enc->possible_crtcs == 1);
    drmModeFreeEncoder(enc);

    drmModeCrtcPtr crtc = drmModeGetCrtc(fd, crtc_id);
    CHECK(crtc && crtc->buffer_id == 0 && !crtc->mode_valid); /* nothing set yet */
    drmModeFreeCrtc(crtc);

    /* a buffer, drawn into, shown */
    struct buf a, b;

    if (make_buf(fd, w, h, &a) || make_buf(fd, w, h, &b)) return 1;

    paint(&a, w, h, 0x00000000);
    paint(&b, w, h, 0x00ff00ff);

    CHECK(drmModeSetCrtc(fd, crtc_id, a.fb, 0, 0, &conn_id, 1, &mode) == 0);

    uint32_t pitch_fb = w * 4; /* the screen's pitch is the width of the mode on this boot: see /dev/fb0 */
    (void)pitch_fb;

    struct stat st;
    CHECK(fstat(fb0, &st) == 0 && st.st_size >= (off_t)w * h * 4);

    uint32_t fb_pitch = st.st_size / h;

    for (uint32_t i = 0; i < 8; i++) {
        uint32_t x = (i * 157) % w, y = (i * 89) % h;

        CHECK(screen_px(fb0, fb_pitch, x, y) == a.map[y * (a.pitch / 4) + x]);
    }

    crtc = drmModeGetCrtc(fd, crtc_id);
    CHECK(crtc && crtc->buffer_id == a.fb && crtc->mode_valid && crtc->mode.hdisplay == w);
    drmModeFreeCrtc(crtc);

    /* a mode that is not ours */
    drmModeModeInfo bad = mode;
    bad.hdisplay = w / 2;
    CHECK(drmModeSetCrtc(fd, crtc_id, a.fb, 0, 0, &conn_id, 1, &bad) != 0);

    /* flips: the screen has the buffer at once, the event comes at the next vblank */
    uint64_t t0 = now_ms();

    CHECK(drmModePageFlip(fd, crtc_id, b.fb, DRM_MODE_PAGE_FLIP_EVENT, (void *)0x1234) == 0);
    CHECK(screen_px(fb0, fb_pitch, 3, 5) == b.map[5 * (b.pitch / 4) + 3]);
    int busy = drmModePageFlip(fd, crtc_id, a.fb, DRM_MODE_PAGE_FLIP_EVENT, NULL); /* libdrm gives -errno or -1 */
    CHECK(busy < 0 && (busy == -EBUSY || errno == EBUSY));
    CHECK(wait_flip(fd) == 0);
    CHECK(flip_data == 0x1234 && flip_crtc == crtc_id);

    /* one every 16 ms or so, 30 of them */
    t0 = now_ms();

    for (int i = 0; i < 30; i++) {
        struct buf *next = i & 1 ? &b : &a;

        CHECK(drmModePageFlip(fd, crtc_id, next->fb, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0);
        CHECK(wait_flip(fd) == 0);
    }

    uint64_t took = now_ms() - t0;

    printf("drmtest: 30 flips in %lu ms\n", (unsigned long)took);
    CHECK(took >= 30 * 12 && took < 30 * 40);

    /* no event asked for: nothing comes */
    CHECK(drmModePageFlip(fd, crtc_id, a.fb, 0, NULL) == 0);

    struct pollfd p = { fd, POLLIN, 0 };

    CHECK(poll(&p, 1, 100) == 0);

    /* a buffer to another program: a dma-buf, the same pages */
    int prime;
    CHECK(drmPrimeHandleToFD(fd, b.handle, DRM_CLOEXEC | DRM_RDWR, &prime) == 0);
    CHECK(lseek(prime, 0, SEEK_END) == (off_t)b.size);

    uint32_t *alias = mmap(NULL, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, prime, 0);
    CHECK(alias != MAP_FAILED);
    CHECK(alias[7] == b.map[7] && alias[(h - 1) * (b.pitch / 4) + w - 1] == b.map[(h - 1) * (b.pitch / 4) + w - 1]);

    alias[11] = 0xabcdef01;
    CHECK(b.map[11] == 0xabcdef01);

    uint32_t again;
    CHECK(drmPrimeFDToHandle(fd, prime, &again) == 0 && again == b.handle);

    /* from a second opening, which has handles of its own */
    int fd2 = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    uint32_t other;

    CHECK(fd2 >= 0 && drmPrimeFDToHandle(fd2, prime, &other) == 0);

    uint32_t fb2;
    CHECK(drmModeAddFB(fd2, w, h, 24, 32, b.pitch, other, &fb2) == 0);
    CHECK(drmModeRmFB(fd, fb2) != 0); /* not ours */
    close(fd2); /* its framebuffer and its handle go with it */

    /* errors: no such objects, and a buffer too small for the framebuffer */
    CHECK(drmModeGetConnector(fd, 999) == NULL);
    CHECK(drmModeGetCrtc(fd, 999) == NULL);
    CHECK(drmModeRmFB(fd, 9999) != 0);

    uint32_t junk;
    CHECK(drmModeAddFB(fd, w, h * 2, 24, 32, a.pitch, a.handle, &junk) != 0);

    /* dpms, which is the one property that changes */
    CHECK(drmModeConnectorSetProperty(fd, conn_id, 1, DRM_MODE_DPMS_ON) == 0);
    CHECK(drmModeConnectorSetProperty(fd, conn_id, 1, 99) != 0);

    /* a framebuffer taken off while shown leaves the crtc without one */
    CHECK(drmModeRmFB(fd, b.fb) == 0);
    CHECK(drmModeRmFB(fd, a.fb) == 0);

    crtc = drmModeGetCrtc(fd, crtc_id);
    CHECK(crtc && crtc->buffer_id == 0);
    drmModeFreeCrtc(crtc);

    /* leave something to look at */
    struct buf last;
    if (make_buf(fd, w, h, &last)) return 1;

    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t bar = x * 8 / w;
            static const uint32_t colors[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x202020 };

            last.map[y * (last.pitch / 4) + x] = y < h / 8 ? (x * 255 / w) * 0x010101 : colors[bar];
        }

    CHECK(drmModeSetCrtc(fd, crtc_id, last.fb, 0, 0, &conn_id, 1, &mode) == 0);
    CHECK(drmModeSetCrtc(fd, crtc_id, 0, 0, 0, NULL, 0, NULL) == 0); /* off */

    CHECK(drmModeSetCrtc(fd, crtc_id, last.fb, 0, 0, &conn_id, 1, &mode) == 0);

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    printf("drmtest: ok\n");
    return 0x5e;
}
