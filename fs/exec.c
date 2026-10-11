#include <serika/buddy.h>
#include <serika/elf.h>
#include <serika/errno.h>
#include <serika/exec.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <asm/arch.h>

#define ELF_ET_DYN_BASE 0x555555554000ULL      /* linux's, without randomization */
#define EXEC_MAX_PHNUM  64
#define EXEC_ARGS_MAX   (USER_STACK_SIZE / 4)  /* strings, vectors and auxv */
#define EXEC_NR_AUXV    19                     /* AT_NULL included */

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

/* not random: AT_RANDOM only seeds glibc's canaries until there is a real source */
static void fill_random(uint8_t *p, size_t n) {
    uint64_t x = rdtsc() | 1;

    for (size_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        p[i] = (uint8_t)x;
    }
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
    uintptr_t entry;
    uintptr_t phdr;
    uint64_t  phnum;
};

/* sysv: argc, argv, NULL, envp, NULL, auxv at sp (16 aligned), strings above */
static int setup_stack(struct mm *mm, const char *path, const char *const argv[], const char *const envp[],
                       const struct exec_image *img, uintptr_t *sp_out) {
    static const char platform[] = "x86_64";

    size_t argc, envc;
    size_t strings = sizeof(platform) + 16 + strnlen(path, PATH_MAX) + 1;

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
    fill_random(buf + (at - sp), 16);
    at += 16;

    uintptr_t platform_at = put_string(buf, sp, &at, platform);
    uintptr_t execfn_at   = put_string(buf, sp, &at, path);

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
        AT_BASE,     0, /* no interpreter yet */
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

int task_create_exec(const char *path, const char *const argv[], const char *const envp[], int nice,
                     struct task **out) {
    struct file *f;

    int err = vfs_open(path, O_RDONLY, 0, &f);
    if (err) return err;

    Elf64_Ehdr eh;
    Elf64_Phdr *ph = NULL;
    struct mm *mm = NULL;
    uintptr_t bounce = 0;

    if (!S_ISREG(f->inode->mode)) {
        err = -EACCES;
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

    /* dynamic: needs ld.so loaded next to it, not yet */
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            err = -ENOEXEC;
            goto out;
        }
    }

    /* static-pie has no interpreter: it relocates itself wherever it lands */
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

    uintptr_t sp;
    if ((err = setup_stack(mm, path, argv, envp, &img, &sp)) != 0) goto out;

    struct task *task = task_create_mm(mm, img.entry, sp, nice);
    if (!task) {
        err = -ENOMEM;
        goto out;
    }

    mm = NULL; /* the task's now */
    *out = task;

out:
    if (mm) mm_put(mm);
    free_pages((void *)bounce, 0);
    kfree(ph);
    vfs_close(f);
    return err;
}
