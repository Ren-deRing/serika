#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/elf.h>
#include <serika/errno.h>
#include <serika/exec.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/random.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/vfs.h>

#include <asm/arch.h>
#include <asm/task.h>

#define ELF_ET_DYN_BASE 0x555555554000ULL      /* linux's, without randomization */
#define EXEC_MAX_PHNUM  64
#define EXEC_ARGS_MAX   (USER_STACK_SIZE / 4)  /* strings, vectors and auxv */
#define EXEC_NR_AUXV    19                     /* AT_NULL included */
#define EXEC_LINE_MAX   256                    /* the "#!" line, linux's too */
#define EXEC_SCRIPT_DEPTH 4                    /* a script whose interpreter is a script */

static int read_exact(struct file *f, void *buf, size_t n, uint64_t pos) {
    uint8_t *to = buf;

    while (n) {
        long r = kernel_read(f, to, n, &pos);
        if (r < 0) return r;
        if (r == 0) return -ENOEXEC; /* the file is shorter than its headers say */

        to += r;
        n  -= r;
    }

    return 0;
}

static int check_ehdr(const Elf64_Ehdr *eh) {
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return -ENOEXEC;

    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_ident[EI_VERSION] != EV_CURRENT)
        return -ENOEXEC;

    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return -ENOEXEC;
    if (eh->e_machine != EM_X86_64) return -ENOEXEC;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return -ENOEXEC;
    if (eh->e_phnum == 0 || eh->e_phnum > EXEC_MAX_PHNUM) return -ENOEXEC;

    return 0;
}

/* no vfs lock is held while mm_copy_to() holds the mm's: reads go through bounce */
static int load_segment(struct file *f, struct mm *mm, const Elf64_Phdr *ph, uintptr_t bias, void *bounce) {
    if (ph->p_filesz > ph->p_memsz) return -ENOEXEC;

    uintptr_t va = bias + ph->p_vaddr;
    if (va < bias || va + ph->p_memsz < va) return -ENOEXEC;

    uintptr_t start = ALIGN_DOWN(va, PAGE_SIZE);
    uintptr_t end   = ALIGN_UP(va + ph->p_memsz, PAGE_SIZE);

    uint32_t prot = 0;
    if (ph->p_flags & PF_R) prot |= PROT_READ;
    if (ph->p_flags & PF_W) prot |= PROT_WRITE;
    if (ph->p_flags & PF_X) prot |= PROT_EXEC;

    /* a bad address, or two segments on one page */
    int err = mm_map(mm, start, end - start, prot);
    if (err) return err == -ENOMEM ? err : -ENOEXEC;

    /* the rest up to p_memsz is bss: pages come zeroed */
    for (uint64_t done = 0; done < ph->p_filesz;) {
        size_t n = ph->p_filesz - done;
        if (n > PAGE_SIZE) n = PAGE_SIZE;

        if ((err = read_exact(f, bounce, n, ph->p_offset + done)) != 0) return err;
        if ((err = mm_copy_to(mm, va + done, bounce, n)) != 0) return err;

        done += n;
    }

    return 0;
}

/* PT_PHDR, or the PT_LOAD whose file range holds them. glibc finds its TLS through this */
static uintptr_t phdr_addr(const Elf64_Ehdr *eh, const Elf64_Phdr *ph, uintptr_t bias) {
    uint64_t size = (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr);

    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_PHDR) return bias + ph[i].p_vaddr;

    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_offset <= eh->e_phoff &&
            eh->e_phoff + size <= ph[i].p_offset + ph[i].p_filesz)
            return bias + ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);

    return 0;
}

/* count and string bytes, -E2BIG past the limit */
static int count_strings(const char *const v[], size_t *count, size_t *bytes) {
    *count = 0;

    for (; v && v[*count]; (*count)++) {
        *bytes += strnlen(v[*count], EXEC_ARGS_MAX) + 1;
        if (*bytes + *count * sizeof(uintptr_t) > EXEC_ARGS_MAX) return -E2BIG;
    }

    return 0;
}

/* buf stands for [sp, ...). copies s at *at, returns where it is for the task */
static uintptr_t put_string(uint8_t *buf, uintptr_t sp, uintptr_t *at, const char *s) {
    size_t n = strlen(s) + 1;
    uintptr_t addr = *at;

    memcpy(buf + (addr - sp), s, n);
    *at += n;

    return addr;
}

struct exec_image {
    uintptr_t entry; /* the program's, for AT_ENTRY. with ld.so it is not where we start */
    uintptr_t phdr;
    uint64_t  phnum;
    uintptr_t base;  /* ld.so's, 0 without one */
};

/* sysv: argc, argv, NULL, envp, NULL, auxv at sp (16 aligned), strings above */
static int setup_stack(struct mm *mm, const char *name, const char *const argv[], const char *const envp[],
                       const struct exec_image *img, uintptr_t *sp_out) {
    static const char platform[] = "x86_64";

    size_t argc, envc;
    size_t strings = sizeof(platform) + 16 + strnlen(name, PATH_MAX) + 1;

    int err;
    if ((err = count_strings(argv, &argc, &strings)) != 0) return err;
    if ((err = count_strings(envp, &envc, &strings)) != 0) return err;

    size_t vecs = 1 + (argc + 1) + (envc + 1) + 2 * EXEC_NR_AUXV;

    uintptr_t top = USER_STACK_TOP;
    uintptr_t sp  = ALIGN_DOWN(top - strings - vecs * sizeof(uintptr_t), 16);
    size_t size   = top - sp;

    if (size > EXEC_ARGS_MAX) return -E2BIG;

    /* built in a kernel buffer that stands for [sp, top) */
    int order = SIZE_TO_ORDER(size);
    uintptr_t phys = (uintptr_t)alloc_pages(order);
    if (!phys) return -ENOMEM;

    uint8_t *buf = (uint8_t *)P2V(phys);
    memset(buf, 0, size);

    uintptr_t at = top - strings;

    uintptr_t random_at = at;
    get_random_bytes(buf + (at - sp), 16);
    at += 16;

    uintptr_t platform_at = put_string(buf, sp, &at, platform);
    uintptr_t execfn_at   = put_string(buf, sp, &at, name);

    uintptr_t *v = (uintptr_t *)buf;

    *v++ = argc;
    for (size_t i = 0; i < argc; i++) *v++ = put_string(buf, sp, &at, argv[i]);
    *v++ = 0;
    for (size_t i = 0; i < envc; i++) *v++ = put_string(buf, sp, &at, envp[i]);
    *v++ = 0;

    uint32_t eax, ebx, ecx, edx;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);

    const uintptr_t auxv[EXEC_NR_AUXV * 2] = {
        AT_PHDR,     img->phdr,
        AT_PHENT,    sizeof(Elf64_Phdr),
        AT_PHNUM,    img->phnum,
        AT_PAGESZ,   PAGE_SIZE,
        AT_BASE,     img->base,
        AT_FLAGS,    0,
        AT_ENTRY,    img->entry,
        AT_UID,      0,
        AT_EUID,     0,
        AT_GID,      0,
        AT_EGID,     0,
        AT_SECURE,   0,
        AT_RANDOM,   random_at,
        AT_HWCAP,    edx,
        AT_HWCAP2,   0,
        AT_CLKTCK,   100,
        AT_PLATFORM, platform_at,
        AT_EXECFN,   execfn_at,
        AT_NULL,     0,
    };

    memcpy(v, auxv, sizeof(auxv));

    err = mm_copy_to(mm, sp, buf, size);

    free_pages((void *)phys, order);

    if (!err) *sp_out = sp;
    return err;
}

/* PT_INTERP: the path of the program that loads this one */
static int read_interp(struct file *f, const Elf64_Phdr *ph, char **out) {
    if (ph->p_filesz < 2 || ph->p_filesz > PATH_MAX) return -ENOEXEC;

    char *path = kmalloc(PATH_MAX);
    if (!path) return -ENOMEM;

    int err = read_exact(f, path, ph->p_filesz, ph->p_offset);
    if (!err && path[ph->p_filesz - 1] != '\0') err = -ENOEXEC;

    if (err) {
        kfree(path);
        return err;
    }

    *out = path;
    return 0;
}

/* ld.so: ET_DYN with no interpreter of its own, in an mmap area of its size */
static int load_interp(struct mm *mm, const char *path, void *bounce, uintptr_t *base, uintptr_t *entry) {
    struct file *f;

    int err = vfs_open(path, O_RDONLY, 0, &f);
    if (err) return err;

    Elf64_Ehdr eh;
    Elf64_Phdr *ph = NULL;

    if (!S_ISREG(f->inode->mode)) {
        err = -EACCES;
        goto out;
    }

    if ((err = read_exact(f, &eh, sizeof(eh), 0)) != 0) goto out;
    if ((err = check_ehdr(&eh)) != 0) goto out;

    if (eh.e_type != ET_DYN) {
        err = -ENOEXEC;
        goto out;
    }

    if (!(ph = kmalloc(eh.e_phnum * sizeof(Elf64_Phdr)))) {
        err = -ENOMEM;
        goto out;
    }

    if ((err = read_exact(f, ph, eh.e_phnum * sizeof(Elf64_Phdr), eh.e_phoff)) != 0) goto out;

    uintptr_t lo = UINTPTR_MAX, hi = 0;

    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            err = -ENOEXEC;
            goto out;
        }

        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;

        if (ALIGN_DOWN(ph[i].p_vaddr, PAGE_SIZE) < lo) lo = ALIGN_DOWN(ph[i].p_vaddr, PAGE_SIZE);
        if (ph[i].p_vaddr + ph[i].p_memsz > hi)      hi = ph[i].p_vaddr + ph[i].p_memsz;
    }

    if (hi <= lo) {
        err = -ENOEXEC;
        goto out;
    }

    uintptr_t area = mm_take_area(mm, ALIGN_UP(hi, PAGE_SIZE) - lo);
    if (!area) {
        err = -ENOMEM;
        goto out;
    }

    uintptr_t bias = area - lo;

    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;

        if ((err = load_segment(f, mm, &ph[i], bias, bounce)) != 0) goto out;
    }

    *base = bias;
    *entry = bias + eh.e_entry;

out:
    kfree(ph);
    vfs_close(f);
    return err;
}

static int load(const char *path, const char *name, const char *const argv[], const char *const envp[],
                struct mm **out, uintptr_t *entry, uintptr_t *sp_out, int depth);

/* "#! interpreter [arg]" in line, n bytes: the interpreter is run with its arg, the script's name and the script's
 * own arguments after that. only one arg, the whole rest of the line, as linux does */
static int exec_script(char *line, size_t n, const char *name, const char *const argv[], const char *const envp[],
                       struct mm **out, uintptr_t *entry, uintptr_t *sp_out, int depth) {
    if (depth >= EXEC_SCRIPT_DEPTH) return -ELOOP;

    char *end = line;
    while (end < line + n && *end != '\n') end++;

    if (end == line + n && n == EXEC_LINE_MAX) return -ENOEXEC; /* a line that does not end */

    *end = '\0';

    char *interp = line + 2;
    while (*interp == ' ' || *interp == '\t') interp++;
    if (!*interp) return -ENOEXEC;

    char *arg = interp;
    while (*arg && *arg != ' ' && *arg != '\t') arg++;

    if (*arg) {
        *arg++ = '\0';
        while (*arg == ' ' || *arg == '\t') arg++;
    }

    char *tail = arg + strlen(arg);
    while (tail > arg && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = '\0';

    size_t argc = 0;
    while (argv && argv[argc]) argc++;

    size_t bytes = (argc + 4) * sizeof(char *);
    int order = SIZE_TO_ORDER(bytes);

    uintptr_t phys = (uintptr_t)alloc_pages(order);
    if (!phys) return -ENOMEM;

    const char **vec = (const char **)P2V(phys);
    size_t k = 0;

    vec[k++] = interp;
    if (*arg) vec[k++] = arg;
    vec[k++] = name;
    for (size_t i = 1; i < argc; i++) vec[k++] = argv[i];
    vec[k] = NULL;

    int err = load(interp, interp, vec, envp, out, entry, sp_out, depth + 1);

    free_pages((void *)phys, order);
    return err;
}

/* a new address space with path loaded and its stack built: where to start, and with what rsp */
int exec_load(const char *path, const char *name, const char *const argv[], const char *const envp[],
              struct mm **out, uintptr_t *entry, uintptr_t *sp_out) {
    return load(path, name, argv, envp, out, entry, sp_out, 0);
}

static int load(const char *path, const char *name, const char *const argv[], const char *const envp[],
                struct mm **out, uintptr_t *entry, uintptr_t *sp_out, int depth) {
    struct file *f;

    int err = vfs_open(path, O_RDONLY, 0, &f);
    if (err) return err;

    Elf64_Ehdr eh;
    Elf64_Phdr *ph = NULL;
    struct mm *mm = NULL;
    uintptr_t bounce = 0;
    char *interp = NULL;

    if (!S_ISREG(f->inode->mode)) {
        err = -EACCES;
        goto out;
    }

    /* a script names its interpreter on the first line */
    char line[EXEC_LINE_MAX];
    uint64_t pos = 0;
    long got = kernel_read(f, line, sizeof(line), &pos);

    if (got >= 2 && line[0] == '#' && line[1] == '!') {
        err = exec_script(line, got, name, argv, envp, out, entry, sp_out, depth);
        goto out;
    }

    if ((err = read_exact(f, &eh, sizeof(eh), 0)) != 0) goto out;
    if ((err = check_ehdr(&eh)) != 0) goto out;

    ph = kmalloc(eh.e_phnum * sizeof(Elf64_Phdr));
    bounce = (uintptr_t)alloc_pages(0);
    mm = mm_create();

    if (!ph || !bounce || !mm) {
        err = -ENOMEM;
        goto out;
    }

    if ((err = read_exact(f, ph, eh.e_phnum * sizeof(Elf64_Phdr), eh.e_phoff)) != 0) goto out;

    /* dynamic: ld.so goes in next to it and starts first */
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP && !interp && (err = read_interp(f, &ph[i], &interp)) != 0) goto out;
    }

    /* a pie, static or not, is placed: at linux's spot */
    uintptr_t bias = eh.e_type == ET_DYN ? ELF_ET_DYN_BASE : 0;

    uintptr_t image_end = 0;

    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;

        if ((err = load_segment(f, mm, &ph[i], bias, (void *)P2V(bounce))) != 0) goto out;

        if (bias + ph[i].p_vaddr + ph[i].p_memsz > image_end) image_end = bias + ph[i].p_vaddr + ph[i].p_memsz;
    }

    mm_set_brk(mm, image_end);

    /* a segment in the stack's range fails here */
    if ((err = mm_map(mm, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, PROT_READ | PROT_WRITE)) != 0) {
        if (err != -ENOMEM) err = -ENOEXEC;
        goto out;
    }

    struct exec_image img = {
        .entry = bias + eh.e_entry,
        .phdr  = phdr_addr(&eh, ph, bias),
        .phnum = eh.e_phnum,
    };

    uintptr_t start = img.entry;

    if (interp && (err = load_interp(mm, interp, (void *)P2V(bounce), &img.base, &start)) != 0) goto out;

    if ((err = setup_stack(mm, name, argv, envp, &img, sp_out)) != 0) goto out;

    *out = mm;
    *entry = start;
    mm = NULL; /* the caller's now */

out:
    if (mm) mm_put(mm);
    free_pages((void *)bounce, 0);
    kfree(interp);
    kfree(ph);
    vfs_close(f);
    return err;
}

/* a new process running path, a child of the caller. not runnable until sched_add_task() */
int task_create_exec(const char *path, const char *const argv[], const char *const envp[], int nice,
                     struct task **out) {
    struct mm *mm;
    uintptr_t entry, sp;

    int err = exec_load(path, path, argv, envp, &mm, &entry, &sp);
    if (err) return err;

    struct files *files = files_create();
    struct sighand *sighand = sighand_create();
    struct task *task = files && sighand ? task_create_mm(mm, files, sighand, entry, sp, nice) : NULL;

    if (!task) {
        mm_put(mm);
        if (files) files_put(files);
        if (sighand) sighand_put(sighand);
        return -ENOMEM;
    }

    *out = task;
    return 0;
}

/* a NULL-ended user array of strings into area: the array first, then the strings. NULL is empty */
static int copy_strings(uintptr_t uvec, char *area, size_t *used, const char ***out) {
    size_t n = 0;
    uintptr_t p;

    *used = ALIGN_UP(*used, sizeof(char *));

    for (;; n++) {
        if ((n + 1) * sizeof(char *) > EXEC_ARGS_MAX - *used) return -E2BIG;
        if (!uvec) break;
        if (copy_from_user(&p, uvec + n * sizeof(char *), sizeof(p)) != 0) return -EFAULT;
        if (!p) break;
    }

    const char **vec = (const char **)(area + *used);
    *used += (n + 1) * sizeof(char *);

    for (size_t i = 0; i < n; i++) {
        if (copy_from_user(&p, uvec + i * sizeof(char *), sizeof(p)) != 0) return -EFAULT;

        long len = strncpy_from_user(area + *used, p, EXEC_ARGS_MAX - *used);
        if (len < 0) return len == -ENAMETOOLONG ? -E2BIG : len;

        vec[i] = area + *used;
        *used += len + 1;
    }

    vec[n] = NULL;
    *out = vec;
    return 0;
}

long sys_execve(long upath, long uargv, long uenvp) {
    struct task *curr = current_task();

    /* one task to replace. the mm may be shared with a parent that waits in vfork: this task only leaves it */
    if (__atomic_load_n(&curr->group->live, 2) != 1 || __atomic_load_n(&curr->sighand->users, 2) != 1) /* __ATOMIC_ACQUIRE */
        return -EBUSY;

    char *path, *name = kmalloc(PATH_MAX);
    if (!name) return -ENOMEM;

    /* the name as it was given, for a script and AT_EXECFN */
    long len = strncpy_from_user(name, upath, PATH_MAX - 1);
    int err = len < 0 ? len : len == 0 ? -ENOENT : path_from_user(AT_FDCWD, upath, &path);
    if (err) {
        kfree(name);
        return err;
    }

    int order = SIZE_TO_ORDER(EXEC_ARGS_MAX);
    uintptr_t area = (uintptr_t)alloc_pages(order);
    if (!area) {
        kfree(path);
        kfree(name);
        return -ENOMEM;
    }

    const char **argv, **envp;
    size_t used = 0;

    struct mm *mm;
    uintptr_t entry, sp;

    if ((err = copy_strings(uargv, (char *)P2V(area), &used, &argv)) == 0 &&
        (err = copy_strings(uenvp, (char *)P2V(area), &used, &envp)) == 0)
        err = exec_load(path, name, argv, envp, &mm, &entry, &sp);

    free_pages((void *)area, order);
    kfree(path);
    kfree(name);

    if (err) return err;

    /* no way back from here: the old program is gone */
    task_exec_mm(mm);
    files_close_on_exec(curr->files);
    signal_exec_reset();
    arch_exec_regs(curr->tf, entry, sp);

    return 0;
}
