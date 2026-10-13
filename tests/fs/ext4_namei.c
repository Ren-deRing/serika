#include <serika/block.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mutex.h>
#include <serika/printk.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <fs/ext4/ext4.h>
#include <tests/test.h>

#define FILES 300

static struct ext4_sb *sb_of(void) {
    struct file *f;

    if (vfs_open("/mnt", O_RDONLY | O_DIRECTORY, 0, &f) != 0) return NULL;

    struct ext4_sb *sb = ext4_i(f->inode)->sb;

    vfs_close(f);
    return sb;
}

static uint64_t free_blocks(struct ext4_sb *sb) {
    mutex_lock(&sb->meta_lock);
    uint64_t n = ext4_free_blocks_count(sb);
    mutex_unlock(&sb->meta_lock);
    return n;
}

static void name_of(char *path, size_t n, const char *dir, int i) {
    ksnprintf(path, n, "%s/file-with-a-long-name-for-the-test-%04d", dir, i);
}

/* a file with text in it, made with O_EXCL */
static bool make_file(const char *path, const char *text) {
    struct file *f;

    if (vfs_open(path, O_CREAT | O_EXCL | O_WRONLY, 0644, &f) != 0) return false;

    uint64_t pos = 0;
    long n = kernel_write(f, text, strlen(text), &pos);

    vfs_close(f);
    return n == (long)strlen(text);
}

static bool has_text(const char *path, const char *text) {
    struct file *f;
    char buf[256];
    uint64_t pos = 0;

    if (vfs_open(path, O_RDONLY, 0, &f) != 0) return false;

    long n = kernel_read(f, buf, sizeof(buf), &pos);

    vfs_close(f);
    return n == (long)strlen(text) && memcmp(buf, text, n) == 0;
}

static bool exists(const char *path) {
    struct file *f;

    if (vfs_open(path, O_RDONLY | O_PATH | O_NOFOLLOW, 0, &f) != 0) return false;

    vfs_close(f);
    return true;
}

static uint32_t nlink_of(const char *path) {
    struct file *f;

    if (vfs_open(path, O_RDONLY | O_PATH | O_NOFOLLOW, 0, &f) != 0) return 0xffffffff;

    uint32_t n = f->inode->nlink;

    vfs_close(f);
    return n;
}

/* names made and taken away: a directory of several blocks, links, symlinks, renames of files and a dir, rmdir; and after
 * all of it the counts of free blocks and inodes are what they were */
bool test_ext4_namei() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct ext4_sb *sb = sb_of();
    TEST_ASSERT(sb);

    uint64_t blocks0 = free_blocks(sb);
    uint32_t inodes0 = sb->es->free_inodes_count;
    char path[160], text[32];
    struct file *f;

    TEST_ASSERT(vfs_mkdir("/mnt/d1", 0755) == 0);
    TEST_ASSERT(vfs_mkdir("/mnt/d1", 0755) == -EEXIST);
    TEST_ASSERT(nlink_of("/mnt/d1") == 2);

    /* 300 files with long names: the dir has to grow past a block */
    for (int i = 0; i < FILES; i++) {
        name_of(path, sizeof(path), "/mnt/d1", i);
        ksnprintf(text, sizeof(text), "data %d\n", i);
        TEST_ASSERT(make_file(path, text));
    }

    TEST_ASSERT(vfs_open("/mnt/d1", O_RDONLY | O_DIRECTORY, 0, &f) == 0);
    TEST_ASSERT(f->inode->size >= 3 * EXT4_BLOCK_SIZE);
    vfs_close(f);

    name_of(path, sizeof(path), "/mnt/d1", 7);
    TEST_ASSERT(vfs_open(path, O_CREAT | O_EXCL | O_WRONLY, 0644, &f) == -EEXIST);

    for (int i = 0; i < FILES; i++) {
        name_of(path, sizeof(path), "/mnt/d1", i);
        ksnprintf(text, sizeof(text), "data %d\n", i);
        TEST_ASSERT(has_text(path, text));
    }

    /* every third one removed, the room used again by others */
    for (int i = 0; i < FILES; i += 3) {
        name_of(path, sizeof(path), "/mnt/d1", i);
        TEST_ASSERT(vfs_unlink(path) == 0);
        TEST_ASSERT(!exists(path));
    }

    for (int i = 1; i < FILES; i += 3) {
        name_of(path, sizeof(path), "/mnt/d1", i);
        ksnprintf(text, sizeof(text), "data %d\n", i);
        TEST_ASSERT(has_text(path, text));
    }

    for (int i = 0; i < FILES; i += 3) {
        name_of(path, sizeof(path), "/mnt/d1", i);
        ksnprintf(text, sizeof(text), "again %d\n", i);
        TEST_ASSERT(make_file(path, text));
        TEST_ASSERT(has_text(path, text));
    }

    /* a second name for a file, and the first taken away */
    TEST_ASSERT(vfs_link("/mnt/d1/file-with-a-long-name-for-the-test-0001", "/mnt/d1/hard") == 0);
    TEST_ASSERT(nlink_of("/mnt/d1/hard") == 2);
    TEST_ASSERT(vfs_unlink("/mnt/d1/file-with-a-long-name-for-the-test-0001") == 0);
    TEST_ASSERT(nlink_of("/mnt/d1/hard") == 1 && has_text("/mnt/d1/hard", "data 1\n"));

    /* a symlink in the inode, and one in a block; both read back, and the short one is followed */
    TEST_ASSERT(make_file("/mnt/d1/data.txt", "payload\n"));
    TEST_ASSERT(vfs_symlink("data.txt", "/mnt/d1/short") == 0);
    TEST_ASSERT(vfs_open("/mnt/d1/short", O_RDONLY | O_PATH | O_NOFOLLOW, 0, &f) == 0);

    char link[256];

    TEST_ASSERT(S_ISLNK(f->inode->mode) && vfs_readlink(f->inode, link, sizeof(link)) == 8 && memcmp(link, "data.txt", 8) == 0);
    vfs_close(f);
    TEST_ASSERT(has_text("/mnt/d1/short", "payload\n"));

    char longt[201];

    memset(longt, 'x', 200);
    longt[200] = 0;
    TEST_ASSERT(vfs_symlink(longt, "/mnt/d1/long") == 0);
    TEST_ASSERT(vfs_open("/mnt/d1/long", O_RDONLY | O_PATH | O_NOFOLLOW, 0, &f) == 0);
    TEST_ASSERT(f->inode->size == 200 && vfs_readlink(f->inode, link, sizeof(link)) == 200 && memcmp(link, longt, 200) == 0);
    vfs_close(f);

    /* renames: in the same dir, over a name that is there, and a dir to another parent */
    TEST_ASSERT(vfs_rename("/mnt/d1/hard", "/mnt/d1/renamed") == 0);
    TEST_ASSERT(!exists("/mnt/d1/hard") && has_text("/mnt/d1/renamed", "data 1\n"));
    TEST_ASSERT(make_file("/mnt/d1/target", "old\n"));
    TEST_ASSERT(vfs_rename("/mnt/d1/renamed", "/mnt/d1/target") == 0);
    TEST_ASSERT(has_text("/mnt/d1/target", "data 1\n") && !exists("/mnt/d1/renamed"));

    TEST_ASSERT(vfs_mkdir("/mnt/d2", 0755) == 0 && vfs_mkdir("/mnt/d2/sub", 0755) == 0);
    TEST_ASSERT(nlink_of("/mnt/d2") == 3);
    TEST_ASSERT(make_file("/mnt/d2/sub/inside", "in\n"));
    TEST_ASSERT(vfs_rename("/mnt/d2/sub", "/mnt/d3") == 0);
    TEST_ASSERT(nlink_of("/mnt/d2") == 2 && nlink_of("/mnt") >= 4);
    TEST_ASSERT(has_text("/mnt/d3/inside", "in\n"));

    /* its ".." is /mnt now */
    struct file *up, *mnt;

    TEST_ASSERT(vfs_open("/mnt/d3/..", O_RDONLY | O_DIRECTORY, 0, &up) == 0);
    TEST_ASSERT(vfs_open("/mnt", O_RDONLY | O_DIRECTORY, 0, &mnt) == 0);
    TEST_ASSERT(up->inode == mnt->inode);
    vfs_close(up);
    vfs_close(mnt);

    /* a dir with something in it stays; empty it goes */
    TEST_ASSERT(vfs_rmdir("/mnt/d3") == -ENOTEMPTY);
    TEST_ASSERT(vfs_unlink("/mnt/d3/inside") == 0);
    TEST_ASSERT(vfs_rmdir("/mnt/d3") == 0 && !exists("/mnt/d3"));
    TEST_ASSERT(vfs_rmdir("/mnt/d2") == 0);

    /* everything out again */
    for (int i = 0; i < FILES; i++) {
        if (i == 1) continue;                                  /* its name is "target" now */

        name_of(path, sizeof(path), "/mnt/d1", i);
        TEST_ASSERT(vfs_unlink(path) == 0);
    }

    TEST_ASSERT(vfs_unlink("/mnt/d1/target") == 0);
    TEST_ASSERT(vfs_unlink("/mnt/d1/short") == 0 && vfs_unlink("/mnt/d1/data.txt") == 0 && vfs_unlink("/mnt/d1/long") == 0);
    TEST_ASSERT(vfs_rmdir("/mnt/d1") == 0);

    TEST_ASSERT(sb->es->free_inodes_count == inodes0);
    TEST_ASSERT(free_blocks(sb) == blocks0);
    TEST_ASSERT(nlink_of("/mnt") >= 2);
    return true;
}

/* the dir that has an index: new names make it a plain one, and the names it had are all still there */
bool test_ext4_namei_index() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct file *f;
    char path[160];

    TEST_ASSERT(vfs_open("/mnt/many", O_RDONLY | O_DIRECTORY, 0, &f) == 0);
    TEST_ASSERT(ext4_i(f->inode)->flags & EXT4_INODE_INDEX_FL);
    vfs_close(f);

    /* a removal leaves the index alone */
    ksnprintf(path, sizeof(path), "/mnt/many/an-entry-with-a-long-name-%04d", 150);
    TEST_ASSERT(vfs_unlink(path) == 0 && !exists(path));

    TEST_ASSERT(make_file("/mnt/many/new-file", "new\n"));

    TEST_ASSERT(vfs_open("/mnt/many", O_RDONLY | O_DIRECTORY, 0, &f) == 0);
    TEST_ASSERT(!(ext4_i(f->inode)->flags & EXT4_INODE_INDEX_FL));
    vfs_close(f);

    for (int i = 1; i <= 300; i++) {
        if (i == 150) continue;

        ksnprintf(path, sizeof(path), "/mnt/many/an-entry-with-a-long-name-%04d", i);
        TEST_ASSERT(exists(path));
    }

    TEST_ASSERT(has_text("/mnt/many/new-file", "new\n"));

    ksnprintf(path, sizeof(path), "/mnt/many/an-entry-with-a-long-name-%04d", 150);
    TEST_ASSERT(make_file(path, "back\n") && has_text(path, "back\n"));
    return true;
}

/* names that are on the disk: a few are made, the disk is unmounted and mounted again, and they are there; the inodes
 * and blocks of what was deleted are not in use, which fsck says */
bool test_ext4_namei_persist() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct ext4_sb *sb = sb_of();
    TEST_ASSERT(sb);

    TEST_ASSERT(vfs_mkdir("/mnt/keep", 0755) == 0 && vfs_mkdir("/mnt/keep/inner", 0755) == 0);
    TEST_ASSERT(make_file("/mnt/keep/a", "aaa\n") && make_file("/mnt/keep/inner/b", "bbb\n"));
    TEST_ASSERT(vfs_symlink("a", "/mnt/keep/link") == 0 && vfs_link("/mnt/keep/a", "/mnt/keep/inner/a2") == 0);
    TEST_ASSERT(make_file("/mnt/keep/gone", "x\n") && vfs_unlink("/mnt/keep/gone") == 0);

    TEST_ASSERT(ext4_sync_fs(sb) == 0);
    TEST_ASSERT(vfs_umount("/mnt") == 0);
    TEST_ASSERT(ext4_test_mount());

    TEST_ASSERT(has_text("/mnt/keep/a", "aaa\n") && has_text("/mnt/keep/inner/b", "bbb\n"));
    TEST_ASSERT(has_text("/mnt/keep/link", "aaa\n") && has_text("/mnt/keep/inner/a2", "aaa\n"));
    TEST_ASSERT(nlink_of("/mnt/keep/a") == 2 && nlink_of("/mnt/keep") == 3 && nlink_of("/mnt/keep/inner") == 2);
    TEST_ASSERT(!exists("/mnt/keep/gone"));
    TEST_ASSERT(has_text("/mnt/many/new-file", "new\n"));
    return true;
}
