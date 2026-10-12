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

    /* TODO: write, with ext4 writes */
};

int blk_register(struct blkdev *dev); /* -ENOSPC: the table is full */
struct blkdev *blk_find(const char *name);

/* any number of sectors, any buffer in the direct map: cut into what read() takes */
int blk_read(struct blkdev *dev, uint64_t sector, void *buf, size_t count);
