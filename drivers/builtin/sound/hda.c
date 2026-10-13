#include <serika/arch.h>
#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/intc.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/pci.h>
#include <serika/poll.h>
#include <serika/preempt.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/sound.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/timer.h>
#include <serika/vfs.h>
#include <serika/wait.h>

#include <asm/arch.h>

/* an intel high definition audio controller, as qemu has it (intel-hda with a hda-output or hda-duplex codec), and
 * /dev/dsp over it, the way oss had it. there is one stream out: 48 kHz, 16 bits, two channels.
 *
 * the codec is told what to do by verbs, through the controller's immediate command registers. its widgets are found
 * and a way from an output pin to a converter (the dac) is switched on and unmuted. the controller then reads a cyclic
 * buffer by dma, in the pieces of a list: each piece that is played is an interrupt, and the piece is zeroed then, so a
 * program that does not keep up is silence and not the same sound again */

#define HDA_VENDOR      0x8086
#define HDA_DEV_ICH6    0x2668 /* intel-hda */
#define HDA_DEV_ICH9    0x293e /* ich9-intel-hda */

/* the controller's registers */
#define GCAP            0x00
#define GCTL            0x08
#define STATESTS        0x0e
#define INTCTL          0x20
#define INTSTS          0x24
#define ICW             0x60 /* immediate command: the verb */
#define IRR             0x64 /* and the response */
#define ICS             0x68

#define GCTL_CRST       0x1
#define ICS_BUSY        0x1
#define ICS_VALID       0x2
#define INTCTL_GIE      0x80000000u

/* a stream descriptor, from the first output stream */
#define SD_CTL          0x00 /* 32 bits: control, with the stream's tag in bits 20 to 23, and the status in 24 to 31 */
#define SD_STS          0x03 /* the status alone, a byte */
#define SD_LPIB         0x04 /* where the dma is in the buffer */
#define SD_CBL          0x08
#define SD_LVI          0x0c
#define SD_FMT          0x12
#define SD_BDLPL        0x18
#define SD_BDLPU        0x1c

#define SD_CTL_RESET    0x1
#define SD_CTL_RUN      0x2
#define SD_CTL_IOCE     0x4 /* an interrupt for a piece with the flag */
#define SD_STS_BCIS     0x4 /* a piece with the flag is played */
#define SD_STS_ERRORS   0x18

#define STREAM_TAG      1
#define FORMAT          0x0011 /* 48 kHz, 16 bits, two channels */

/* codec verbs: 12 bits of verb and 8 of payload, or 4 and 16 */
#define V_GET_PARAM     0xf0000
#define V_GET_CONN_LIST 0xf0200
#define V_GET_CONFIG    0xf1c00
#define V_SET_CONN_SEL  0x70100
#define V_SET_POWER     0x70500
#define V_SET_STREAM    0x70600
#define V_SET_PIN_CTL   0x70700
#define V_SET_EAPD      0x70c00
#define V_SET_AMP       0x30000
#define V_SET_FORMAT    0x20000

#define P_NODE_COUNT    0x04
#define P_FUNC_TYPE     0x05
#define P_WIDGET_CAPS   0x09
#define P_PIN_CAPS      0x0c
#define P_IN_AMP_CAPS   0x0d
#define P_CONN_LEN      0x0e
#define P_OUT_AMP_CAPS  0x12

#define W_AUD_OUT       0
#define W_MIXER         2
#define W_SELECTOR      3
#define W_PIN           4

#define WC_IN_AMP       (1u << 1)
#define WC_OUT_AMP      (1u << 2)
#define WC_AMP_OVERRIDE (1u << 3)
#define WC_CONN_LIST    (1u << 8)

#define MAX_NODES       64
#define MAX_CONNS       16

/* the buffer: pieces, each of them an interrupt */
#define PIECE           4096
#define PIECES          8
#define BUFSIZE         (PIECE * PIECES)
#define MARGIN          PIECE /* kept free: the dma reads ahead of where it says it is */

struct widget {
    uint32_t caps;
    uint8_t  nconn;
    uint16_t conn[MAX_CONNS];
};

struct hda {
    struct pci_dev   *pci;
    volatile uint8_t *regs;
    volatile uint8_t *sd; /* the first output stream's registers */
    unsigned          sd_index; /* its number: the input streams are before it */

    uint32_t          cad;  /* the codec */
    uint32_t          afg;  /* its audio function group */
    struct widget     w[MAX_NODES];
    uint32_t          nodes; /* the highest widget id + 1 */

    uint8_t          *buf;  /* BUFSIZE, the dma's */
    uintptr_t         buf_phys;
    uint64_t         *bdl;
    uintptr_t         bdl_phys;

    spinlock_t        lock; /* what follows: the interrupt takes it */
    struct wait_queue wq;   /* a writer, for space */
    bool              running;
    uint64_t          wp;        /* bytes written, in all */
    uint64_t          played;    /* bytes the dma has taken */
    uint64_t          zeroed;    /* and of those, the bytes that are zeros again */
    uint32_t          last_lpib;
    bool              open;
};

static struct hda hda;

static uint32_t r32(unsigned off)  { return *(volatile uint32_t *)(hda.regs + off); }
static uint16_t r16(unsigned off)  { return *(volatile uint16_t *)(hda.regs + off); }
static void     w32(unsigned off, uint32_t v) { *(volatile uint32_t *)(hda.regs + off) = v; }
static void     w16(unsigned off, uint16_t v) { *(volatile uint16_t *)(hda.regs + off) = v; }

static uint32_t sd32(unsigned off) { return *(volatile uint32_t *)(hda.sd + off); }
static void     sd_w32(unsigned off, uint32_t v) { *(volatile uint32_t *)(hda.sd + off) = v; }
static void     sd_w16(unsigned off, uint16_t v) { *(volatile uint16_t *)(hda.sd + off) = v; }
static void     sd_w8(unsigned off, uint8_t v)   { *(volatile uint8_t *)(hda.sd + off) = v; }

static void delay_us(uint64_t us) {
    uint64_t until = rdtsc() + this_cpu_read(tsc_freq_hz) / 1000000 * us;

    while (rdtsc() < until) arch_pause();
}

/* ---- the codec ---- */

/* true when done, false at a timeout: about 50 ms */
static bool wait_ics(uint16_t mask, uint16_t want) {
    for (int i = 0; i < 50000; i++) {
        if ((r16(ICS) & mask) == want) return true;
        delay_us(1);
    }

    return false;
}

static int codec_cmd(uint32_t nid, uint32_t verb, uint32_t *resp) {
    if (!wait_ics(ICS_BUSY, 0)) return -EBUSY;

    w16(ICS, ICS_VALID); /* a response that was never taken is gone */
    w32(ICW, hda.cad << 28 | nid << 20 | verb);
    w16(ICS, ICS_BUSY);

    if (!wait_ics(ICS_BUSY | ICS_VALID, ICS_VALID)) return -EIO;

    if (resp) *resp = r32(IRR);

    w16(ICS, ICS_VALID);
    return 0;
}

static uint32_t get_param(uint32_t nid, uint32_t param) {
    uint32_t v = 0;

    codec_cmd(nid, V_GET_PARAM | param, &v);
    return v;
}

/* a widget's id may be a function group's or a widget's */
static uint32_t amp_caps(uint32_t nid, uint32_t param) {
    uint32_t caps = hda.w[nid].caps;

    if (!(caps & WC_AMP_OVERRIDE)) nid = hda.afg;

    return get_param(nid, param);
}

/* the connections of a widget, in the order that a connection select counts them */
static void read_connections(uint32_t nid) {
    struct widget *w = &hda.w[nid];
    uint32_t len = get_param(nid, P_CONN_LEN);
    bool wide = len & 0x80;
    unsigned n = len & 0x7f, per = wide ? 2 : 4;

    if (n > MAX_CONNS) n = MAX_CONNS;

    for (unsigned i = 0; i < n; i += per) {
        uint32_t v = 0;

        codec_cmd(nid, V_GET_CONN_LIST | i, &v);

        for (unsigned j = 0; j < per && i + j < n; j++)
            w->conn[i + j] = wide ? (v >> (16 * j)) & 0xffff : (v >> (8 * j)) & 0xff;
    }

    w->nconn = n;
}

/* a way from the pin to a dac, by what the widgets connect to: the widgets from the pin on, filled into path[]. the
 * length, 0 for none */
static unsigned find_dac(uint32_t nid, uint16_t *path, unsigned depth) {
    if (depth >= 8 || nid >= hda.nodes) return 0;

    uint32_t type = (hda.w[nid].caps >> 20) & 0xf;

    path[depth] = nid;

    if (type == W_AUD_OUT) return depth + 1;
    if (type != W_PIN && type != W_MIXER && type != W_SELECTOR) return 0;

    for (unsigned i = 0; i < hda.w[nid].nconn; i++) {
        unsigned len = find_dac(hda.w[nid].conn[i], path, depth + 1);

        if (len) return len;
    }

    return 0;
}

/* on, and as loud as it goes with no gain over what it is: the 0 dB step */
static void unmute(uint32_t nid, bool output, unsigned input_index) {
    uint32_t caps = output ? amp_caps(nid, P_OUT_AMP_CAPS) : amp_caps(nid, P_IN_AMP_CAPS);
    uint32_t gain = caps & 0x7f;

    uint32_t payload = (output ? 0xb000 : 0x7000) | (output ? 0 : input_index << 8) | gain;

    codec_cmd(nid, V_SET_AMP | payload, NULL);
}

static int codec_setup(void) {
    /* the codec: it says it is there in the state change status after the reset */
    uint16_t present = r16(STATESTS);

    if (!present) return -ENODEV;

    hda.cad = __builtin_ctz(present);

    /* its function groups, and the audio one */
    uint32_t groups = get_param(0, P_NODE_COUNT);
    uint32_t first = (groups >> 16) & 0xff, count = groups & 0xff;

    hda.afg = 0;

    for (uint32_t g = first; g < first + count; g++) {
        if ((get_param(g, P_FUNC_TYPE) & 0xff) == 1) {
            hda.afg = g;
            break;
        }
    }

    if (!hda.afg) return -ENODEV;

    codec_cmd(hda.afg, V_SET_POWER | 0, NULL);

    uint32_t widgets = get_param(hda.afg, P_NODE_COUNT);
    uint32_t wfirst = (widgets >> 16) & 0xff, wcount = widgets & 0xff;

    if (wfirst + wcount > MAX_NODES) return -ENOMEM;

    hda.nodes = wfirst + wcount;

    for (uint32_t nid = wfirst; nid < wfirst + wcount; nid++) {
        hda.w[nid].caps = get_param(nid, P_WIDGET_CAPS);

        if (hda.w[nid].caps & WC_CONN_LIST) read_connections(nid);

        codec_cmd(nid, V_SET_POWER | 0, NULL);
    }

    /* an output pin that something is connected to, the first with a way to a dac */
    uint16_t path[8];
    unsigned len = 0;

    for (uint32_t nid = wfirst; nid < wfirst + wcount && !len; nid++) {
        if (((hda.w[nid].caps >> 20) & 0xf) != W_PIN) continue;
        if (!(get_param(nid, P_PIN_CAPS) & (1u << 4))) continue; /* it can not be an output */

        uint32_t config = 0;
        codec_cmd(nid, V_GET_CONFIG, &config);

        if ((config >> 30) == 1) continue; /* no connection */

        len = find_dac(nid, path, 0);
    }

    if (!len) return -ENODEV;

    /* path[0] is the pin and path[len - 1] the dac: from the pin back, each widget takes what is after it */
    for (unsigned i = 0; i < len; i++) {
        uint32_t nid = path[i];
        uint32_t type = (hda.w[nid].caps >> 20) & 0xf;

        if (i + 1 < len) {
            unsigned index = 0;

            for (unsigned c = 0; c < hda.w[nid].nconn; c++)
                if (hda.w[nid].conn[c] == path[i + 1]) index = c;

            if (hda.w[nid].nconn > 1) codec_cmd(nid, V_SET_CONN_SEL | index, NULL);

            if (hda.w[nid].caps & WC_IN_AMP) unmute(nid, false, index);
        }

        if (hda.w[nid].caps & WC_OUT_AMP) unmute(nid, true, 0);

        if (type == W_PIN) {
            codec_cmd(nid, V_SET_PIN_CTL | 0xc0, NULL); /* out and headphone drive */
            codec_cmd(nid, V_SET_EAPD | 0x02, NULL);
        }
    }

    uint32_t dac = path[len - 1];

    codec_cmd(dac, V_SET_STREAM | (STREAM_TAG << 4), NULL);
    codec_cmd(dac, V_SET_FORMAT | FORMAT, NULL);

    printk("hda: codec %u, output pin %u to dac %u (%u widgets on the way)\n", hda.cad, path[0], dac, len);
    return 0;
}

/* ---- the stream ---- */

static void stream_reset(void) {
    sd_w32(SD_CTL, SD_CTL_RESET);
    for (int i = 0; i < 1000 && !(sd32(SD_CTL) & SD_CTL_RESET); i++) delay_us(10);

    sd_w32(SD_CTL, 0);
    for (int i = 0; i < 1000 && (sd32(SD_CTL) & SD_CTL_RESET); i++) delay_us(10);
}

/* lock held, or no one else here: the buffer empty and the stream stopped */
static void stream_stop(void) {
    stream_reset();

    memset(hda.buf, 0, BUFSIZE);
    hda.running = false;
    hda.wp = hda.played = hda.zeroed = 0;
    hda.last_lpib = 0;
}

static void stream_program(void) {
    for (unsigned i = 0; i < PIECES; i++) {
        hda.bdl[2 * i]     = hda.buf_phys + (uint64_t)i * PIECE;
        hda.bdl[2 * i + 1] = PIECE | (uint64_t)1 << 32; /* its length and the flag for an interrupt */
    }

    sd_w32(SD_BDLPL, (uint32_t)hda.bdl_phys);
    sd_w32(SD_BDLPU, hda.bdl_phys >> 32);
    sd_w32(SD_CBL, BUFSIZE);
    sd_w16(SD_LVI, PIECES - 1);
    sd_w16(SD_FMT, FORMAT);
}

/* lock held. the dma's place in the buffer, and what it has taken of the bytes written. the pieces that are played
 * are zeros again */
static void position_update(void) {
    uint32_t lpib = sd32(SD_LPIB) % BUFSIZE;

    hda.played += (lpib + BUFSIZE - hda.last_lpib) % BUFSIZE;
    hda.last_lpib = lpib;

    uint64_t done = hda.played / PIECE * PIECE;

    while (hda.zeroed < done) {
        memset(hda.buf + hda.zeroed % BUFSIZE, 0, PIECE);
        hda.zeroed += PIECE;
    }
}

/* lock held */
static void stream_start(void) {
    if (hda.running) return;

    sd_w8(SD_STS, SD_STS_BCIS | SD_STS_ERRORS);
    sd_w32(SD_CTL, STREAM_TAG << 20 | SD_CTL_IOCE | SD_CTL_RUN);
    hda.running = true;
}

/* lock held. the bytes a writer may put in now */
static uint64_t space(void) {
    uint64_t ahead = hda.wp > hda.played ? hda.wp - hda.played : 0;

    return ahead + MARGIN >= BUFSIZE ? 0 : BUFSIZE - MARGIN - ahead;
}

static void hda_irq(struct trapframe *tf, void *data) {
    (void)tf; (void)data;

    uint32_t status = r32(INTSTS);

    if (status & (1u << hda.sd_index)) {
        sd_w8(SD_STS, SD_STS_BCIS | SD_STS_ERRORS);

        uint64_t flags = spin_lock_irqsave(&hda.lock);
        if (hda.running) position_update();
        spin_unlock_irqrestore(&hda.lock, flags);

        wake_up(&hda.wq);
    }

    intc_eoi();
}

/* ---- /dev/dsp ---- */

#define SNDCTL_DSP_RESET        0x5000
#define SNDCTL_DSP_SYNC         0x5001
#define SNDCTL_DSP_SPEED        0xc0045002
#define SNDCTL_DSP_STEREO       0xc0045003
#define SNDCTL_DSP_GETBLKSIZE   0xc0045004
#define SNDCTL_DSP_SETFMT       0xc0045005
#define SNDCTL_DSP_CHANNELS     0xc0045006
#define SNDCTL_DSP_POST         0x5008
#define SNDCTL_DSP_SETFRAGMENT  0xc004500a
#define SNDCTL_DSP_GETFMTS      0x8004500b
#define SNDCTL_DSP_GETOSPACE    0x8010500c /* _IOR(P, 12, audio_buf_info): four ints */
#define SNDCTL_DSP_NONBLOCK     0x500e
#define SNDCTL_DSP_GETCAPS      0x8004500f
#define SNDCTL_DSP_GETODELAY    0x80045017

#define AFMT_S16_LE             0x10
#define DSP_CAP_REALTIME        0x400

struct audio_buf_info {
    int fragments;  /* pieces that are free */
    int fragstotal;
    int fragsize;
    int bytes;      /* free */
};

static int dsp_open(struct inode *inode, uint32_t flags) {
    (void)inode;

    if ((flags & O_ACCMODE) == O_RDONLY) return -EINVAL; /* no recording */

    uint64_t f = spin_lock_irqsave(&hda.lock);
    bool busy = hda.open;

    hda.open = true;
    spin_unlock_irqrestore(&hda.lock, f);

    return busy ? -EBUSY : 0;
}

/* what is written is played to the end, for a while: a program that quits right after its last write */
static void dsp_drain(void) {
    uint64_t until = rdtsc() + this_cpu_read(tsc_freq_hz) / 2;

    for (;;) {
        uint64_t f = spin_lock_irqsave(&hda.lock);

        if (hda.wp && !hda.running) stream_start(); /* less than it takes to start on its own */
        if (hda.running) position_update();

        bool done = !hda.wp || hda.played >= hda.wp;

        spin_unlock_irqrestore(&hda.lock, f);

        if (done || rdtsc() >= until) return;

        sleep_until(rdtsc() + this_cpu_read(tsc_freq_hz) / 200); /* 5 ms */
    }
}

static void dsp_release(struct inode *inode, uint32_t flags) {
    (void)inode; (void)flags;

    dsp_drain();

    uint64_t f = spin_lock_irqsave(&hda.lock);
    stream_stop();
    stream_program();
    hda.open = false;
    spin_unlock_irqrestore(&hda.lock, f);
}

static long dsp_write(struct file *f, const void *buf, size_t n) {
    const uint8_t *src = buf;
    size_t done = 0;

    while (done < n) {
        uint64_t flags = spin_lock_irqsave(&hda.lock);

        if (hda.running) position_update();

        /* the program was late: what was to be played has been, and the dma has gone on past. start from here */
        if (hda.running && hda.wp < hda.played) hda.wp = hda.played + 512;

        uint64_t room = space();

        if (room) {
            size_t chunk = n - done < room ? n - done : room;

            if (chunk > BUFSIZE - hda.wp % BUFSIZE) chunk = BUFSIZE - hda.wp % BUFSIZE; /* up to the end of the buffer */

            memcpy(hda.buf + hda.wp % BUFSIZE, src + done, chunk);

            hda.wp += chunk;
            done += chunk;

            if (!hda.running && hda.wp >= 2 * PIECE) stream_start();

            spin_unlock_irqrestore(&hda.lock, flags);
            continue;
        }

        spin_unlock_irqrestore(&hda.lock, flags);

        if (f->flags & O_NONBLOCK) return done ? (long)done : -EAGAIN;

        int err = wait_event_interruptible(hda.wq, space() != 0);
        if (err) return done ? (long)done : err;
    }

    return done;
}

static uint32_t dsp_poll(struct inode *inode, uint32_t fflags, struct poll_table *pt) {
    (void)inode; (void)fflags;

    poll_wait(pt, &hda.wq);

    uint64_t f = spin_lock_irqsave(&hda.lock);
    uint64_t room = space();
    spin_unlock_irqrestore(&hda.lock, f);

    return room >= PIECE ? POLLOUT : 0;
}

static long dsp_ioctl(struct inode *inode, unsigned long cmd, unsigned long arg) {
    (void)inode;

    int v = 0;

    switch (cmd) {
        case SNDCTL_DSP_RESET: {
            uint64_t f = spin_lock_irqsave(&hda.lock);
            stream_stop();
            stream_program();
            spin_unlock_irqrestore(&hda.lock, f);
            return 0;
        }

        case SNDCTL_DSP_SYNC:
        case SNDCTL_DSP_POST:
            dsp_drain();
            return 0;

        /* one format, one rate, two channels: what is asked for is told it is that */
        case SNDCTL_DSP_SPEED:    v = 48000;      break;
        case SNDCTL_DSP_SETFMT:   v = AFMT_S16_LE; break;
        case SNDCTL_DSP_CHANNELS: v = 2;           break;
        case SNDCTL_DSP_STEREO:   v = 1;           break;
        case SNDCTL_DSP_GETFMTS:  v = AFMT_S16_LE; break;
        case SNDCTL_DSP_GETCAPS:  v = DSP_CAP_REALTIME; break;
        case SNDCTL_DSP_GETBLKSIZE: v = PIECE;     break;
        case SNDCTL_DSP_SETFRAGMENT: return 0;     /* the pieces are what they are */
        case SNDCTL_DSP_NONBLOCK: return 0;

        case SNDCTL_DSP_GETOSPACE: {
            uint64_t f = spin_lock_irqsave(&hda.lock);
            uint64_t room = space();
            spin_unlock_irqrestore(&hda.lock, f);

            struct audio_buf_info info = {
                .fragments  = room / PIECE,
                .fragstotal = PIECES - 1,
                .fragsize   = PIECE,
                .bytes      = room,
            };

            return copy_to_user(arg, &info, sizeof(info));
        }

        case SNDCTL_DSP_GETODELAY: {
            uint64_t f = spin_lock_irqsave(&hda.lock);
            if (hda.running) position_update();
            v = hda.wp > hda.played ? hda.wp - hda.played : 0;
            spin_unlock_irqrestore(&hda.lock, f);
            break;
        }

        default:
            return -ENOTTY;
    }

    return copy_to_user(arg, &v, sizeof(v));
}

static const struct inode_ops dsp_ops = {
    .stream       = true,
    .stream_write = dsp_write,
    .ioctl        = dsp_ioctl,
    .poll         = dsp_poll,
    .open         = dsp_open,
    .release      = dsp_release,
};

/* ---- the controller ---- */

static int controller_reset(void) {
    w32(GCTL, 0);
    for (int i = 0; i < 1000 && (r32(GCTL) & GCTL_CRST); i++) delay_us(10);

    w32(GCTL, GCTL_CRST);
    for (int i = 0; i < 1000 && !(r32(GCTL) & GCTL_CRST); i++) delay_us(10);

    if (!(r32(GCTL) & GCTL_CRST)) return -EIO;

    /* the codecs say they are there some time after */
    for (int i = 0; i < 100 && !r16(STATESTS); i++) delay_us(100);

    return 0;
}

void hda_init(void) {
    struct pci_dev *pci = pci_find(HDA_VENDOR, HDA_DEV_ICH6, 0);

    if (!pci) pci = pci_find(HDA_VENDOR, HDA_DEV_ICH9, 0);
    if (!pci) return; /* no sound card: not a failure */

    hda.pci = pci;
    spin_lock_init(&hda.lock);
    wait_queue_init(&hda.wq);

    pci_enable(pci);

    hda.regs = pci_map_bar(pci, 0);
    if (!hda.regs) {
        printk("hda: no registers\n");
        return;
    }

    if (controller_reset() != 0) {
        printk("hda: the controller does not come out of reset\n");
        return;
    }

    uint16_t gcap = r16(GCAP);
    unsigned iss = (gcap >> 8) & 0xf, oss = (gcap >> 12) & 0xf;

    if (!oss) {
        printk("hda: no output streams\n");
        return;
    }

    hda.sd_index = iss;
    hda.sd = hda.regs + 0x80 + 0x20 * iss;

    int err = codec_setup();
    if (err) {
        printk("hda: no codec to play with (%d)\n", err);
        return;
    }

    /* the dma's memory: the buffer, one run of pages, and the list of its pieces */
    void *buf = alloc_pages(SIZE_TO_ORDER(BUFSIZE));
    void *bdl = alloc_pages(0);

    if (!buf || !bdl) {
        printk("hda: out of memory\n");
        return;
    }

    hda.buf_phys = (uintptr_t)buf;
    hda.buf      = (uint8_t *)P2V(hda.buf_phys);
    hda.bdl_phys = (uintptr_t)bdl;
    hda.bdl      = (uint64_t *)P2V(hda.bdl_phys);

    memset(hda.bdl, 0, PAGE_SIZE);

    stream_stop();
    stream_program();

    if (pci_msi_request(pci, hda_irq, NULL) != 0) {
        printk("hda: no interrupt\n");
        return;
    }

    w32(INTCTL, INTCTL_GIE | (1u << iss));

    struct inode *inode = inode_alloc(&dsp_ops, S_IFCHR | 0666);
    if (!inode) return;

    inode->rdev = 14 << 8 | 3; /* oss's /dev/dsp */

    if (vfs_link_inode("/dev/dsp", inode) != 0) {
        iput(inode);
        return;
    }

    iput(inode); /* the name has its own */

    printk("hda: /dev/dsp, 48 kHz 16 bit stereo, %u pieces of %u bytes\n", PIECES, PIECE);
}
