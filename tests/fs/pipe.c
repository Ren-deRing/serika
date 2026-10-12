#include <serika/errno.h>
#include <serika/vfs.h>

#include <tests/test.h>

bool test_pipe() {
    struct file *fds[2];
    char buf[16];
    uint64_t pos = 0;

    TEST_ASSERT(pipe_create(fds, 0) == 0);

    TEST_ASSERT(kernel_write(fds[1], "hello", 5, &pos) == 5);
    TEST_ASSERT(kernel_read(fds[0], buf, 3, &pos) == 3);
    TEST_ASSERT(kernel_read(fds[0], buf + 3, sizeof(buf) - 3, &pos) == 2); /* what is left, not a full buffer */
    TEST_ASSERT(buf[0] == 'h' && buf[4] == 'o');

    /* the writer going is the end for the reader. the pipe outlives the first close */
    vfs_close(fds[1]);
    TEST_ASSERT(kernel_read(fds[0], buf, sizeof(buf), &pos) == 0);

    vfs_close(fds[0]);
    return true;
}
