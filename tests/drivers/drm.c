#include <serika/syscall.h>

#include <tests/test.h>

/* /dev/dri/card0 through libdrm: the display objects, buffers shown on the screen, flips and their events, dma-bufs */
bool test_exec_drm() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/drmtest", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

/* an opengl es context on the gbm device (mesa's llvmpipe), drawing the buffers of a surface that are shown and flipped */
bool test_exec_gbm() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/gbmtest", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}
