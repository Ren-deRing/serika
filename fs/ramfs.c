#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/list.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/vfs.h>

struct ramfs_dirent {
    list_node     node;
    struct inode *inode; /* the link's reference */
    size_t        len;
    char          name[]; /* not terminated */
};

struct ramfs_node {
    /* regular */
    uintptr_t     *pages;   /* phys of each page, 0: a hole */
    size_t         npages;  /* slots in pages, fills its buddy block */

    /* symlink */
    char          *target;

    /* dir */
    list_node      entries;
    struct inode  *parent;  /* a dir's, our reference. the root is its own, without one */
};

static const struct inode_ops ramfs_ops;

static struct ramfs_node *node_of(struct inode *inode) {
    return inode->priv;
}

static struct inode *ramfs_new(uint32_t mode, struct inode *parent) {
    struct ramfs_node *node = kmalloc(sizeof(struct ramfs_node));
    if (!node) return NULL;

    struct inode *inode = inode_alloc(&ramfs_ops, mode);
    if (!inode) {
        kfree(node);
        return NULL;
    }

    memset(node, 0, sizeof(*node));
    list_init(&node->entries);
    if (S_ISDIR(mode)) {
        node->parent = parent ? parent : inode; /* the root is its own */
        if (parent) iget(parent);
    }

    inode->priv  = node;
    inode->nlink = S_ISDIR(mode) ? 2 : 1;

    return inode;
}

struct inode *ramfs_anon_file(void) {
    return ramfs_new(S_IFREG | 0600, NULL);
}

struct inode *ramfs_root(void) {
    return ramfs_new(S_IFDIR | 0755, NULL);
}

static int ramfs_lookup(struct inode *dir, const char *name, size_t len, struct inode **out) {
    struct ramfs_node *node = node_of(dir);

    if (len == 2 && name[0] == '.' && name[1] == '.') {
        struct inode *parent = __atomic_load_n(&node->parent, 2); /* __ATOMIC_ACQUIRE: a rename changes it under another lock */

        iget(parent);
        *out = parent;
        return 0;
    }

    for (list_node *n = node->entries.next; n != &node->entries; n = n->next) {
        struct ramfs_dirent *d = container_of(n, struct ramfs_dirent, node);

        if (d->len == len && memcmp(d->name, name, len) == 0) {
            iget(d->inode);
            *out = d->inode;
            return 0;
        }
    }

    return -ENOENT;
}

static int ramfs_create(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out) {
    if (!S_ISREG(mode) && !S_ISDIR(mode)) return -EINVAL;
    if (dir->nlink == 0) return -ENOENT; /* retired */

    struct ramfs_dirent *d = kmalloc(sizeof(struct ramfs_dirent) + len);
    if (!d) return -ENOMEM;

    struct inode *inode = ramfs_new(mode, dir);
    if (!inode) {
        kfree(d);
        return -ENOMEM;
    }

    d->inode = inode;
    d->len   = len;
    memcpy(d->name, name, len);
    list_add_tail(&d->node, &node_of(dir)->entries);

    if (S_ISDIR(mode)) dir->nlink++;
    inode_touch(dir);

    iget(inode);
    *out = inode;
    return 0;
}

static int ramfs_symlink(struct inode *dir, const char *name, size_t len, const char *target, struct inode **out) {
    size_t tlen = strlen(target);
    if (tlen >= PATH_MAX) return -ENAMETOOLONG;
    if (dir->nlink == 0) return -ENOENT;

    struct ramfs_dirent *d = kmalloc(sizeof(struct ramfs_dirent) + len);
    char *copy = kmalloc(tlen + 1);
    struct inode *inode = (d && copy) ? ramfs_new(S_IFLNK | 0777, dir) : NULL;

    if (!inode) {
        kfree(d);
        kfree(copy);
        return -ENOMEM;
    }

    memcpy(copy, target, tlen + 1);
    node_of(inode)->target = copy;
    inode->size = tlen;

    d->inode = inode;
    d->len   = len;
    memcpy(d->name, name, len);
    list_add_tail(&d->node, &node_of(dir)->entries);
    inode_touch(dir);

    iget(inode);
    *out = inode;
    return 0;
}

static long ramfs_readlink(struct inode *inode, char *buf, size_t n) {
    size_t len = inode->size;
    if (len > n) len = n;

    memcpy(buf, node_of(inode)->target, len);
    return len;
}

static int ramfs_link(struct inode *dir, const char *name, size_t len, struct inode *inode) {
    if (dir->nlink == 0) return -ENOENT;

    struct ramfs_dirent *d = kmalloc(sizeof(struct ramfs_dirent) + len);
    if (!d) return -ENOMEM;

    iget(inode); /* the name's */

    d->inode = inode;
    d->len   = len;
    memcpy(d->name, name, len);
    list_add_tail(&d->node, &node_of(dir)->entries);

    inode->nlink++;
    inode_touch(dir);
    inode->ctime = dir->mtime;

    /* a dir moved here: its .. is this one now. the old one's reference is let go with the caller's still on it */
    if (S_ISDIR(inode->mode)) {
        struct ramfs_node *node = node_of(inode);
        struct inode *old = node->parent;

        iget(dir);
        __atomic_store_n(&node->parent, dir, 3); /* __ATOMIC_RELEASE */
        dir->nlink++;

        iput(old);
    }

    return 0;
}

static int ramfs_unlink(struct inode *dir, const char *name, size_t len, struct inode *inode) {
    for (list_node *n = node_of(dir)->entries.next; n != &node_of(dir)->entries; n = n->next) {
        struct ramfs_dirent *d = container_of(n, struct ramfs_dirent, node);

        if (d->len != len || memcmp(d->name, name, len) != 0) continue;
        if (d->inode != inode) return -ENOENT; /* another file by now */

        list_del(&d->node);
        kfree(d);

        if (S_ISDIR(inode->mode)) dir->nlink--; /* its .. */
        inode_touch(dir);
        return 0;
    }

    return -ENOENT;
}

static int ramfs_dir_retire(struct inode *dir) {
    if (!list_empty(&node_of(dir)->entries)) return -ENOTEMPTY;

    dir->nlink = 0;
    return 0;
}

static long ramfs_read(struct inode *inode, void *buf, size_t n, uint64_t pos) {
    struct ramfs_node *node = node_of(inode);
    uint8_t *to = buf;

    if (pos >= inode->size) return 0;
    if (n > inode->size - pos) n = inode->size - pos;

    for (size_t done = 0; done < n;) {
        size_t idx = (pos + done) >> PAGE_SHIFT;
        size_t off = (pos + done) & (PAGE_SIZE - 1);
        size_t c   = PAGE_SIZE - off;
        if (c > n - done) c = n - done;

        if (idx < node->npages && node->pages[idx]) memcpy(to + done, (void *)P2V(node->pages[idx] + off), c);
        else                                        memset(to + done, 0, c);

        done += c;
    }

    return n;
}

/* the slot array comes from buddy: kmalloc stops at a page */
static void ramfs_free_slots(struct ramfs_node *node) {
    if (node->pages) free_pages((void *)V2P((uintptr_t)node->pages), SIZE_TO_ORDER(node->npages * sizeof(uintptr_t)));
}

/* slots for npages pages */
static int ramfs_grow(struct ramfs_node *node, size_t npages) {
    if (npages <= node->npages) return 0;

    int order = SIZE_TO_ORDER(npages * sizeof(uintptr_t));

    uintptr_t phys = (uintptr_t)alloc_pages(order);
    if (!phys) return -ENOMEM;

    uintptr_t *pages = (uintptr_t *)P2V(phys);
    memset(pages, 0, ORDER_TO_SIZE(order));

    if (node->pages) memcpy(pages, node->pages, node->npages * sizeof(uintptr_t));
    ramfs_free_slots(node);

    node->pages  = pages;
    node->npages = ORDER_TO_SIZE(order) / sizeof(uintptr_t);
    return 0;
}

static long ramfs_write(struct inode *inode, const void *buf, size_t n, uint64_t pos) {
    struct ramfs_node *node = node_of(inode);
    const uint8_t *from = buf;

    if (n == 0) return 0;
    if (ramfs_grow(node, ((pos + n - 1) >> PAGE_SHIFT) + 1) != 0) return -ENOMEM;

    size_t done = 0;

    while (done < n) {
        size_t idx = (pos + done) >> PAGE_SHIFT;
        size_t off = (pos + done) & (PAGE_SIZE - 1);
        size_t c   = PAGE_SIZE - off;
        if (c > n - done) c = n - done;

        if (!node->pages[idx]) {
            uintptr_t phys = (uintptr_t)alloc_pages(0);
            if (!phys) break;

            memset((void *)P2V(phys), 0, PAGE_SIZE);
            frame_init(phys);
            node->pages[idx] = phys;
        }

        memcpy((void *)P2V(node->pages[idx] + off), from + done, c);
        done += c;
    }

    if (pos + done > inode->size) inode->size = pos + done;

    return done ? (long)done : -ENOMEM;
}

/* the page of the file at index for a mapping: a hole is filled with zeros. the mapping's reference is taken */
static uintptr_t ramfs_getpage(struct inode *inode, uint64_t index) {
    struct ramfs_node *node = node_of(inode);

    if (index >= ALIGN_UP(inode->size, PAGE_SIZE) >> PAGE_SHIFT) return 0;
    if (ramfs_grow(node, index + 1) != 0) return 0;

    if (!node->pages[index]) {
        uintptr_t phys = (uintptr_t)alloc_pages(0);
        if (!phys) return 0;

        memset((void *)P2V(phys), 0, PAGE_SIZE);
        frame_init(phys);
        node->pages[index] = phys;
    }

    frame_get(node->pages[index]);
    return node->pages[index];
}

static int ramfs_truncate(struct inode *inode, uint64_t size) {
    struct ramfs_node *node = node_of(inode);

    /* pages past the end go, the tail of the last one is zeroed for a later grow */
    for (size_t i = ALIGN_UP(size, PAGE_SIZE) >> PAGE_SHIFT; i < node->npages; i++) {
        if (node->pages[i]) frame_put(node->pages[i]); /* a mapping of it keeps it until it goes */
        node->pages[i] = 0;
    }

    size_t idx = size >> PAGE_SHIFT;
    size_t off = size & (PAGE_SIZE - 1);

    if (off && idx < node->npages && node->pages[idx])
        memset((void *)P2V(node->pages[idx] + off), 0, PAGE_SIZE - off);

    inode->size = size;
    return 0;
}

/* the pages inside the range go, and the ends of the ones it only cuts are zeroed. a page that is mapped stays in the
 * mappings it is in: they keep what they saw until they are unmapped */
static int ramfs_punch_hole(struct inode *inode, uint64_t off, uint64_t len) {
    struct ramfs_node *node = node_of(inode);
    uint64_t end = off + len;

    for (uint64_t pos = off; pos < end;) {
        size_t idx = pos >> PAGE_SHIFT;
        size_t in_page = pos & (PAGE_SIZE - 1);
        size_t c = PAGE_SIZE - in_page;

        if (c > end - pos) c = end - pos;

        if (idx < node->npages && node->pages[idx]) {
            if (c == PAGE_SIZE) {
                frame_put(node->pages[idx]);
                node->pages[idx] = 0;
            } else {
                memset((void *)P2V(node->pages[idx] + in_page), 0, c);
            }
        }

        pos += c;
    }

    return 0;
}

/* entries 0 and 1 are "." and "..", then the list in order */
static long ramfs_getdents(struct inode *dir, void *buf, size_t n, uint64_t *pos) {
    struct ramfs_node *node = node_of(dir);
    size_t done = 0;
    uint64_t idx = 0;

    for (int i = 0; i < 2; i++, idx++) {
        if (idx < *pos) continue;

        struct inode *at = i ? node->parent : dir; /* the name is ".." cut to len */
        size_t r = vfs_dirent_put((char *)buf + done, n - done, at->ino, idx + 1, at->mode, "..", 1 + i);

        if (!r) return done ? (long)done : -EINVAL;

        done += r;
        *pos = idx + 1;
    }

    for (list_node *it = node->entries.next; it != &node->entries; it = it->next, idx++) {
        if (idx < *pos) continue;

        struct ramfs_dirent *d = container_of(it, struct ramfs_dirent, node);
        size_t r = vfs_dirent_put((char *)buf + done, n - done, d->inode->ino, idx + 1, d->inode->mode, d->name, d->len);

        if (!r) return done ? (long)done : -EINVAL;

        done += r;
        *pos = idx + 1;
    }

    return done;
}

static void ramfs_evict(struct inode *inode) {
    struct ramfs_node *node = node_of(inode);

    for (size_t i = 0; i < node->npages; i++)
        if (node->pages[i]) frame_put(node->pages[i]);
    ramfs_free_slots(node);
    kfree(node->target);

    while (!list_empty(&node->entries)) {
        struct ramfs_dirent *d = list_first(&node->entries, struct ramfs_dirent, node);

        list_del(&d->node);
        iput(d->inode);
        kfree(d);
    }

    if (S_ISDIR(inode->mode) && node->parent != inode) iput(node->parent);

    kfree(node);
}

static const struct inode_ops ramfs_ops = {
    .lookup     = ramfs_lookup,
    .create     = ramfs_create,
    .symlink    = ramfs_symlink,
    .readlink   = ramfs_readlink,
    .link       = ramfs_link,
    .unlink     = ramfs_unlink,
    .dir_retire = ramfs_dir_retire,
    .read       = ramfs_read,
    .write      = ramfs_write,
    .truncate   = ramfs_truncate,
    .punch_hole = ramfs_punch_hole,
    .getpage    = ramfs_getpage,
    .getdents   = ramfs_getdents,
    .evict      = ramfs_evict,
};
