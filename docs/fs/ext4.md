# ext4

`fs/ext4/`: ext4 on a block device (`docs/block.md`), a 4 KiB block, mounted on `/usr`. The data of a file and the names in a directory can be changed (stages 1 to 3). This is the design of the write path. Each stage below is built, tested and checked with `e2fsck` before the next one starts, and
the file says which are done.

Not here, and not in this design: the journal (jbd2: its own sub-project, which goes where the flusher commits), htree
inserts and splits (its own sub-project, see Directories), inline data, bigalloc, meta_bg, quotas, encryption, extended
attributes that are written (they are read over, and kept), `MAP_SHARED` writes reaching the disk, delayed allocation,
and the ext2 and ext3 block map for files that are written (a file with a block map reads, and is `-EOPNOTSUPP` to
write).

## Features

Mounted read-write if the file system has no feature that is not listed: `filetype`, `extent`, `64bit`, `flex_bg`,
`dir_index`, `sparse_super`, `large_file`, `huge_file`, `dir_nlink`, `extra_isize`, `ext_attr`, `metadata_csum`,
`csum_seed`, `uninit_bg` with `metadata_csum` (`gdt_csum` alone is a crc16 of the descriptors, which is not written: read only) and
`resize_inode` (its blocks are never touched). `needs_recovery`
refuses the mount until there is a journal. Else mounted read only, as today.

## Layers

| file | what it is |
|---|---|
| `csum.c` | crc32c and the checksum of each kind of metadata |
| `buf.c` | the block cache: dirty blocks and their write back |
| `alloc.c` | block and inode bitmaps, group descriptors, free counts, where to allocate |
| `extent.c` | the extent tree: find, map, insert (merge, split, grow in depth), remove (cut, drop empty nodes) |
| `pages.c` | the pages of a file in the cache: a radix tree, the dirty bit, drop and write out |
| `inode.c` | `read`, `write`, `truncate`, `punch_hole`, the inode on the disk, the queue of dirty inodes |
| `dir.c` | the entries of a directory: lookup, getdents, add, remove, empty, `.` and `..` |
| `namei.c` | create (files and dirs), symlink, link, unlink, dir_retire, the end of an inode with no names |
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

The pages of a file that are in the cache are a radix tree (`pages.c`): nodes of 512 slots, a page each, that grow in
height with the file (1 level: 512 pages, 2: 1 GiB, 3: 512 GiB, 4: all a file can have), and exist only where there is a
page, so a hole costs nothing. A slot of a leaf is the frame of the page, with the dirty bit in bit 0. A file is at most
16 TiB (`EXT4_FILE_MAX`: its blocks are numbered with 32 bits).

`write` makes a block for every page of the range that has none (`alloc_holes()`: a zero page in the cache for each first,
so a block that was just allocated is never read, then `ext4_alloc_blocks()` near the block before it and
`ext4_ext_insert()`, which makes the last extent longer when the block follows it), copies into the pages and marks them
dirty. If the blocks run out the bytes that have blocks are written (a short write) and the next call is `-ENOSPC`.
A page is written to its block by the flusher or `fsync`. `truncate` zeros the tail of the last page, drops the pages and
`ext4_ext_remove()` gives back the blocks; a longer size is a hole. `punch_hole` zeros the ends of the range that cut a
page, drops the pages inside and removes their blocks, which splits an extent in the middle. A page of a hole reads as
zeros.

Not written: a file with a block map and not extents (`-EOPNOTSUPP`), and a range inside an unwritten extent (`-EOPNOTSUPP`:
a block would be allocated over the extent's own; the conversion is a TODO).

Dirty pages are counted for the whole file system. Past 8192 (32 MiB) a writer writes out the dirty pages of its own file
before it goes on.

## Extent tree (`extent.c`)

A path from the root (`i_block`, 4 entries) to the leaf, found for a block; the nodes are blocks of the cache read with
their checksum. `ext4_ext_next()` gives the extent of a block or the first after it (across leaves), `ext4_map()` is
built on it. `ext4_ext_insert()` merges with the extent before or after when the blocks follow, else puts a new one in
the leaf; a full leaf is split in halves, and a full parent is split first; a full root gets a block that takes its
entries and holds the one entry itself (the tree is a level deeper). `ext4_ext_remove()` cuts the range off the extents:
the front, the tail, the middle (two extents; if the second cannot be made nothing changes), or the whole entry, and a
node that has no entries is given back (`ext4_bforget()` so the cache does not write it over what the block becomes) and
its entry in the parent goes, up to the root, which stays as an empty leaf. After each change the keys of the index
nodes are made equal to the first entry of their child, which e2fsck wants. `ext4_ext_insert` and `ext4_ext_remove` keep
`nblocks` (the data and the nodes), with `meta_lock` held.

## Blocks of the disk (`alloc.c`)

The bitmap of a group (read with its checksum checked), its descriptor and the counter of the super block change
together; the super block is written by the next sync (`super_dirty`). `ext4_alloc_blocks(goal, want)` gives up to `want`
blocks in a row from the first free bit at or after the goal, in that group or the next ones; a group with
`BLOCK_UNINIT` has its bitmap made when it is first needed (the copy of the super block and the descriptors, its own bitmaps
and inode table if they are in it, the bits past the end of the last group), and the flag goes. `ext4_free_blocks()` clears the bits and says so on
the log if one was clear.

## Inodes

`inode_ops` has one op that is optional, `mark_dirty(inode)`, called by the VFS with the inode's mutex held when it changes
`nlink`, the mode, a time or the size (`inode_touch`, `drop_link`, chmod, utimes). `ext4_mark_dirty()` sets a flag and
puts the inode on a list (its own spin lock), nothing more; the change that allocates or frees blocks queues its inode
before it lets go of `meta_lock`. The flusher writes the pages of each inode on the list and then the inode (the fields
the kernel has: mode, links, size, times, flags, `i_block`, `i_blocks`; the owner and the attributes stay as they are), with
its checksum.
`evict` of an inode whose `nlink` is 0 gives its blocks and the inode back. A crash in between leaves an orphan: the
journal sub-project makes the orphan list.

Allocation of an inode: a directory in the group with the most free inodes (among the groups with room for blocks), a
file in its parent's group. Of a block: next to the last block of the file; else in the group of its inode; else the
first group that has one. A group with `BLOCK_UNINIT` or `INODE_UNINIT` has its bitmap made in memory (the blocks of its
own metadata are used) the first time it is needed, and the flag is cleared when something is allocated in it.

## Inodes of the files (`alloc.c`, `namei.c`)

`ext4_alloc_inode()` takes a free bit of the inode bitmap: a directory in the group with the most free inodes, a file in
its parent's group. A group with `INODE_UNINIT` has its bitmap made then (the bits past its inodes set), its inode table
zeroed if `ITABLE_ZEROED` is not set, and `itable_unused` set; with `gdt_csum` or `metadata_csum` that count says where
the inodes in use end and is lowered when one past it is taken. The inode on the disk is made new and whole (the slot
zeroed, the mode, one link or two for a dir, times and `crtime`, the empty root of extents, a generation, the checksum)
and read back through the cache like any other, so a new inode is the same as one that was always there.

An idle inode has one reference, the cache's. The VFS lets one go for each name taken away (`drop_link()`, or the `iput()`
of `rmdir`), so `unlink` gives one back while the inode keeps another name (`nlink > 1`), and with the last name it is the
cache's that goes: the inode is evicted when nobody has it open any more. `link` takes none. When the last one is gone `ext4_evict()` writes back the
pages and the inode if they are dirty, and deletes an inode whose `nlink` is 0: its pages are dropped, its blocks
(`ext4_ext_remove()`; a dir's blocks too) and the inode go back, the inode on the disk gets `dtime`.

## Directories (`dir.c`)

`ext4_lookup()`, `ext4_dir_add()` and `ext4_dir_remove()` are the places where an index will go: today they walk the linear
blocks. A new entry goes in the first block with room for it (an entry is split off the slack of another, an entry that is
not in use is used again, or a block is added to the directory); a removed entry joins the one before it, the first of a
block is marked as not in use. With `metadata_csum` each block ends with a 12 byte tail that is not an entry (`inode` 0,
`rec_len` 12, `file_type` 0xde, the checksum), which is never a place for a name.

An indexed directory (`INDEX_FL`) is read with the linear scan, and a name taken out of it leaves the index right (only its
leaf changes). A name put in makes it a plain one first (`ext4_dir_make_linear()`): the index blocks are entries that are
not in use, so `INDEX_FL` off is all it takes, and, with checksums, the 12 byte tail that an index block does not have (the
last entry gives up 12 bytes). This is the stop gap until the index is kept: the htree sub-project makes `add` and
`remove` follow it. For that `ext4_sb` already has `hash_version` and `hash_seed` from the super block.

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

A kernel thread (`task_create()`), every 5 s. It writes, in this order: for each inode on the queue the dirty data pages and
the inode; the dirty metadata blocks (checksums made as they go); `blk_flush()`; the super block if the counters changed.
`sync(2)`, `fsync(2)` (the file's pages and inode first), `fdatasync(2)` and `umount` run the same code in the caller.
`fsync` and `fdatasync` sync the whole file system of the file (its pages and inode first, once those are written). `sync` (162), `syncfs` (306) and `umount2` (166) are there; `umount2` takes flags 0 only, is `-EBUSY` while anything of the
file system is open, mapped or a cwd, and unmounts an ext4 only.

## Stages

1. Foundation (done): `csum.c`, `buf.c`, the flusher, mounted read-write, `sync`, `umount2`, `fsync`; the checksum of what
   is read. The test image.
2. Allocation and files (done): `alloc.c`, `extent.c`, `pages.c`, `write`, `truncate`, `punch_hole`, the inode written, `mark_dirty`.
3. Directories and names (done): `dir.c`, `namei.c`: create, mkdir, symlink, link, unlink, rmdir, rename through them.
4. Verification and docs: `e2fsck`, mounting the image on the host, this file made to say what is.

## Tests

Stage 3's: `ext4 namei`, `ext4 namei index`, `ext4 namei persist`. Stage 2's: `ext4 inode`, `ext4 alloc`, `ext4 extent`, `ext4 write`, `ext4 persist`. Stage 1's tests: `ext4 crc32c`, `ext4 csum`, `ext4 csum dir`, `ext4 mount csum`, `ext4 read csum`, `ext4 bflush`, `ext4 sync`,
`ext4 umount`. `make fsck` checks the two images after a run.

- `build/test.img`: 400 MiB (four groups; the three after the first start `BLOCK_UNINIT`), `metadata_csum` on, no journal, made by the Makefile and attached as a second disk (`sdb`).
  Tests that write run there only and are skipped without it, so `/usr` is not touched.
- `tests/fs/ext4_write.c` (mirrors `fs/ext4/`): crc32c of `"123456789"` is `0xe3069283`; allocate and free bitmap round
  trip; a file written sparse, overwritten, truncated, punched; a directory of hundreds of names across blocks;
  link, unlink, rename, symlink; no space; a file written, `sync`, and read again after the cache is dropped.
- `make fsck`: `e2fsck -fn` on `build/test.img` and `build/disk.img`. A stage is finished when it is clean. The user runs
  QEMU; the check is run after it exits.
- The system call tests of `user/glibc/` get write cases on `/usr` in stage 4.
