#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/list.h>
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

    /* dir */
    list_node      entries;
    struct inode  *parent;  /* no reference: dirs are never removed yet */
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
    node->parent = parent ? parent : inode; /* the root is its own */

    inode->priv  = node;
    inode->nlink = S_ISDIR(mode) ? 2 : 1;

    return inode;
}

struct inode *ramfs_root(void) {
    return ramfs_new(S_IFDIR | 0755, NULL);
}

static int ramfs_lookup(struct inode *dir, const char *name, size_t len, struct inode **out) {
    struct ramfs_node *node = node_of(dir);

    if (len == 2 && name[0] == '.' && name[1] == '.') {
        iget(node->parent);
        *out = node->parent;
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

    iget(inode);
    *out = inode;
    return 0;
}

static int ramfs_link(struct inode *dir, const char *name, size_t len, struct inode *inode) {
    struct ramfs_dirent *d = kmalloc(sizeof(struct ramfs_dirent) + len);
    if (!d) return -ENOMEM;

    iget(inode); /* the name's */

    d->inode = inode;
    d->len   = len;
    memcpy(d->name, name, len);
    list_add_tail(&d->node, &node_of(dir)->entries);

    inode->nlink++;
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
            node->pages[idx] = phys;
        }

        memcpy((void *)P2V(node->pages[idx] + off), from + done, c);
        done += c;
    }

    if (pos + done > inode->size) inode->size = pos + done;

    return done ? (long)done : -ENOMEM;
}

static int ramfs_truncate(struct inode *inode, uint64_t size) {
    struct ramfs_node *node = node_of(inode);

    /* pages past the end go, the tail of the last one is zeroed for a later grow */
    for (size_t i = ALIGN_UP(size, PAGE_SIZE) >> PAGE_SHIFT; i < node->npages; i++) {
        free_pages((void *)node->pages[i], 0);
        node->pages[i] = 0;
    }

    size_t idx = size >> PAGE_SHIFT;
    size_t off = size & (PAGE_SIZE - 1);

    if (off && idx < node->npages && node->pages[idx])
        memset((void *)P2V(node->pages[idx] + off), 0, PAGE_SIZE - off);

    inode->size = size;
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

    for (size_t i = 0; i < node->npages; i++) free_pages((void *)node->pages[i], 0);
    ramfs_free_slots(node);

    while (!list_empty(&node->entries)) {
        struct ramfs_dirent *d = list_first(&node->entries, struct ramfs_dirent, node);

        list_del(&d->node);
        iput(d->inode);
        kfree(d);
    }

    kfree(node);
}

static const struct inode_ops ramfs_ops = {
    .lookup   = ramfs_lookup,
    .create   = ramfs_create,
    .link     = ramfs_link,
    .read     = ramfs_read,
    .write    = ramfs_write,
    .truncate = ramfs_truncate,
    .getdents = ramfs_getdents,
    .evict    = ramfs_evict,
};
