#include <serika/block.h>
#include <serika/errno.h>
#include <serika/string.h>

#define BLK_MAX 8

/* the most one read() or write() is asked for: one request is a command with one buffer */
#define BLK_CHUNK (64 * 1024 / SECTOR_SIZE)

static struct blkdev *devs[BLK_MAX];

int blk_register(struct blkdev *dev) {
    for (int i = 0; i < BLK_MAX; i++) {
        if (devs[i]) continue;

        devs[i] = dev;
        return 0;
    }

    return -ENOSPC;
}

struct blkdev *blk_find(const char *name) {
    for (int i = 0; i < BLK_MAX; i++)
        if (devs[i] && strcmp(devs[i]->name, name) == 0) return devs[i];

    return NULL;
}

int blk_read(struct blkdev *dev, uint64_t sector, void *buf, size_t count) {
    if (sector > dev->sectors || count > dev->sectors - sector) return -EINVAL;

    while (count) {
        size_t n = count < BLK_CHUNK ? count : BLK_CHUNK;

        int err = dev->read(dev, sector, buf, n);
        if (err) return err;

        sector += n;
        buf = (char *)buf + n * SECTOR_SIZE;
        count -= n;
    }

    return 0;
}

int blk_write(struct blkdev *dev, uint64_t sector, const void *buf, size_t count) {
    if (!dev->write) return -EROFS;
    if (sector > dev->sectors || count > dev->sectors - sector) return -EINVAL;

    while (count) {
        size_t n = count < BLK_CHUNK ? count : BLK_CHUNK;

        int err = dev->write(dev, sector, buf, n);
        if (err) return err;

        sector += n;
        buf = (const char *)buf + n * SECTOR_SIZE;
        count -= n;
    }

    return 0;
}

int blk_flush(struct blkdev *dev) {
    return dev->flush ? dev->flush(dev) : 0;
}
