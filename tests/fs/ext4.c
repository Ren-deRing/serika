#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <tests/test.h>

/* /usr is the disk (an ext4) when there is one: files, a dir, a link, the way back up, and nothing changed */
bool test_ext4_read() {
    struct file *f, *g;
    uint64_t pos;
    char buf[64];

    if (vfs_open("/usr/hello.txt", O_RDONLY, 0, &f) != 0) {
        printk("ext4: no disk on /usr -- skipped\n");
        return true;
    }

    /* the file */
    static const char text[] = "hello from the disk\n";

    pos = 0;
    TEST_ASSERT(kernel_read(f, buf, sizeof(buf), &pos) == sizeof(text) - 1);
    TEST_ASSERT(memcmp(buf, text, sizeof(text) - 1) == 0);
    TEST_ASSERT(f->inode->size == sizeof(text) - 1 && S_ISREG(f->inode->mode));

    /* the same inode by another way. it has a file system of its own: a file of the ramfs with its number is another */
    TEST_ASSERT(vfs_open("/usr/../usr/./hello.txt", O_RDONLY, 0, &g) == 0);
    TEST_ASSERT(g->inode == f->inode && f->inode->dev != 0);
    vfs_close(g);
    vfs_close(f);

    /* a dir, and out of the file system by its ".." */
    TEST_ASSERT(vfs_open("/usr", O_RDONLY | O_DIRECTORY, 0, &f) == 0);
    TEST_ASSERT(f->inode->dev != 0 && f->inode != vfs_root);
    vfs_close(f);

    TEST_ASSERT(vfs_open("/usr/..", O_RDONLY | O_DIRECTORY, 0, &f) == 0);
    TEST_ASSERT(f->inode == vfs_root);
    vfs_close(f);

    /* the programs of the toolchain, and a link among them */
    TEST_ASSERT(vfs_open("/usr/bin/clang-22", O_RDONLY, 0, &f) == 0);

    pos = 0;
    TEST_ASSERT(kernel_read(f, buf, 4, &pos) == 4);
    TEST_ASSERT(memcmp(buf, "\x7f" "ELF", 4) == 0);

    /* the last bytes (a read stops at the end), and a page of the middle for a mapping */
    TEST_ASSERT(f->inode->size > 10 * PAGE_SIZE);
    pos = f->inode->size - 8;
    TEST_ASSERT(kernel_read(f, buf, 16, &pos) == 8);

    uintptr_t page = vfs_getpage(f->inode, 5);
    TEST_ASSERT(page);
    frame_put(page);
    TEST_ASSERT(vfs_getpage(f->inode, f->inode->size >> 12 << 1) == 0); /* far past the end */
    vfs_close(f);

    TEST_ASSERT(vfs_open("/usr/bin/cc", O_RDONLY | O_PATH | O_NOFOLLOW, 0, &f) == 0);
    TEST_ASSERT(S_ISLNK(f->inode->mode));
    TEST_ASSERT(vfs_readlink(f->inode, buf, sizeof(buf)) == 8 && memcmp(buf, "clang-22", 8) == 0);
    vfs_close(f);

    TEST_ASSERT(vfs_open("/usr/bin/cc", O_RDONLY, 0, &f) == 0); /* followed */
    TEST_ASSERT(S_ISREG(f->inode->mode));
    vfs_close(f);

    /* the entries of a dir */
    TEST_ASSERT(vfs_open("/usr/bin", O_RDONLY | O_DIRECTORY, 0, &f) == 0);

    char dents[1024];
    pos = 0;

    int names = 0;
    bool found = false;

    for (;;) {
        long n = kernel_getdents(f, dents, sizeof(dents), &pos);
        TEST_ASSERT(n >= 0);
        if (n == 0) break;

        for (long off = 0; off < n;) {
            uint16_t reclen = *(uint16_t *)(dents + off + 16);
            const char *name = dents + off + 19;

            names++;
            if (strcmp(name, "clang-22") == 0) found = true;

            off += reclen;
        }
    }

    TEST_ASSERT(found && names > 10);
    vfs_close(f);

    /* not there, and what is there is not made again: the disk is not changed by this test (those that change an ext4 are on
     * the test disk) */
    TEST_ASSERT(vfs_open("/usr/nope", O_RDONLY, 0, &f) == -ENOENT);
    TEST_ASSERT(vfs_open("/usr/hello.txt/x", O_RDONLY, 0, &f) == -ENOTDIR);
    TEST_ASSERT(vfs_open("/usr/hello.txt", O_RDWR | O_CREAT | O_EXCL, 0644, &f) == -EEXIST);
    TEST_ASSERT(vfs_mkdir("/usr/bin", 0755) == -EEXIST);
    TEST_ASSERT(vfs_unlink("/usr/nope") == -ENOENT);
    TEST_ASSERT(vfs_rmdir("/usr/nope") == -ENOENT);
    TEST_ASSERT(vfs_mount("sda", "/usr", "ext4") == -EBUSY);
    return true;
}
