#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "gbmtest: line %d: %s\n", __LINE__, #c);     \
            return 1;                                                    \
        }                                                                \
    } while (0)

static int flipped;

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, unsigned crtc, void *data) {
    (void)fd; (void)seq; (void)sec; (void)usec; (void)crtc; (void)data;
    flipped++;
}

/* an opengl es context on the gbm device, drawn into the buffers of a gbm surface, which the display shows: what a
 * compositor does, through mesa's software renderer */
int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);

    drmModeResPtr res = drmModeGetResources(fd);
    CHECK(res);

    drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[0]);
    CHECK(conn && conn->count_modes);

    drmModeModeInfo mode = conn->modes[0];
    uint32_t w = mode.hdisplay, h = mode.vdisplay, crtc_id = res->crtcs[0], conn_id = conn->connector_id;

    int fb0 = open("/dev/fb0", O_RDONLY);
    struct stat st;
    CHECK(fb0 >= 0 && fstat(fb0, &st) == 0);

    uint32_t fb_pitch = st.st_size / h;

    struct gbm_device *gbm = gbm_create_device(fd);
    CHECK(gbm);
    printf("gbmtest: backend %s\n", gbm_device_get_backend_name(gbm));

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    CHECK(get_display);

    EGLDisplay dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    CHECK(dpy != EGL_NO_DISPLAY);

    EGLint major, minor;
    CHECK(eglInitialize(dpy, &major, &minor));
    CHECK(eglBindAPI(EGL_OPENGL_ES_API));

    static const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE,
    };
    EGLConfig configs[64], config = NULL;
    EGLint n = 0;

    CHECK(eglChooseConfig(dpy, cfg_attrs, configs, 64, &n) && n > 0);

    for (int i = 0; i < n; i++) {
        EGLint id;

        if (eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &id) && id == (EGLint)GBM_FORMAT_XRGB8888) {
            config = configs[i];
            break;
        }
    }

    CHECK(config);

    struct gbm_surface *gs = gbm_surface_create(gbm, w, h, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    CHECK(gs);

    EGLSurface surf = eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)gs, NULL);
    CHECK(surf != EGL_NO_SURFACE);

    static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
    CHECK(ctx != EGL_NO_CONTEXT);
    CHECK(eglMakeCurrent(dpy, surf, surf, ctx));

    printf("gbmtest: %s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    drmEventContext ev = { .version = 3, .page_flip_handler2 = on_flip };
    struct gbm_bo *prev = NULL;
    uint32_t prev_fb = 0;

    for (int frame = 0; frame < 6; frame++) {
        /* red, green, blue, and again */
        static const float colors[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        const float *c = colors[frame % 3];

        glViewport(0, 0, w, h);
        glClearColor(c[0], c[1], c[2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();

        CHECK(eglSwapBuffers(dpy, surf));

        struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
        CHECK(bo);

        uint32_t handle = gbm_bo_get_handle(bo).u32, stride = gbm_bo_get_stride(bo), fb;

        CHECK(gbm_bo_get_width(bo) == w && gbm_bo_get_height(bo) == h);
        CHECK(drmModeAddFB(fd, w, h, 24, 32, stride, handle, &fb) == 0);

        if (frame == 0) {
            CHECK(drmModeSetCrtc(fd, crtc_id, fb, 0, 0, &conn_id, 1, &mode) == 0);
        } else {
            CHECK(drmModePageFlip(fd, crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0);

            struct pollfd p = { fd, POLLIN, 0 };

            CHECK(poll(&p, 1, 2000) == 1);
            CHECK(drmHandleEvent(fd, &ev) == 0);
        }

        /* the screen has what was drawn */
        uint32_t px = 0;

        CHECK(pread(fb0, &px, 4, (off_t)(h / 2) * fb_pitch + (w / 2) * 4) == 4);
        printf("gbmtest: frame %d, pixel 0x%08x\n", frame, px);
        CHECK((px & 0xffffff) == (((uint32_t)(c[0] * 255) << 16) | ((uint32_t)(c[1] * 255) << 8) | (uint32_t)(c[2] * 255)));

        if (prev) {
            drmModeRmFB(fd, prev_fb);
            gbm_surface_release_buffer(gs, prev);
        }

        prev = bo;
        prev_fb = fb;
    }

    CHECK(flipped == 5);

    printf("gbmtest: ok\n");
    return 0x5e;
}
