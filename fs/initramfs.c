#include <serika/boot.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <stdbool.h>

/* cpio newc: ascii hex fields, name and data padded to 4 */
struct cpio_newc {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
};

static bool hex8(const char *s, uint32_t *out) {
    uint32_t v = 0;

    for (int i = 0; i < 8; i++) {
        char c = s[i];

        if (c >= '0' && c <= '9')      v = (v << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (c - 'A' + 10);
        else return false;
    }

    *out = v;
    return true;
}

static int unpack_file(const char *path, uint32_t mode, const void *data, size_t size) {
    struct file *f;

    int err = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, mode & 07777, &f);
    if (err) return err;

    uint64_t pos = 0;

    while (pos < size) {
        long r = kernel_write(f, (const uint8_t *)data + pos, size - pos, &pos);
        if (r <= 0) {
            vfs_close(f);
            return r ? (int)r : -EIO;
        }
    }

    vfs_close(f);
    return 0;
}

/* the data is the target, without a nul */
static int unpack_symlink(const char *path, const uint8_t *data, size_t size) {
    if (size == 0 || size >= PATH_MAX) return -EINVAL;

    char *target = kmalloc(size + 1);
    if (!target) return -ENOMEM;

    memcpy(target, data, size);
    target[size] = '\0';

    int err = vfs_symlink(target, path);

    kfree(target);
    return err;
}

/* task context: the vfs sleeps */
void initramfs_unpack(void) {
    if (!g_bootinfo.initrd.size) {
        printk("initramfs: no initrd\n");
        return;
    }

    const uint8_t *base = (const uint8_t *)P2V(g_bootinfo.initrd.phys_base);
    size_t size = g_bootinfo.initrd.size;

    char *path = kmalloc(PATH_MAX);
    if (!path) {
        printk("initramfs: no memory for a path\n");
        return;
    }

    uint32_t files = 0, dirs = 0, links = 0;
    size_t off = 0;

    while (off + sizeof(struct cpio_newc) <= size) {
        const struct cpio_newc *h = (const struct cpio_newc *)(base + off);
        uint32_t mode, filesize, namesize;

        if (memcmp(h->magic, "070701", 6) != 0 ||
            !hex8(h->mode, &mode) || !hex8(h->filesize, &filesize) || !hex8(h->namesize, &namesize)) {
            printk("initramfs: bad header at 0x%lx -- not cpio newc?\n", off);
            break;
        }

        size_t name_at = off + sizeof(struct cpio_newc);
        size_t data_at = ALIGN_UP(name_at + namesize, 4);
        const char *name = (const char *)(base + name_at);

        if (namesize == 0 || data_at > size || filesize > size - data_at || name[namesize - 1] != '\0') {
            printk("initramfs: entry at 0x%lx runs past the end -- truncated?\n", off);
            break;
        }

        if (strcmp(name, "TRAILER!!!") == 0) break;

        off = ALIGN_UP(data_at + filesize, 4);

        /* "./bin/x", "bin/x" and "/bin/x" are all /bin/x. "." is the root */
        while (name[0] == '.' && name[1] == '/') name += 2;
        while (name[0] == '/') name++;

        if (name[0] == '\0' || strcmp(name, ".") == 0) continue;

        if (strlen(name) + 2 > PATH_MAX) {
            printk("initramfs: skipping a name of %lu bytes -- too long\n", strlen(name));
            continue;
        }

        path[0] = '/';
        strcpy(path + 1, name);

        int err;

        if (S_ISDIR(mode)) {
            err = vfs_mkdir(path, mode & 07777);
            if (err == -EEXIST) err = 0;
            if (!err) dirs++;
        } else if (S_ISREG(mode)) {
            err = unpack_file(path, mode, base + data_at, filesize);
            if (!err) files++;
        } else if (S_ISLNK(mode)) {
            err = unpack_symlink(path, base + data_at, filesize);
            if (!err) links++;
        } else {
            printk("initramfs: skipping %s -- type 0%o not supported\n", path, mode & S_IFMT);
            continue;
        }

        if (err) printk("initramfs: %s -- error %d\n", path, err);
    }

    kfree(path);
    printk("initramfs: %u files, %u dirs, %u links\n", files, dirs, links);
}
