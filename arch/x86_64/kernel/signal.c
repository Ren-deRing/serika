#include <serika/arch.h>
#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/percpu.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/string.h>
#include <serika/task.h>

#include <asm/cpu.h>
#include <asm/task.h>
#include <asm/trapframe.h>

#include <stddef.h>

struct sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate; /* the user address of the xsave area */
    uint64_t reserved[8];
};

struct ucontext {
    uint64_t          uc_flags;
    uint64_t          uc_link;
    uint64_t          ss_sp;    /* stack_t. no sigaltstack yet */
    int32_t           ss_flags;
    int32_t           __pad;
    uint64_t          ss_size;
    struct sigcontext uc_mcontext;
    uint64_t          uc_sigmask;
};

struct siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t __pad;
    union {
        struct { int32_t pid; uint32_t uid; } kill;
        uint64_t addr;
        uint8_t  __size[112];
    };
};

struct rt_sigframe {
    uint64_t        pretcode; /* the restorer: the handler returns into it */
    struct ucontext uc;
    struct siginfo  info;
};

_Static_assert(sizeof(struct sigcontext) == 256, "sigcontext");
_Static_assert(sizeof(struct ucontext) == 304, "ucontext");
_Static_assert(sizeof(struct siginfo) == 128, "siginfo");

#define RFLAGS_TF 0x00100
#define RFLAGS_DF 0x00400
#define RFLAGS_RF 0x10000

/* what a frame may set: the arithmetic flags, TF, DF, AC and RF. never IF, IOPL, NT or VM */
#define RFLAGS_USER 0x50dd5ULL

#define RED_ZONE 128

static uint64_t xcr0(void) {
    uint32_t lo, hi;
    asm volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

static bool fault_signal(int sig) {
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP;
}

/* irq off, on the way to user mode. false: it could not be written, the task dies */
static bool setup_frame(struct trapframe *tf, int sig, const struct k_sigaction *ka, const struct ksiginfo *info) {
    struct task *curr = current_task();
    uint32_t xsize = this_cpu_read(arch_cpu_data)->xsave_user_size;
    uint64_t mask = xcr0();

    if (ka->handler >= USER_SPACE_END || ka->restorer >= USER_SPACE_END) return false;

    /* below the red zone: the xsave area, 64 aligned, then the frame, as if called */
    uintptr_t fp    = ALIGN_DOWN(tf->rsp - RED_ZONE - xsize, 64);
    uintptr_t frame = ALIGN_DOWN(fp - sizeof(struct rt_sigframe), 16) - 8;

    if (frame >= tf->rsp) return false; /* wrapped */

    /* the task's fpu state is still in the registers: the kernel uses none */
    int order = SIZE_TO_ORDER(xsize);
    uintptr_t buf = (uintptr_t)alloc_pages(order);
    if (!buf) return false;

    memset((void *)P2V(buf), 0, ORDER_TO_SIZE(order));
    asm volatile ("xsave (%0)" :: "r"(P2V(buf)), "a"((uint32_t)mask), "d"((uint32_t)(mask >> 32)) : "memory");

    int err = copy_to_user(fp, (void *)P2V(buf), xsize);
    free_pages((void *)buf, order);
    if (err) return false;

    /* zeroed first: nothing of the kernel's stack goes out */
    struct rt_sigframe f;
    memset(&f, 0, sizeof(f));

    f.pretcode = ka->restorer;

    struct sigcontext *sc = &f.uc.uc_mcontext;

    sc->r8  = tf->r8;  sc->r9  = tf->r9;  sc->r10 = tf->r10; sc->r11 = tf->r11;
    sc->r12 = tf->r12; sc->r13 = tf->r13; sc->r14 = tf->r14; sc->r15 = tf->r15;
    sc->rdi = tf->rdi; sc->rsi = tf->rsi; sc->rbp = tf->rbp; sc->rbx = tf->rbx;
    sc->rdx = tf->rdx; sc->rax = tf->rax; sc->rcx = tf->rcx; sc->rsp = tf->rsp;
    sc->rip = tf->rip;
    sc->eflags  = tf->rflags;
    sc->cs      = tf->cs;
    sc->ss      = tf->ss;
    sc->err     = tf->error;
    sc->trapno  = tf->vector;
    sc->oldmask = curr->blocked;
    sc->cr2     = info->addr;
    sc->fpstate = fp;

    f.uc.uc_sigmask = curr->blocked;

    f.info.si_signo = sig;
    f.info.si_code  = info->code;

    if (fault_signal(sig)) f.info.addr = info->addr;
    else                   f.info.kill.pid = info->pid;

    if (copy_to_user(frame, &f, sizeof(f)) != 0) return false;

    tf->rip = ka->handler;
    tf->rsp = frame;
    tf->rdi = sig;
    tf->rsi = frame + offsetof(struct rt_sigframe, info);
    tf->rdx = frame + offsetof(struct rt_sigframe, uc);
    tf->rax = 0;

    /* the abi enters functions with DF clear */
    tf->rflags &= ~(RFLAGS_DF | RFLAGS_TF | RFLAGS_RF);

    return true;
}

/* xrstor faults in the kernel on what it refuses. check all of it first */
static bool xstate_valid(const uint8_t *x, uint64_t mask) {
    uint32_t mxcsr = *(const uint32_t *)(x + 24);
    uint64_t xstate_bv = *(const uint64_t *)(x + 512);

    if (mxcsr & 0xffff0000u) return false; /* reserved */
    if (xstate_bv & ~mask) return false;   /* a component xcr0 has not */

    /* xcomp_bv and the rest of the header: zero in the standard form */
    for (int i = 520; i < 576; i++)
        if (x[i]) return false;

    return true;
}

/* rt_sigreturn. the frame is the user's, anything in it may be hostile. false: the task dies */
bool signal_restore(struct trapframe *tf) {
    uint32_t xsize = this_cpu_read(arch_cpu_data)->xsave_user_size;
    uint64_t mask = xcr0();
    struct rt_sigframe f;

    /* the handler's ret took pretcode */
    if (copy_from_user(&f, tf->rsp - 8, sizeof(f)) != 0) return false;

    struct sigcontext *sc = &f.uc.uc_mcontext;

    /* iretq to a non-canonical rip or rsp faults in the kernel */
    if (sc->rip >= USER_SPACE_END || sc->rsp >= USER_SPACE_END) return false;

    int order = SIZE_TO_ORDER(xsize);
    uintptr_t buf = (uintptr_t)alloc_pages(order);
    if (!buf) return false;

    uint8_t *x = (uint8_t *)P2V(buf);
    memset(x, 0, ORDER_TO_SIZE(order));

    if (sc->fpstate && (copy_from_user(x, sc->fpstate, xsize) != 0 || !xstate_valid(x, mask))) {
        free_pages((void *)buf, order);
        return false;
    }

    tf->r8  = sc->r8;  tf->r9  = sc->r9;  tf->r10 = sc->r10; tf->r11 = sc->r11;
    tf->r12 = sc->r12; tf->r13 = sc->r13; tf->r14 = sc->r14; tf->r15 = sc->r15;
    tf->rdi = sc->rdi; tf->rsi = sc->rsi; tf->rbp = sc->rbp; tf->rbx = sc->rbx;
    tf->rdx = sc->rdx; tf->rax = sc->rax; tf->rcx = sc->rcx; tf->rsp = sc->rsp;
    tf->rip = sc->rip;

    /* cs and ss stay the user's, whatever the frame says */
    tf->rflags = (tf->rflags & ~RFLAGS_USER) | (sc->eflags & RFLAGS_USER);

    signal_set_blocked(f.uc.uc_sigmask);

    /* no fpstate: the fpu starts over, as linux does */
    if (sc->fpstate) asm volatile ("xrstor (%0)" :: "r"(x), "a"((uint32_t)mask), "d"((uint32_t)(mask >> 32)) : "memory");
    else             arch_fpu_reset();

    free_pages((void *)buf, order);
    return true;
}

/* the default action, or a frame that could not be built */
static void signal_kill(int sig, const struct ksiginfo *info, struct trapframe *tf) {
    struct tgroup *g = current_task()->group;

    arch_irq_enable(); /* task_exit() closes the files */

    /* the others of an exiting process get SIGKILL: only the first death is news */
    if (g && g->exiting)
        ;
    else if (fault_signal(sig) && info->addr)
        printk("signal: task %lu killed by %d -- addr 0x%lx, rip 0x%lx\n", current_task()->id, sig, info->addr, tf->rip);
    else
        printk("signal: task %lu killed by %d -- rip 0x%lx\n", current_task()->id, sig, tf->rip);

    task_group_exit(sig);
}

/* irq off, the last stop before user mode, from a syscall or an interrupt */
void exit_to_user(struct trapframe *tf, bool syscall) {
    for (;;) {
        while (this_cpu_read(rq.need_resched)) schedule();

        struct ksiginfo info;
        struct k_sigaction ka;

        int sig = signal_dequeue(&info, &ka);

        /* a call a signal cut short: again, or -EINTR when a handler without SA_RESTART runs */
        if (syscall && (long)tf->rax == -ERESTARTSYS) {
            if (sig && ka.handler != SIG_DFL && !(ka.flags & SA_RESTART)) {
                tf->rax = -EINTR;
            } else {
                tf->rax = tf->error; /* the number, saved at entry */
                tf->rip -= 2;        /* the syscall instruction */
            }
        }

        if (!sig) return;

        /* the frame is written through the mm, whose lock sleeps. a signal that comes now is seen on the next pass */
        arch_irq_enable();

        if (ka.handler == SIG_DFL) signal_kill(sig, &info, tf);

        if (!(ka.flags & SA_RESTORER) || !setup_frame(tf, sig, &ka, &info)) {
            struct ksiginfo segv = { .signo = SIGSEGV, .code = SI_KERNEL };
            signal_kill(SIGSEGV, &segv, tf);
        }

        signal_delivered(sig, &ka);

        /* the last look is with irq off: after it only the iret can let an ipi in, and that lands in user mode */
        arch_irq_disable();
    }
}
