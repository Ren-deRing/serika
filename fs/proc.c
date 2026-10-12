#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/vfs.h>

#include <asm/arch.h>

/* /proc, what programs look for: the way to a process's open files (fd/N, which opens the file again, as systemd
 * does to make a path-only fd readable), its cwd, the mounts, and a few files of the system. nothing is kept: the
 * inodes are made at each lookup and the files are made when they are read */

enum proc_kind {
    K_ROOT, K_SELF, K_PID, K_FDDIR, K_FDINFODIR, K_FDLINK, K_FDINFO, K_CWD, K_EXE, K_GEN,
};

struct proc_node {
    enum proc_kind kind;
    uint64_t       pid;
    int            fd;  /* K_FDLINK, K_FDINFO */
    int            gen; /* K_GEN: gens[] */
};

static const struct inode_ops proc_ops;

/* ---- the files made of text ---- */

typedef size_t (*gen_fn)(const struct proc_node *n, char *buf, size_t cap);

static size_t gen_uptime(const struct proc_node *n, char *buf, size_t cap) {
    (void)n;

    uint64_t tsc = rdtsc(), freq = this_cpu_read(tsc_freq_hz);

    return ksnprintf(buf, cap, "%lu.%02lu %lu.00\n", tsc / freq, tsc % freq * 100 / freq, tsc / freq);
}

static size_t gen_meminfo(const struct proc_node *n, char *buf, size_t cap) {
    (void)n;

    size_t total = buddy_managed_pages() * 4, free = buddy_free_pages() * 4;

    return ksnprintf(buf, cap,
                     "MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\nBuffers:               0 kB\n"
                     "Cached:                0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n",
                     total, free, free);
}

static size_t gen_cpuinfo(const struct proc_node *n, char *buf, size_t cap) {
    (void)n;

    size_t at = 0;
    uint64_t mhz = this_cpu_read(tsc_freq_hz) / 1000000;

    for (uint32_t i = 0, shown = 0; i < MAX_CPUS && shown < sched_nr_online(); i++) {
        if (!sched_cpu_online(i)) continue;

        at += ksnprintf(buf + at, cap - at, "processor\t: %u\nvendor_id\t: serika\nmodel name\t: serika virtual cpu\n"
                        "cpu MHz\t\t: %lu.000\n\n", shown++, mhz);
    }

    return at;
}

static size_t gen_version(const struct proc_node *n, char *buf, size_t cap) {
    (void)n;

    return ksnprintf(buf, cap, "Linux version 6.8.0-serika (serika@serika) (clang) #1 SMP\n");
}

static size_t gen_cmdline(const struct proc_node *n, char *buf, size_t cap) {
    (void)n;

    return ksnprintf(buf, cap, "serika\n");
}

static size_t gen_loadavg(const struct proc_node *n, char *buf, size_t cap) {
    (void)n;

    return ksnprintf(buf, cap, "0.00 0.00 0.00 1/1 1\n");
}

static size_t gen_mounts(const struct proc_node *n, char *buf, size_t cap) {
    (void)n;

    return vfs_mounts_text(buf, cap, false);
}

static size_t gen_mountinfo(const struct proc_node *n, char *buf, size_t cap) {
    (void)n;

    return vfs_mounts_text(buf, cap, true);
}

static const struct { const char *name; gen_fn fn; bool per_pid; } gens[] = {
    { "uptime",    gen_uptime,    false },
    { "meminfo",   gen_meminfo,   false },
    { "cpuinfo",   gen_cpuinfo,   false },
    { "version",   gen_version,   false },
    { "cmdline",   gen_cmdline,   false },
    { "loadavg",   gen_loadavg,   false },
    { "mounts",    gen_mounts,    true  },
    { "mountinfo", gen_mountinfo, true  },
};

#define NGENS (sizeof(gens) / sizeof(gens[0]))

/* ---- processes and their files ---- */

static struct file *file_of(struct files *files, int fd) {
    struct file *f = NULL;

    if (fd < 0 || fd >= FILES_MAX) return NULL;

    uint64_t flags = spin_lock_irqsave(&files->lock);

    f = files->fd[fd];
    if (f) fget(f);

    spin_unlock_irqrestore(&files->lock, flags);
    return f;
}

static bool pid_exists(uint64_t pid) {
    struct files *files = task_files_get(pid);

    if (!files) return false;

    files_put(files);
    return true;
}

static size_t gen_fdinfo(const struct proc_node *n, char *buf, size_t cap) {
    struct files *files = task_files_get(n->pid);
    if (!files) return 0;

    struct file *f = file_of(files, n->fd);
    size_t len = 0;

    if (f) {
        len = ksnprintf(buf, cap, "pos:\t%lu\nflags:\t0%o\nmnt_id:\t%u\nino:\t%lu\n", f->pos, f->flags, 1 + f->inode->dev,
                        f->inode->ino);
        vfs_close(f);
    }

    files_put(files);
    return len;
}

/* ---- inodes ---- */

static struct inode *proc_inode(enum proc_kind kind, uint64_t pid, int fd, int gen) {
    uint32_t mode = kind == K_ROOT || kind == K_PID || kind == K_FDDIR || kind == K_FDINFODIR ? S_IFDIR | 0555 :
                    kind == K_GEN || kind == K_FDINFO ? S_IFREG | 0444 : S_IFLNK | 0777;

    struct proc_node *node = kmalloc(sizeof(*node));
    if (!node) return NULL;

    struct inode *inode = inode_alloc(&proc_ops, mode);
    if (!inode) {
        kfree(node);
        return NULL;
    }

    node->kind = kind;
    node->pid  = pid;
    node->fd   = fd;
    node->gen  = gen;

    inode->priv = node;
    inode->dev  = VFS_DEV_PROC;
    inode->ino  = ((uint64_t)kind << 56) ^ (pid << 20) ^ ((uint64_t)(fd + 1) << 4) ^ (uint64_t)gen ^ 0x100;
    inode->nlink = S_ISDIR(mode) ? 2 : 1;
    return inode;
}

struct inode *proc_root(void) {
    return proc_inode(K_ROOT, 0, 0, 0);
}

static struct proc_node *node_of(struct inode *inode) {
    return inode->priv;
}

static bool is_number(const char *s, size_t len, uint64_t *out) {
    uint64_t v = 0;

    if (!len || len > 10) return false;

    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }

    *out = v;
    return true;
}

static int proc_lookup(struct inode *dir, const char *name, size_t len, struct inode **out) {
    struct proc_node *n = node_of(dir);
    struct inode *inode = NULL;
    uint64_t num;

    if (len == 2 && name[0] == '.' && name[1] == '.') {
        switch (n->kind) {
            case K_PID:        inode = proc_inode(K_ROOT, 0, 0, 0); break;
            case K_FDDIR:
            case K_FDINFODIR:  inode = proc_inode(K_PID, n->pid, 0, 0); break;
            default:           inode = proc_inode(K_ROOT, 0, 0, 0); break;
        }
    } else if (n->kind == K_ROOT) {
        if (len == 4 && memcmp(name, "self", 4) == 0) {
            inode = proc_inode(K_SELF, 0, 0, 0);
        } else if (is_number(name, len, &num)) {
            if (pid_exists(num)) inode = proc_inode(K_PID, num, 0, 0);
        } else {
            for (size_t i = 0; i < NGENS && !inode; i++)
                if (strlen(gens[i].name) == len && memcmp(gens[i].name, name, len) == 0)
                    inode = proc_inode(K_GEN, 0, 0, i);
        }
    } else if (n->kind == K_PID) {
        if (len == 2 && memcmp(name, "fd", 2) == 0)                  inode = proc_inode(K_FDDIR, n->pid, 0, 0);
        else if (len == 6 && memcmp(name, "fdinfo", 6) == 0)         inode = proc_inode(K_FDINFODIR, n->pid, 0, 0);
        else if (len == 3 && memcmp(name, "cwd", 3) == 0)            inode = proc_inode(K_CWD, n->pid, 0, 0);
        else if (len == 3 && memcmp(name, "exe", 3) == 0)            inode = proc_inode(K_EXE, n->pid, 0, 0);
        else {
            for (size_t i = 0; i < NGENS && !inode; i++)
                if (gens[i].per_pid && strlen(gens[i].name) == len && memcmp(gens[i].name, name, len) == 0)
                    inode = proc_inode(K_GEN, n->pid, 0, i);
        }
    } else if ((n->kind == K_FDDIR || n->kind == K_FDINFODIR) && is_number(name, len, &num)) {
        struct files *files = task_files_get(n->pid);

        if (files) {
            struct file *f = file_of(files, num);

            if (f) {
                inode = proc_inode(n->kind == K_FDDIR ? K_FDLINK : K_FDINFO, n->pid, num, 0);
                vfs_close(f);
            }

            files_put(files);
        }
    }

    if (!inode) return -ENOENT;

    *out = inode;
    return 0;
}

/* where a link goes. /proc/self is the caller's process; an fd is the path it was opened by, or what it is */
static long proc_readlink(struct inode *inode, char *buf, size_t n) {
    struct proc_node *node = node_of(inode);
    char tmp[PATH_MAX];
    size_t len = 0;

    if (node->kind == K_SELF) {
        len = ksnprintf(tmp, sizeof(tmp), "%lu", (unsigned long)sys_getpid());
    } else if (node->kind == K_CWD) {
        struct files *files = task_files_get(node->pid);
        if (!files) return -ENOENT;

        len = files_cwd(files, tmp, sizeof(tmp) - 1);

        files_put(files);
    } else if (node->kind == K_FDLINK) {
        struct files *files = task_files_get(node->pid);
        if (!files) return -ENOENT;

        struct file *f = file_of(files, node->fd);

        files_put(files);
        if (!f) return -ENOENT;

        if (f->path) {
            len = strlen(f->path) < sizeof(tmp) ? strlen(f->path) : sizeof(tmp) - 1;
            memcpy(tmp, f->path, len);
        } else if (S_ISSOCK(f->inode->mode)) {
            len = ksnprintf(tmp, sizeof(tmp), "socket:[%lu]", f->inode->ino);
        } else if (S_ISFIFO(f->inode->mode) || f->inode->ops->stream) {
            len = ksnprintf(tmp, sizeof(tmp), "pipe:[%lu]", f->inode->ino);
        } else {
            len = ksnprintf(tmp, sizeof(tmp), "anon_inode:[%lu]", f->inode->ino);
        }

        vfs_close(f);
    } else {
        return -ENOENT; /* exe: the program's path is not kept */
    }

    if (len > n) len = n;

    memcpy(buf, tmp, len);
    return len;
}

/* opening /proc/self/fd/N is opening the file that fd is: another file on the same inode, with the flags asked for */
static int proc_follow_link(struct inode *link, struct inode **target) {
    struct proc_node *node = node_of(link);

    if (node->kind != K_FDLINK) return -ENOENT;

    struct files *files = task_files_get(node->pid);
    if (!files) return -ENOENT;

    struct file *f = file_of(files, node->fd);

    files_put(files);
    if (!f) return -ENOENT;

    iget(f->inode);
    *target = f->inode;

    vfs_close(f);
    return 0;
}

static long proc_read(struct inode *inode, void *buf, size_t n, uint64_t pos) {
    struct proc_node *node = node_of(inode);
    int order = 1; /* 8 KiB is more than any of these is */
    char *text = alloc_pages(order);

    if (!text) return -ENOMEM;

    text = (char *)P2V((uintptr_t)text);

    size_t len = node->kind == K_FDINFO ? gen_fdinfo(node, text, ORDER_TO_SIZE(order)) :
                                          gens[node->gen].fn(node, text, ORDER_TO_SIZE(order));
    if (len >= ORDER_TO_SIZE(order)) len = ORDER_TO_SIZE(order) - 1; /* snprintf's length is what it wanted */

    long done = 0;

    if (pos < len) {
        done = len - pos < n ? len - pos : n;
        memcpy(buf, text + pos, done);
    }

    free_pages((void *)V2P((uintptr_t)text), order);
    return done;
}

/* "." and "..", then the names; or for the fd dirs, the fds that are open */
static long proc_getdents(struct inode *dir, void *buf, size_t n, uint64_t *pos) {
    struct proc_node *node = node_of(dir);
    size_t done = 0;
    uint64_t idx = 0;

    for (int i = 0; i < 2; i++, idx++) {
        if (idx < *pos) continue;

        size_t r = vfs_dirent_put((char *)buf + done, n - done, dir->ino, idx + 1, S_IFDIR, "..", 1 + i);

        if (!r) return done ? (long)done : -EINVAL;

        done += r;
        *pos = idx + 1;
    }

    if (node->kind == K_FDDIR || node->kind == K_FDINFODIR) {
        struct files *files = task_files_get(node->pid);
        if (!files) return done;

        for (int fd = 0; fd < FILES_MAX; fd++) {
            struct file *f = file_of(files, fd);

            if (!f) continue;
            vfs_close(f);

            if ((uint64_t)(2 + fd) < *pos) continue;

            char name[12];
            size_t len = ksnprintf(name, sizeof(name), "%d", fd);
            size_t r = vfs_dirent_put((char *)buf + done, n - done, 1000 + fd, 2 + fd + 1,
                                      node->kind == K_FDDIR ? S_IFLNK : S_IFREG, name, len);

            if (!r) {
                files_put(files);
                return done ? (long)done : -EINVAL;
            }

            done += r;
            *pos = 2 + fd + 1;
        }

        files_put(files);
        return done;
    }

    /* the names: the root's own, a process's */
    struct { const char *name; uint32_t mode; } names[16];
    size_t count = 0;

    if (node->kind == K_ROOT) {
        names[count++] = (typeof(names[0])){ "self", S_IFLNK };

        for (size_t i = 0; i < NGENS; i++) names[count++] = (typeof(names[0])){ gens[i].name, S_IFREG };
    } else if (node->kind == K_PID) {
        names[count++] = (typeof(names[0])){ "fd", S_IFDIR };
        names[count++] = (typeof(names[0])){ "fdinfo", S_IFDIR };
        names[count++] = (typeof(names[0])){ "cwd", S_IFLNK };
        names[count++] = (typeof(names[0])){ "exe", S_IFLNK };

        for (size_t i = 0; i < NGENS; i++)
            if (gens[i].per_pid) names[count++] = (typeof(names[0])){ gens[i].name, S_IFREG };
    }

    for (size_t i = 0; i < count; i++, idx++) {
        if (idx < *pos) continue;

        size_t r = vfs_dirent_put((char *)buf + done, n - done, 2000 + idx, idx + 1, names[i].mode, names[i].name,
                                  strlen(names[i].name));

        if (!r) return done ? (long)done : -EINVAL;

        done += r;
        *pos = idx + 1;
    }

    return done;
}

static void proc_evict(struct inode *inode) {
    kfree(inode->priv);
}

static const struct inode_ops proc_ops = {
    .lookup      = proc_lookup,
    .readlink    = proc_readlink,
    .follow_link = proc_follow_link,
    .read        = proc_read,
    .getdents    = proc_getdents,
    .evict       = proc_evict,
};
