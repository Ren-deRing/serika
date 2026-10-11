#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* exits 0x5e if every check holds, else the number of the failed one.
   "fdcheck <closed fd> <kept fd>" is how it runs after its own execve */

static int global = 1;

static int fdcheck(int closed, int kept) {
    char c;

    if (read(closed, &c, 1) != -1 || errno != EBADF) return 1;
    if (read(kept, &c, 1) != 1) return 2;
    return 0x5e;
}

/* the child's exit code, -1 if it did not exit by itself */
static int wait_child(pid_t pid) {
    int status;

    if (waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "fdcheck") == 0) return fdcheck(atoi(argv[2]), atoi(argv[3]));

    /* the child's writes stay in the child */
    volatile int local = 2;
    char *heap = malloc(64);
    strcpy(heap, "parent");

    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        global = 10;
        local = 20;
        strcpy(heap, "child");
        _exit(global + local == 30 && strcmp(heap, "child") == 0 ? 7 : 8);
    }
    if (wait_child(pid) != 7) return 2;
    if (global != 1 || local != 2 || strcmp(heap, "parent") != 0) return 3;

    /* and the parent's stay in the parent */
    pid = fork();
    if (pid < 0) return 4;
    if (pid == 0) {
        usleep(20000);
        _exit(global == 1 ? 0 : 1);
    }
    global = 100;
    if (wait_child(pid) != 0) return 5;
    global = 1;

    /* a big region, written on both sides after the fork */
    size_t size = 4 << 20;
    unsigned char *big = malloc(size);
    if (!big) return 6;
    for (size_t i = 0; i < size; i += 4096) big[i] = (unsigned char)(i >> 12);

    pid = fork();
    if (pid < 0) return 7;
    if (pid == 0) {
        for (size_t i = 0; i < size; i += 4096)
            if (big[i] != (unsigned char)(i >> 12)) _exit(1);
        for (size_t i = 0; i < size; i += 4096) big[i] = 0xaa;
        _exit(0);
    }
    for (size_t i = 0; i < size; i += 8192) big[i] = 0x55;
    if (wait_child(pid) != 0) return 8;
    for (size_t i = 0; i < size; i += 4096)
        if (big[i] != ((i / 4096) % 2 == 0 ? 0x55 : (unsigned char)(i >> 12))) return 9;
    free(big);

    /* no child, then one still running */
    if (waitpid(-1, NULL, WNOHANG) != -1 || errno != ECHILD) return 10;

    pid = fork();
    if (pid < 0) return 11;
    if (pid == 0) {
        usleep(20000);
        _exit(3);
    }
    if (waitpid(pid, NULL, WNOHANG) != 0) return 12;

    int status;
    if (waitpid(-1, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 3) return 13;

    /* execve: another program's exit code comes back */
    pid = fork();
    if (pid == 0) {
        char *args[] = { "/bin/hello", NULL };
        execv("/bin/hello", args);
        _exit(1);
    }
    if (wait_child(pid) != 0x5e) return 14;

    /* a failed execve returns, and the caller goes on */
    pid = fork();
    if (pid == 0) {
        char *args[] = { "/nope", NULL };
        execv("/nope", args);
        _exit(errno == ENOENT ? 0 : 1);
    }
    if (wait_child(pid) != 0) return 15;

    /* close-on-exec fds go, the others stay */
    int closed = open("/etc/motd", O_RDONLY | O_CLOEXEC);
    int kept = open("/etc/motd", O_RDONLY);
    if (closed < 0 || kept < 0) return 16;

    pid = fork();
    if (pid == 0) {
        char a[16], b[16];
        snprintf(a, sizeof(a), "%d", closed);
        snprintf(b, sizeof(b), "%d", kept);

        char *args[] = { "/bin/fork", "fdcheck", a, b, NULL };
        execv("/bin/fork", args);
        _exit(1);
    }
    if (wait_child(pid) != 0x5e) return 17;

    printf("fork: all checks passed\n");
    return 0x5e;
}
