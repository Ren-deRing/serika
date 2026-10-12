#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_W 800
#define MAX_H 600

static const char *vs_src =
    "attribute vec2 pos;\n"
    "attribute vec3 col;\n"
    "uniform float angle;\n"
    "varying vec3 c;\n"
    "void main() {\n"
    "    float s = sin(angle), k = cos(angle);\n"
    "    gl_Position = vec4(pos.x * k - pos.y * s, pos.x * s + pos.y * k, 0.0, 1.0);\n"
    "    c = col;\n"
    "}\n";

static const char *fs_src =
    "precision mediump float;\n"
    "varying vec3 c;\n"
    "void main() { gl_FragColor = vec4(c, 1.0); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "fbdemo: shader: %s\n", log);
        exit(1);
    }

    return s;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);

    return t.tv_sec + t.tv_nsec / 1e9;
}

/* draws a turning triangle with llvmpipe, and puts each frame on the framebuffer. fbdemo [seconds] */
int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 10;

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        perror("fbdemo: /dev/fb0");
        return 1;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;

    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        perror("fbdemo: ioctl");
        return 1;
    }

    if (var.bits_per_pixel != 32) {
        fprintf(stderr, "fbdemo: %u bits per pixel is not what this draws\n", var.bits_per_pixel);
        return 1;
    }

    uint8_t *fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        perror("fbdemo: mmap");
        return 1;
    }

    int w = var.xres < MAX_W ? var.xres : MAX_W;
    int h = var.yres < MAX_H ? var.yres : MAX_H;

    printf("fbdemo: screen %ux%u, drawing %dx%d for %.0f s\n", var.xres, var.yres, w, h, seconds);

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;

    EGLint major, minor, n = 0;
    EGLConfig cfg;
    const EGLint cfg_attrs[] = { EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor) || !eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "fbdemo: no egl\n");
        return 1;
    }

    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "fbdemo: no context\n");
        return 1;
    }

    printf("fbdemo: %s\n", glGetString(GL_RENDERER));

    GLuint tex, rbo_fbo;

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &rbo_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rbo_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(prog, 0, "pos");
    glBindAttribLocation(prog, 1, "col");
    glLinkProgram(prog);
    glUseProgram(prog);

    GLint angle = glGetUniformLocation(prog, "angle");

    static const GLfloat verts[] = { 0.0f, 0.8f, -0.7f, -0.6f, 0.7f, -0.6f };
    static const GLfloat cols[]  = { 1, 0.2f, 0.2f, 0.2f, 1, 0.2f, 0.2f, 0.4f, 1 };

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, cols);
    glViewport(0, 0, w, h);

    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return 1;

    double start = now(), report = start;
    long frames = 0, last = 0;

    while (now() - start < seconds) {
        double t = now() - start;

        glClearColor(0.1f + 0.1f * sinf(t), 0.1f, 0.2f + 0.1f * cosf(t * 1.3f), 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1f(angle, t * 1.5f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);

        /* the picture is upside down for a screen, and its pixels are rgba where the screen has its own order */
        for (int y = 0; y < h; y++) {
            const uint8_t *src = px + (size_t)(h - 1 - y) * w * 4;
            uint32_t *dst = (uint32_t *)(fb + (size_t)y * fix.line_length);

            for (int x = 0; x < w; x++, src += 4)
                dst[x] = ((uint32_t)src[0] << var.red.offset) | ((uint32_t)src[1] << var.green.offset) |
                         ((uint32_t)src[2] << var.blue.offset);
        }

        frames++;

        if (now() - report >= 1) {
            printf("fbdemo: %ld frames in the last second\n", frames - last);
            last = frames;
            report = now();
        }
    }

    printf("fbdemo: %ld frames, %.1f per second\n", frames, frames / (now() - start));
    return 0;
}
