#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/cpu.h>
#include <serika/vfs.h>

#include <asm/arch.h>

#include <tests/test.h>

/* the sound card (if there is one): what is written to /dev/dsp is taken by the dma, an interrupt at a time, to be more
 * than the buffer holds. silence: it can be run on a machine with the sound off */
bool test_hda_play() {
    struct file *f;

    if (vfs_open("/dev/dsp", O_WRONLY | O_NONBLOCK, 0, &f) != 0) {
        printk("hda: no sound card -- skipped\n");
        return true;
    }

    static uint8_t silence[4096];
    uint64_t pos = 0, total = 0, until = rdtsc() + 3 * this_cpu_read(tsc_freq_hz);

    /* 24 pieces: three times what the buffer has, which is 340 ms of sound that takes that long to be taken */
    while (total < 24 * sizeof(silence) && rdtsc() < until) {
        size_t want = 24 * sizeof(silence) - total;
        long n = kernel_write(f, silence, want < sizeof(silence) ? want : sizeof(silence), &pos);

        if (n > 0) total += n;
        else       TEST_ASSERT(n == -EAGAIN);

        if (n <= 0) arch_pause();
    }

    TEST_ASSERT(total == 24 * sizeof(silence));

    /* one open at a time */
    struct file *g;
    TEST_ASSERT(vfs_open("/dev/dsp", O_WRONLY, 0, &g) == -EBUSY);

    vfs_close(f);

    /* and it can be opened again once it is closed */
    TEST_ASSERT(vfs_open("/dev/dsp", O_WRONLY, 0, &g) == 0);
    vfs_close(g);
    return true;
}
