#include <serika/errno.h>
#include <serika/exec.h>
#include <serika/sched.h>
#include <serika/syscall.h>

#include <tests/test.h>

bool test_exec() {
    struct task *t;

    TEST_ASSERT(task_create_exec("/bin/nope", NULL, NULL, 0, &t) == -ENOENT);
    TEST_ASSERT(task_create_exec("/etc/motd", NULL, NULL, 0, &t) == -ENOEXEC);
    TEST_ASSERT(task_create_exec("/bin", NULL, NULL, 0, &t) == -EACCES);
    return true;
}

bool test_exec_glibc() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/hello", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

/* files, the console, time and munmap, through glibc */
bool test_exec_files() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/files", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

/* a shell and its applets on a static binary: pipes, redirection, the cwd, fork and execve */
bool test_exec_busybox() {
    const char *const argv[] = { "/bin/busybox", "sh", "/etc/busybox.sh", NULL };
    struct task *t;
    long status = 0;

    TEST_ASSERT(task_create_exec("/bin/busybox", argv, NULL, 0, &t) == 0);
    sched_add_task(t);

    TEST_ASSERT(reap_within(t, &status, 20000) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0); /* the script's number of the check that failed */
    return true;
}

/* getrandom, madvise, poll, select, sendfile and a robust mutex, through glibc */
bool test_exec_syscalls() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/syscalls", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

/* mesa's llvmpipe: llvm compiles the shaders and the rasterizer runs on threads, on a context with no display */
bool test_exec_gl() {
    const char *const argv[] = { "/bin/gltest", NULL };
    const char *const envp[] = { "LIBGL_ALWAYS_SOFTWARE=1", "EGL_PLATFORM=surfaceless", "HOME=/tmp",
                                 "MESA_SHADER_CACHE_DISABLE=true", "LP_NUM_THREADS=2", NULL };
    struct task *t;
    long status = 0;

    TEST_ASSERT(task_create_exec("/bin/gltest", argv, envp, 0, &t) == 0);
    sched_add_task(t);

    TEST_ASSERT(reap_within(t, &status, 120000) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}
