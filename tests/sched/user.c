#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/sched.h>
#include <serika/syscall.h>

#include <asm/arch.h>

#include <tests/test.h>

static const uint8_t code[] = {
    0xb9, 0x00, 0x00, 0x00, 0x10,       /* mov $0x10000000, %ecx */
    0xff, 0xc9,                         /* 1: dec %ecx           */
    0x75, 0xfc,                         /* jnz 1b                */
    0xb8, 0xff, 0xff, 0x00, 0x00,       /* mov $0xffff, %eax     */
    0x0f, 0x05,                         /* syscall               */
    0x48, 0x83, 0xf8, 0xda,             /* cmp $-38, %rax        */
    0x75, 0x0c,                         /* jne 2f                */
    0xbf, 0x5e, 0x00, 0x00, 0x00,       /* mov $0x5e, %edi       */
    0xb8, 0x3c, 0x00, 0x00, 0x00,       /* mov $60, %eax (exit)  */
    0x0f, 0x05,                         /* syscall (SYS_EXIT)    */
    0xeb, 0xfe,                         /* 2: jmp 2b             */
};

static const uint8_t stack_code[] = {
    0x6a, 0x5e,                         /* push $0x5e            */
    0x5f,                               /* pop %rdi              */
    0xb8, 0x3c, 0x00, 0x00, 0x00,       /* mov $60, %eax (exit)  */
    0x0f, 0x05,                         /* syscall (SYS_EXIT)    */
    0xeb, 0xfe,                         /* jmp .                 */
};

static const uint8_t segv_code[] = {
    0x89, 0x04, 0x25, 0x00, 0x10, 0x00, 0x00, /* mov %eax, 0x1000 */
    0xeb, 0xfe,                               /* jmp .            */
};

/* reaped id, 0 on timeout */
static long run_user(const uint8_t *c, size_t size, long *status) {
    struct task *t = task_create_user(c, size, 0);
    if (!t) return 0;

    sched_add_task(t);

    long reaped = 0;
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz * 2; /* 2s */

    while ((reaped = task_wait(t, status, WNOHANG)) == 0 && rdtsc() < until) arch_pause();

    return reaped;
}

bool test_user_task() {
    long status = 0;

    TEST_ASSERT(run_user(code, sizeof(code), &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

bool test_user_fault() {
    long status = 0;

    /* the stack page comes on the first push */
    TEST_ASSERT(run_user(stack_code, sizeof(stack_code), &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);

    /* no vma there */
    TEST_ASSERT(run_user(segv_code, sizeof(segv_code), &status) > 0);
    TEST_ASSERT(status == 11);
    return true;
}

/* loops a while unless fast, then exits with the code */
static const uint8_t exit_code[] = {
    0xb9, 0x00, 0x00, 0x00, 0x04,       /* mov $0x4000000, %ecx  */
    0xff, 0xc9,                         /* 1: dec %ecx           */
    0x75, 0xfc,                         /* jnz 1b                */
    0xbf, 0x00, 0x00, 0x00, 0x00,       /* mov $code, %edi       */
    0xb8, 0x3c, 0x00, 0x00, 0x00,       /* mov $60, %eax (exit)  */
    0x0f, 0x05,                         /* syscall (SYS_EXIT)    */
    0xeb, 0xfe,                         /* jmp .                 */
};

static struct task *spawn_exit(uint8_t code, bool fast) {
    uint8_t c[sizeof(exit_code)];
    for (size_t i = 0; i < sizeof(c); i++) c[i] = exit_code[i];

    /* one round */
    if (fast) {
        c[1] = 0x01;
        c[4] = 0x00;
    }

    c[10] = code;

    struct task *t = task_create_user(c, sizeof(c), 0);
    if (t) sched_add_task(t);

    return t;
}

static volatile bool wc_done;
static volatile long wc_nohang, wc_mask, wc_echild;

/* sleeps in task_wait() until each child exits */
static void wait_child_main(void) {
    for (int i = 1; i <= 3; i++)
        if (!spawn_exit(i, false)) return;

    wc_nohang = task_wait(NULL, NULL, WNOHANG); /* all still looping */

    for (int i = 0; i < 3; i++) {
        long status = 0;
        if (task_wait(NULL, &status, 0) > 0) wc_mask |= 1 << WEXITSTATUS(status);
    }

    wc_echild = task_wait(NULL, NULL, 0);
    wc_done = true;
}

bool test_wait_child() {
    wc_done = false;
    wc_nohang = -1;
    wc_mask = 0;
    wc_echild = 0;

    struct task *t = task_create(wait_child_main, 0);
    TEST_ASSERT(t);
    sched_add_task(t);

    TEST_ASSERT(wait_for(&wc_done, 2000));
    TEST_ASSERT(wc_nohang == 0);
    TEST_ASSERT(wc_mask == 0xe);
    TEST_ASSERT(wc_echild == -ECHILD);
    return true;
}

static volatile bool orphan_done;
static volatile bool orphan_ok;

/* exits with one zombie child and one still running */
static void orphan_main(void) {
    struct task *zombie = spawn_exit(0, true);
    struct task *running = spawn_exit(0, false);

    orphan_ok = zombie && running;

    /* ours until we reap it or exit */
    while (zombie && __atomic_load_n(&zombie->state, 2) != TASK_ZOMBIE) arch_pause(); /* __ATOMIC_ACQUIRE */

    orphan_done = true;
}

bool test_wait_orphan() {
    orphan_done = false;
    orphan_ok = false;

    struct task *t = task_create(orphan_main, 0);
    TEST_ASSERT(t);
    sched_add_task(t);

    TEST_ASSERT(wait_for(&orphan_done, 1000));
    TEST_ASSERT(orphan_ok);

    /* the running one exits with no parent and frees itself */
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 5; /* 200ms */
    while (rdtsc() < until) arch_pause();

    /* orphans are not ours */
    TEST_ASSERT(task_wait(NULL, NULL, WNOHANG) == -ECHILD);
    return true;
}
