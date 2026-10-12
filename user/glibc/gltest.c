#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static const char *vs_src =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *fs_src =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "void main() { gl_FragColor = vec4(uv.x, uv.y, 0.25, 1.0); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("gltest: shader: %s\n", log);
        return 0;
    }

    return s;
}

/* the exit code is the number of the check that failed */
int main(void) {
    /* no display and no window: a context of its own on the cpu */
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!get_display) return 1;

    EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY) return 2;

    EGLint major, minor;
    if (!eglInitialize(dpy, &major, &minor)) return 3;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return 4;

    const EGLint cfg_attrs[] = { EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;

    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n < 1) return 5;

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) return 6;

    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return 7;

    printf("gltest: %s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    /* a texture to draw into */
    GLuint tex, fbo;

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 8;

    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 9;

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) return 10;

    glUseProgram(prog);

    /* two triangles over the whole target */
    static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1 };

    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    static unsigned char px[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);

    if (glGetError() != GL_NO_ERROR) return 11;

    /* the color is (u, v, 0.25): the corners and the middle say whether it came out right */
    #define AT(x, y) (&px[((y) * W + (x)) * 4])
    if (AT(0, 0)[0] > 8 || AT(0, 0)[1] > 8) return 12;
    if (AT(W - 1, 0)[0] < 240 || AT(W - 1, 0)[1] > 8) return 13;
    if (AT(0, H - 1)[0] > 8 || AT(0, H - 1)[1] < 240) return 14;
    if (abs(AT(W / 2, H / 2)[0] - 128) > 10 || abs(AT(W / 2, H / 2)[1] - 128) > 10) return 15;
    if (abs(AT(W / 2, H / 2)[2] - 64) > 6 || AT(W / 2, H / 2)[3] != 255) return 16;

    FILE *out = fopen("/tmp/gltest.ppm", "wb");
    if (out) {
        fprintf(out, "P6\n%d %d\n255\n", W, H);

        for (int i = 0; i < W * H; i++) fwrite(&px[i * 4], 1, 3, out);

        fclose(out);
    }

    printf("gltest: ok\n");
    return 0x5e;
}
