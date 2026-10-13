# ext4

`fs/ext4/`: ext4 on a block device (`docs/block.md`), a 4 KiB block, mounted on `/usr`. Read only today; this is the
design of the write path. Each stage below is built, tested and checked with `e2fsck` before the next one starts, and
the file says which are done.

Not here, and not in this design: the journal (jbd2: its own sub-project, which goes where the flusher commits), htree
inserts and splits (its own sub-project, see Directories), inline data, bigalloc, meta_bg, quotas, encryption, extended
attributes that are written (they are read over, and kept), `MAP_SHARED` writes reaching the disk, delayed allocation,
and the ext2 and ext3 block map for files that are written (a file with a block map reads, and is `-EOPNOTSUPP` to
write).

## Features

Mounted read-write if the file system has no feature that is not listed: `filetype`, `extent`, `64bit`, `flex_bg`,
`dir_index`, `sparse_super`, `large_file`, `huge_file`, `dir_nlink`, `extra_isize`, `ext_attr`, `metadata_csum`,
`csum_seed`, `uninit_bg` (`gdt_csum`; `metadata_csum` includes it) and `resize_inode` (its blocks are never touched). `needs_recovery`
refuses the mount until there is a journal. Else mounted read only, as today.

## Layers

| file | what it is |
|---|---|
| `csum.c` | crc32c and the checksum of each kind of metadata |
| `buf.c` | the block cache: dirty blocks and their write back |
| `alloc.c` | block and inode bitmaps, group descriptors, free counts, where to allocate |
| `extent.c` | the extent tree: map, insert, remove, split, grow in depth, truncate, punch |
| `inode.c` | reading and writing a file by pages, the inode on the disk, times |
| `dir.c` | the entries of a directory: `struct ext4_dir_ops` |
| `namei.c` | create, mkdir, symlink, link, unlink, dir_retire |
| `flush.c` | the flusher thread, `sync`, `fsync` and the state of the file system |
| `super.c` | mount, the superblock, the inode cache |

## Blocks (`buf.c`)

A block is a page, kept for good as it is today (TODO: let go of blocks when memory is short). The entry has a dirty
flag, and the super block has a list of the dirty ones.

- `ext4_bread()`: the block, to read. A pointer that stays valid.
- `ext4_bdirty()`: the same, to write: the block is put on the list of dirty ones.
- `ext4_bnew()`: a block that is allocated now: zeros, not read from the disk.

A block whose checksum is verified when it is read is verified once: it is the memory copy that is changed from then
on, and its checksum is made when the block is written out (not at each change).

## Locks

The VFS holds a directory's mutex for `lookup`, `create`, `unlink` and `link`, and an inode's for `read`, `write`,
`truncate`, `punch_hole` and `getdents`. `nlink` is changed by the VFS (`drop_link`). Order:

`rename_mutex`, an inode's mutex (one at a time), `sb->meta_lock`, `bcache_lock`, the buddy and slab locks.

A metadata block is changed with `meta_lock` held, from `ext4_start_changes()` to the last `ext4_bdirty()` of the change;
it covers the bitmaps, the group descriptors and the counters of the super block too. It is not held across a disk read
of a file's data. The flusher takes it for the whole of a metadata write out, and a sync only sets VALID while it holds it
with nothing dirty, so a change that is in progress is never taken for done.

## File data

`info->pages[]` as today with a dirty bit for each page. `write` fills the page, marks it dirty and allocates the block
if the page has none (extents are made longer when the block follows the last one). A page is written to the disk by
the flusher. `truncate` and `punch_hole` give back blocks and drop the pages. A page of a hole reads as zeros and is
allocated when written. TODO: the array is as long as the file, for a very large file a radix tree.

Dirty pages are counted for the whole file system. Past 8192 (32 MiB) a writer writes out the dirty pages itself
before it adds one.

## Inodes

`inode_ops` gets one op that is optional, `mark_dirty(inode)`, called with the inode's mutex held when the VFS changes
`nlink`, the mode, a time or the size (`inode_touch`, `drop_link`, chmod, utimes, truncate). `ext4_mark_dirty()` sets a
flag and puts the inode on a list, nothing more. The flusher writes the inode on the disk, with its checksum.
`evict` of an inode whose `nlink` is 0 gives its blocks and the inode back. A crash in between leaves an orphan: the
journal sub-project makes the orphan list.

Allocation of an inode: a directory in the group with the most free inodes (among the groups with room for blocks), a
file in its parent's group. Of a block: next to the last block of the file; else in the group of its inode; else the
first group that has one. A group with `BLOCK_UNINIT` or `INODE_UNINIT` has its bitmap made in memory (the blocks of its
own metadata are used) the first time it is needed, and the flag is cleared when something is allocated in it.

## Directories (`dir.c`)

```c
struct ext4_dir_ops {
    int (*find)(struct inode *dir, const char *name, size_t len, uint32_t *ino, uint8_t *type);
    int (*add)(struct inode *dir, const char *name, size_t len, uint32_t ino, uint8_t type);
    int (*remove)(struct inode *dir, const char *name, size_t len);
};
```

`ext4_lookup`, `create`, `link` and `unlink` go through the ops of the directory, which `INDEX_FL` chooses. Today only
the linear one exists: entries in blocks, a new entry in the first block that has room (an entry is split off the slack
of another, or a block is added to the directory), a removed entry joins the one before it. With `metadata_csum` each
block ends with a 12 byte tail that is not an entry (`inode` 0, `rec_len` 12, `file_type` 0xde, the checksum).

An indexed directory (`INDEX_FL`) is read with the linear scan, as it is today. The first change to one makes it linear:
the entries are gathered into plain blocks, the index blocks are given back and `INDEX_FL` is cleared. This is the stop
gap and goes away with the htree sub-project, whose `find`, `add` and `remove` are the ops of an indexed directory. For
it `ext4_sb` already has `hash_version` and `hash_seed` from the super block.

## Checksums (`csum.c`)

crc32c (software, one table), the raw one ext4 uses with no inversion at the ends (`"123456789"` with seed `~0` is
`0x1cf96d7c`, the complement of the usual `0xe3069283`). The seed is `crc32c(~0, uuid)`, or the one in the super block with
`csum_seed`. Verified when a block is first read, `-EIO` and a message
with the kind if not. Made when the block is written. All of it only if `metadata_csum` is on.

| of | over |
|---|---|
| super block | the first 0x3fc bytes, in the field at 0x3fc |
| group descriptor | `group` (le32), the descriptor with its checksum field as 0; the low 16 bits |
| block and inode bitmap | the bitmap (`blocks_per_group / 8` and `inodes_per_group / 8` bytes); in the descriptor, the high 16 bits if `desc_size` ≥ 64 |
| inode | `ino` then `generation` for the seed, then the inode with its checksum fields as 0, `inode_size` bytes; low in `osd2`, high in `checksum_hi` |
| extent block | the inode's seed, the block up to the tail (4 bytes after the last possible entry) |
| directory block | the inode's seed, the block up to the tail |

## State and errors

- A checksum that does not match, a bitmap that disagrees with the descriptor, a block that is freed twice: a message,
  `-EIO`, `errors` of the super block set. The file system goes on, no panic.
- A write to the disk that fails: the file system is read only from then on (`-EROFS`).
- `-ENOSPC` when nothing is free. There is no reserve (`-m 0` in the image).
- `state` of the super block: VALID is cleared at the first change and set once the flusher finds nothing dirty after a
  `sync`, so `e2fsck` knows an image that was not shut down.

## Flusher (`flush.c`)

A kernel thread (`task_create()`), every 5 s or when woken. It writes, in this order: the dirty data pages of files
whose blocks are allocated; the dirty inodes; the dirty metadata blocks (checksums made as they go); `blk_flush()`.
`sync(2)`, `fsync(2)` (the file's pages and inode first), `fdatasync(2)` and `umount` run the same code in the caller.
`fsync` and `fdatasync` sync the whole file system of the file (its pages and inode first, once those are written). `sync` (162), `syncfs` (306) and `umount2` (166) are there; `umount2` takes flags 0 only, is `-EBUSY` while anything of the
file system is open, mapped or a cwd, and unmounts an ext4 only.

## Stages

1. Foundation (done): `csum.c`, `buf.c`, the flusher, mounted read-write, `sync`, `umount2`, `fsync`; the checksum of what
   is read. The test image.
2. Allocation and files: `alloc.c`, `extent.c`, `write`, `truncate`, `punch_hole`, the inode written, `mark_dirty`.
3. Directories and names: `dir.c`, `namei.c`: create, mkdir, symlink, link, unlink, rmdir, rename through them.
4. Verification and docs: `e2fsck`, mounting the image on the host, this file made to say what is.

## Tests

Stage 1's tests: `ext4 crc32c`, `ext4 csum`, `ext4 csum dir`, `ext4 mount csum`, `ext4 read csum`, `ext4 bflush`, `ext4 sync`,
`ext4 umount`. `make fsck` checks the two images after a run.

- `build/test.img`: 16 MiB, `metadata_csum` on, no journal, made by the Makefile and attached as a second disk (`sdb`).
  Tests that write run there only and are skipped without it, so `/usr` is not touched.
- `tests/fs/ext4_write.c` (mirrors `fs/ext4/`): crc32c of `"123456789"` is `0xe3069283`; allocate and free bitmap round
  trip; a file written sparse, overwritten, truncated, punched; a directory of hundreds of names across blocks;
  link, unlink, rename, symlink; no space; a file written, `sync`, and read again after the cache is dropped.
- `make fsck`: `e2fsck -fn` on `build/test.img` and `build/disk.img`. A stage is finished when it is clean. The user runs
  QEMU; the check is run after it exits.
- The system call tests of `user/glibc/` get write cases on `/usr` in stage 4.
