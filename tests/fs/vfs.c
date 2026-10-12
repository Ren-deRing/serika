#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <tests/test.h>

#define VFS_TEST_SIZE 10000

static uint8_t vfs_buf[VFS_TEST_SIZE];

bool test_vfs_basic() {
    struct file *f, *g;
    uint64_t pos;

    TEST_ASSERT(vfs_mkdir("/t", 0755) == 0);
    TEST_ASSERT(vfs_mkdir("/t", 0755) == -EEXIST);
    TEST_ASSERT(vfs_mkdir("/", 0755) == -EEXIST);

    TEST_ASSERT(vfs_open("/t/a", O_RDWR | O_CREAT, 0644, &f) == 0);
    TEST_ASSERT(vfs_open("/t/a", O_RDWR | O_CREAT | O_EXCL, 0644, &g) == -EEXIST);

    /* across pages */
    for (int i = 0; i < VFS_TEST_SIZE; i++) vfs_buf[i] = (uint8_t)(i * 7);

    pos = 0;
    TEST_ASSERT(kernel_write(f, vfs_buf, VFS_TEST_SIZE, &pos) == VFS_TEST_SIZE);
    TEST_ASSERT(pos == VFS_TEST_SIZE && f->inode->size == VFS_TEST_SIZE);

    uint8_t small[100];
    pos = 4090;
    TEST_ASSERT(kernel_read(f, small, sizeof(small), &pos) == sizeof(small));
    TEST_ASSERT(memcmp(small, vfs_buf + 4090, sizeof(small)) == 0);

    pos = VFS_TEST_SIZE;
    TEST_ASSERT(kernel_read(f, small, sizeof(small), &pos) == 0);

    /* a hole reads as zeros */
    pos = 20000;
    TEST_ASSERT(kernel_write(f, "x", 1, &pos) == 1);
    pos = 12000;
    TEST_ASSERT(kernel_read(f, small, sizeof(small), &pos) == sizeof(small));
    for (size_t i = 0; i < sizeof(small); i++) TEST_ASSERT(small[i] == 0);

    /* the same inode through . and .. */
    TEST_ASSERT(vfs_open("/t/../t/./a", O_RDONLY, 0, &g) == 0);
    TEST_ASSERT(g->inode == f->inode);

    pos = 0;
    TEST_ASSERT(kernel_write(g, "x", 1, &pos) == -EBADF);
    vfs_close(g);

    /* bad paths */
    TEST_ASSERT(vfs_open("/t/a/x", O_RDONLY, 0, &g) == -ENOTDIR);
    TEST_ASSERT(vfs_open("/nope/x", O_RDONLY, 0, &g) == -ENOENT);
    TEST_ASSERT(vfs_open("t/a", O_RDONLY, 0, &g) == -ENOENT);
    TEST_ASSERT(vfs_open("/t", O_RDWR, 0, &g) == -EISDIR);
    TEST_ASSERT(vfs_open("/t/a", O_RDONLY | O_DIRECTORY, 0, &g) == -ENOTDIR);

    /* truncate, then append */
    TEST_ASSERT(vfs_open("/t/a", O_WRONLY | O_TRUNC | O_APPEND, 0, &g) == 0);
    TEST_ASSERT(f->inode->size == 0);

    pos = 0;
    TEST_ASSERT(kernel_write(g, "ab", 2, &pos) == 2);
    pos = 0;
    TEST_ASSERT(kernel_write(g, "cd", 2, &pos) == 2);
    TEST_ASSERT(pos == 4 && f->inode->size == 4);
    vfs_close(g);

    pos = 0;
    TEST_ASSERT(kernel_read(f, small, 8, &pos) == 4);
    TEST_ASSERT(memcmp(small, "abcd", 4) == 0);

    vfs_close(f);
    return true;
}

bool test_vfs_initrd() {
    static const char motd[] = "hello from initrd\n";
    struct file *f;
    char buf[64];
    uint64_t pos = 0;

    TEST_ASSERT(vfs_open("/etc/motd", O_RDONLY, 0, &f) == 0);
    TEST_ASSERT(kernel_read(f, buf, sizeof(buf), &pos) == sizeof(motd) - 1);
    TEST_ASSERT(memcmp(buf, motd, sizeof(motd) - 1) == 0);
    vfs_close(f);

    pos = 0;
    TEST_ASSERT(vfs_open("/bin/hello", O_RDONLY, 0, &f) == 0);
    TEST_ASSERT(S_ISREG(f->inode->mode));
    TEST_ASSERT(kernel_read(f, buf, 4, &pos) == 4);
    TEST_ASSERT(memcmp(buf, "\x7f" "ELF", 4) == 0);
    vfs_close(f);
    return true;
}
