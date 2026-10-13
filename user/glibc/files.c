#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* exits 0x5e if every check holds, else the number of the failed one */

static long ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

int main(void) {
    static const char motd[] = "hello from initrd\n";
    char buf[64];
    struct stat st;

    /* stdio on a file */
    FILE *fp = fopen("/etc/motd", "r");
    if (!fp) return 1;
    if (!fgets(buf, sizeof(buf), fp) || strcmp(buf, motd) != 0) return 2;
    if (fgets(buf, sizeof(buf), fp)) return 3; /* at the end */
    fclose(fp);

    /* raw fds, seek, stat */
    int fd = open("/etc/motd", O_RDONLY);
    if (fd < 3) return 4;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != (off_t)strlen(motd)) return 5;
    if (lseek(fd, 6, SEEK_SET) != 6 || read(fd, buf, 4) != 4 || memcmp(buf, "from", 4) != 0) return 6;
    if (lseek(fd, 0, SEEK_END) != (off_t)strlen(motd)) return 7;
    if (isatty(fd)) return 8;
    if (close(fd) != 0 || close(fd) != -1 || errno != EBADF) return 9;

    /* relative paths start at the root */
    fd = open("etc/motd", O_RDONLY);
    if (fd < 0) return 10;
    close(fd);

    if (open("/nope", O_RDONLY) != -1 || errno != ENOENT) return 11;
    if (stat("/bin", &st) != 0 || !S_ISDIR(st.st_mode)) return 12;

    /* a new file, written and read back */
    fd = open("/scratch", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 13;
    if (write(fd, "abc", 3) != 3 || lseek(fd, 0, SEEK_SET) != 0) return 14;
    if (read(fd, buf, sizeof(buf)) != 3 || memcmp(buf, "abc", 3) != 0) return 15;
    if (fstat(fd, &st) != 0 || st.st_size != 3) return 16;
    close(fd);

    /* the console is a tty, cooked: a read would wait for a line, so it is only asked */
    struct termios tio;
    if (!isatty(1)) return 17;
    if (tcgetattr(0, &tio) != 0 || !(tio.c_lflag & ICANON)) return 18;

    /* time */
    struct timespec t0;
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) return 19;
    if (usleep(20000) != 0 || ms_since(&t0) < 20) return 20;

    struct timespec req = { 0, 5000000 };
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0 || nanosleep(&req, NULL) != 0 || ms_since(&t0) < 5) return 21;

    /* munmap gives it back, and mmap still works after */
    char *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 22;
    memset(p, 1, 1 << 20);
    if (munmap(p, 1 << 20) != 0) return 23;

    char *q = malloc(1 << 20);
    if (!q) return 24;
    memset(q, 2, 1 << 20);
    free(q);

    /* fallocate: a hole is zeros, the size stays, and what is around it stays. without keeping the size it is refused */
    int mfd = memfd_create("hole", 0);
    if (mfd < 0) return 25;

    char *page = malloc(3 * 4096);
    if (!page) return 26;
    memset(page, 7, 3 * 4096);
    if (write(mfd, page, 3 * 4096) != 3 * 4096) return 27;

    if (fallocate(mfd, FALLOC_FL_PUNCH_HOLE, 0, 4096) != -1 || errno != EOPNOTSUPP) return 28;
    if (fallocate(mfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 4096 - 16, 4096 + 32) != 0) return 29; /* cuts two pages, drops none */
    if (fallocate(mfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 2 * 4096, 4096 * 4) != 0) return 30; /* past the end too */

    if (fstat(mfd, &st) != 0 || st.st_size != 3 * 4096) return 31;

    memset(page, 1, 3 * 4096);
    if (pread(mfd, page, 3 * 4096, 0) != 3 * 4096) return 32;
    for (int i = 0; i < 3 * 4096; i++) {
        int hole = (i >= 4096 - 16 && i < 2 * 4096 + 16) || i >= 2 * 4096;
        if (page[i] != (hole ? 0 : 7)) return 33;
    }

    /* a mapping that has the pages in does not go on showing them: a whole page and the cut end of another */
    memset(page, 9, 3 * 4096);
    if (pwrite(mfd, page, 3 * 4096, 0) != 3 * 4096) return 34;

    volatile char *map = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (map == MAP_FAILED) return 35;
    if (map[0] != 9 || map[4096] != 9 || map[2 * 4096] != 9) return 36; /* faulted in */

    if (fallocate(mfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 4096, 4096 + 100) != 0) return 37;
    if (map[4096] != 0 || map[2 * 4096 - 1] != 0 || map[2 * 4096 + 99] != 0) return 38;
    if (map[0] != 9 || map[2 * 4096 + 100] != 9 || map[3 * 4096 - 1] != 9) return 39;

    map[4096 + 5] = 3; /* and the hole is a page again */
    if (map[4096 + 5] != 3 || map[4096 + 6] != 0) return 40;

    munmap((void *)map, 3 * 4096);
    free(page);
    close(mfd);

    /* close_range: marked to close on exec, then closed, up to the end */
    int cr[2];
    if (pipe(cr) != 0) return 41;
    if (close_range(cr[0], cr[1], CLOSE_RANGE_CLOEXEC) != 0) return 42;
    if (!(fcntl(cr[0], F_GETFD) & FD_CLOEXEC) || !(fcntl(cr[1], F_GETFD) & FD_CLOEXEC)) return 43;
    if (close_range(cr[0], ~0U, 0) != 0) return 44;
    if (fcntl(cr[0], F_GETFD) != -1 || errno != EBADF || fcntl(cr[1], F_GETFD) != -1) return 45;
    if (close_range(5, 4, 0) != -1 || errno != EINVAL) return 46;

    printf("files: all checks passed\n");
    return 0x5e;
}
