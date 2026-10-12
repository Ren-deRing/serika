#include <serika/arch.h>
#include <serika/errno.h>
#include <serika/lock.h>
#include <serika/mm.h>
#include <serika/printk.h>
#include <serika/random.h>
#include <serika/string.h>
#include <serika/syscall.h>

#include <asm/arch.h>

#define GRND_NONBLOCK 0x01
#define GRND_RANDOM   0x02
#define GRND_INSECURE 0x04

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

#define QR(a, b, c, d)                                  \
    do {                                                \
        a += b; d ^= a; d = ROTL32(d, 16);              \
        c += d; b ^= c; b = ROTL32(b, 12);              \
        a += b; d ^= a; d = ROTL32(d, 8);               \
        c += d; b ^= c; b = ROTL32(b, 7);               \
    } while (0)

/* ChaCha20 under fast key erasure: a block's first 32 bytes are the next key, the other 32 go out.
 * what was output cannot be worked back to an earlier key */
static uint32_t    key[8];
static spinlock_t  random_lock = SPINLOCK_INITIALIZER;

static void chacha_block(const uint32_t k[8], uint8_t out[64]) {
    uint32_t in[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 }; /* "expand 32-byte k" */
    uint32_t x[16];

    memcpy(in + 4, k, 32); /* the counter and nonce stay 0: every block has a key of its own */
    memcpy(x, in, sizeof(x));

    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }

    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + in[i];
        memcpy(out + 4 * i, &v, 4); /* x86_64 is little endian */
    }
}

static bool hw_rdseed(uint64_t *v) {
    for (int i = 0; i < 64; i++) {
        uint8_t ok;
        asm volatile ("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok) :: "cc");
        if (ok) return true;
    }

    return false;
}

static bool hw_rdrand(uint64_t *v) {
    for (int i = 0; i < 16; i++) {
        uint8_t ok;
        asm volatile ("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok) :: "cc");
        if (ok) return true;
    }

    return false;
}

void random_init(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    bool seed = ebx & (1U << 18);

    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    bool rand = ecx & (1U << 30);

    uint64_t s[4];
    bool good = true;

    for (int i = 0; i < 4; i++) {
        if (!((seed && hw_rdseed(&s[i])) || (rand && hw_rdrand(&s[i])))) {
            s[i] = 0;
            good = false;
        }

        s[i] ^= rdtsc(); /* no worse for it */
    }

    if (!good) printk("random: no RDSEED or RDRAND -- the generator is seeded from the tsc, which is NOT secure\n");

    memcpy(key, s, sizeof(key));
}

void get_random_bytes(void *buf, size_t n) {
    uint8_t out[64];
    uint8_t *p = buf;

    uint64_t flags = spin_lock_irqsave(&random_lock);

    while (n) {
        chacha_block(key, out);
        memcpy(key, out, 32);

        size_t chunk = n < 32 ? n : 32;
        memcpy(p, out + 32, chunk);

        p += chunk;
        n -= chunk;
    }

    spin_unlock_irqrestore(&random_lock, flags);

    memset(out, 0, sizeof(out));
}

/* seeded at boot, so it never waits: all three flags only name a pool that does not differ here */
long sys_getrandom(long ubuf, long len, long flags) {
    uint8_t buf[256];
    long done = 0;

    if (flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)) return -EINVAL;
    if (len < 0) return -EINVAL;
    if (len > 0x7fffffff) len = 0x7fffffff;

    while (done < len) {
        size_t n = len - done < (long)sizeof(buf) ? (size_t)(len - done) : sizeof(buf);

        get_random_bytes(buf, n);

        int err = copy_to_user(ubuf + done, buf, n);
        if (err) {
            if (!done) done = err;
            break;
        }

        done += n;
    }

    memset(buf, 0, sizeof(buf));
    return done;
}
