#pragma once

#include <stddef.h>
#include <stdint.h>

#define SECTOR_SIZE 512

/* a disk. sectors are 512 bytes whatever the device's own size */
struct blkdev {
    char     name[16];
    uint64_t sectors;
    void    *priv;

    /* buf is kernel memory that is contiguous in physical memory (the direct map: a page, a kmalloc, alloc_pages).
     * may sleep. 0 or -EIO */
    int (*read)(struct blkdev *dev, uint64_t sector, void *buf, size_t count);

    /* the same for buf, which is read. NULL: a disk that cannot be written */
    int (*write)(struct blkdev *dev, uint64_t sector, const void *buf, size_t count);

    /* what was written is on the medium, not in the disk's cache. NULL: it has no cache */
    int (*flush)(struct blkdev *dev);
};

int blk_register(struct blkdev *dev); /* -ENOSPC: the table is full */
struct blkdev *blk_find(const char *name);

/* any number of sectors, any buffer in the direct map: cut into what read() takes */
int blk_read(struct blkdev *dev, uint64_t sector, void *buf, size_t count);

/* the same for blk_write(), which is -EROFS for a disk that has no write() */
int blk_write(struct blkdev *dev, uint64_t sector, const void *buf, size_t count);
int blk_flush(struct blkdev *dev);

void ahci_init(void); /* a block device for each sata disk behind an ahci controller, after pci_init() */
