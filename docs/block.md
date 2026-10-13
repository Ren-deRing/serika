# Block devices

`struct blkdev` (`include/serika/block.h`) is a disk: a name (`sda`), a number of 512 byte sectors and three calls that
may sleep, `read`, `write` and `flush`. `blk_read()`, `blk_write()` and `blk_flush()` check the range and cut a request
into pieces of 64 KiB for the call. A disk without `write` answers `blk_write()` with `-EROFS`, one without `flush`
answers `blk_flush()` with 0. The buffers are kernel memory that is contiguous in physical memory: a page, a kmalloc,
`alloc_pages()`.

`flush` returns when what was written is on the medium and not in the disk's cache. A file system that orders its
writes (a journal before the blocks it covers) puts a flush between them.

## AHCI

`drivers/builtin/block/ahci.c`: the SATA controller of the chipset (class 01:06:01; q35 has one at 00:1f.2). A disk
behind a port is `sda`, `sdb`, ... in the order the ports are found.

- A port that has a device with the phy up and the signature of an ATA disk is used. ATAPI, port multipliers and
  hot plug are not.
- A port gets a page: the command list, the area the device's FISes land in, the command table of slot 0 and a place
  for `IDENTIFY`. The firmware may have left the port running: it is stopped before its list is changed.
- One command at a time, in slot 0, with the port's lock held, so no NCQ: `READ DMA EXT`, `WRITE DMA EXT` and
  `FLUSH CACHE EXT` with 48 bit lba, one PRD entry for the buffer.
- Done is the bit of the command in `PxCI` going out, or a fatal bit in `PxIS` (host bus, interface, task file): then
  the command fails with `-EIO`, and the port is stopped and started again.
- The controller must do 64 bit addresses (`CAP.S64A`). The disk must have 48 bit lba and 512 byte logical sectors.
- One message (msi) for the controller; a controller without one is polled, with a limit of ten seconds.

`virtio_blk` is in `drivers/modules/virtio_blk/`: not built, kept for when modules can be loaded.

## Tests

`block read`: the whole is the same as sector by sector, the ext4 magic, the end of the disk. `block write`: the last
three sectors are written as 2 and 1, flushed and read back, then put back as they were.
