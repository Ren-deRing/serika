#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/sched.h>
#include <serika/syscall.h>

#include <asm/arch.h>

#include <tests/test.h>

#define THREAD_SHARED 0x200000ULL
#define THREAD_STACK  0x300000ULL

static const uint8_t thread_code[] = {
    /* process: spin until the thread writes the shared word, exit with it */
    0x8b, 0x04, 0x25, 0x00, 0x00, 0x20, 0x00, /* 1: mov 0x200000, %eax   */
    0x85, 0xc0,                               /* test %eax, %eax         */
    0x74, 0xf5,                               /* jz 1b                   */
    0x89, 0xc7,                               /* mov %eax, %edi          */
    0xb8, 0x3c, 0x00, 0x00, 0x00,             /* mov $60, %eax (exit)    */
    0x0f, 0x05,                               /* syscall (SYS_EXIT)      */
    0xeb, 0xfe,                               /* jmp .                   */
    [0x40] =
    /* thread: write the shared word, exit(0) through its own stack */
    0xc7, 0x04, 0x25, 0x00, 0x00, 0x20, 0x00, /* movl $0x5e, 0x200000    */
    0x5e, 0x00, 0x00, 0x00,
    0x6a, 0x00,                               /* push $0                 */
    0x5f,                                     /* pop %rdi                */
    0xb8, 0x3c, 0x00, 0x00, 0x00,             /* mov $60, %eax (exit)    */
    0x0f, 0x05,                               /* syscall (SYS_EXIT)      */
    0xeb, 0xfe,                               /* jmp .                   */
};

bool test_thread_mm() {
    struct task *proc = task_create_user(thread_code, sizeof(thread_code), 0);
    TEST_ASSERT(proc);

    /* before either runs: the process reads the shared word right away */
    TEST_ASSERT(mm_map(proc->mm, THREAD_SHARED, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0);
    TEST_ASSERT(mm_map(proc->mm, THREAD_STACK, 16 * PAGE_SIZE, PROT_READ | PROT_WRITE) == 0);

    struct task *thread = task_create_thread(proc, USER_CODE_BASE + 0x40, THREAD_STACK + 16 * PAGE_SIZE, 0);
    TEST_ASSERT(thread);
    TEST_ASSERT(thread->mm == proc->mm);

    long tid = thread->id;

    sched_add_task(proc);
    sched_add_task(thread);

    /* the process is reaped once both threads are gone. a thread is nobody's child: it frees itself */
    long status = 0;

    TEST_ASSERT(reap_within(proc, &status, 2000) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    TEST_ASSERT(task_waitpid(tid, NULL, WNOHANG) == -ECHILD);
    return true;
}


/* pthreads through glibc: clone threads, futexes, tls, and how a threaded process ends */
bool test_thread_glibc() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/threads", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);

    /* exit() in one thread ends one blocked in a futex too */
    TEST_ASSERT(run_exec_mode("/bin/threads", "exitgroup", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x42);

    /* the main thread leaves first, the process ends with the last one */
    TEST_ASSERT(run_exec_mode("/bin/threads", "leaderexit", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}
