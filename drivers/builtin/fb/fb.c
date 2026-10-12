#include <serika/boot.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/printk.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/vfs.h>

/* /dev/fb0: the framebuffer the bootloader set up. a program maps it and draws, or reads and writes it as a file */

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611

#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR   2

/* linux's, which is what a program that draws on a framebuffer reads */
struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin, hsync_len, vsync_len;
    uint32_t sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};

struct fb_fix_screeninfo {
    char     id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

_Static_assert(sizeof(struct fb_var_screeninfo) == 160, "fb_var_screeninfo");
_Static_assert(sizeof(struct fb_fix_screeninfo) == 80, "fb_fix_screeninfo");

static uintptr_t fb_phys; /* of the first pixel */
static size_t    fb_len;  /* pitch * height, whole pages */

static void fill_var(struct fb_var_screeninfo *v) {
    const struct framebuffer *fb = &g_bootinfo.fb;

    memset(v, 0, sizeof(*v));

    v->xres = v->xres_virtual = fb->width;
    v->yres = v->yres_virtual = fb->height;
    v->bits_per_pixel = fb->bpp;

    v->red   = (struct fb_bitfield){ fb->red_shift, fb->red_size, 0 };
    v->green = (struct fb_bitfield){ fb->green_shift, fb->green_size, 0 };
    v->blue  = (struct fb_bitfield){ fb->blue_shift, fb->blue_size, 0 };
}

static void fill_fix(struct fb_fix_screeninfo *f) {
    memset(f, 0, sizeof(*f));

    strcpy(f->id, "serika fb");

    f->smem_start  = fb_phys;
    f->smem_len    = fb_len;
    f->type        = FB_TYPE_PACKED_PIXELS;
    f->visual      = FB_VISUAL_TRUECOLOR;
    f->line_length = g_bootinfo.fb.pitch;
}

/* the mode is the boot's: a program may ask for it by name, and gets it back */
static long fb_ioctl(struct inode *inode, unsigned long cmd, unsigned long arg) {
    (void)inode;

    switch (cmd) {
        case FBIOGET_VSCREENINFO:
        case FBIOPUT_VSCREENINFO: {
            struct fb_var_screeninfo v;

            fill_var(&v);
            return copy_to_user(arg, &v, sizeof(v));
        }

        case FBIOGET_FSCREENINFO: {
            struct fb_fix_screeninfo f;

            fill_fix(&f);
            return copy_to_user(arg, &f, sizeof(f));
        }

        case FBIOPAN_DISPLAY:
        case FBIOBLANK:
            return 0; /* one buffer, always on */
    }

    return -ENOTTY;
}

static long fb_read(struct inode *inode, void *buf, size_t n, uint64_t pos) {
    (void)inode;

    if (pos >= fb_len) return 0;
    if (n > fb_len - pos) n = fb_len - pos;

    memcpy(buf, (void *)P2V(fb_phys + pos), n);
    return n;
}

static long fb_write(struct inode *inode, const void *buf, size_t n, uint64_t pos) {
    (void)inode;

    if (pos >= fb_len) return n ? -ENOSPC : 0;
    if (n > fb_len - pos) n = fb_len - pos;

    memcpy((void *)P2V(fb_phys + pos), buf, n);
    return n;
}

/* the frames are the device's, not the allocator's: each has the reference that keeps a mapping from ever freeing it */
static uintptr_t fb_getpage(struct inode *inode, uint64_t index) {
    (void)inode;

    if (index >= fb_len >> PAGE_SHIFT) return 0;

    frame_get(fb_phys + (index << PAGE_SHIFT));
    return fb_phys + (index << PAGE_SHIFT);
}

static const struct inode_ops fb_ops = {
    .read    = fb_read,
    .write   = fb_write,
    .ioctl   = fb_ioctl,
    .getpage = fb_getpage,
    .map_wc  = true,
};

void fb_devices_create(void) {
    const struct framebuffer *fb = &g_bootinfo.fb;

    if (!fb->fb_addr || fb->bpp != 32) {
        printk("fb: none, or not 32 bits per pixel -- no /dev/fb0\n");
        return;
    }

    /* limine gives the address in the direct map */
    fb_phys = V2P((uintptr_t)fb->fb_addr);
    fb_len  = ALIGN_UP((size_t)fb->pitch * fb->height, PAGE_SIZE);

    if (fb_phys + fb_len > g_bootinfo.mem.max_phys_addr) {
        printk("fb: 0x%lx+0x%lx is past the memory map -- no /dev/fb0\n", fb_phys, fb_len);
        return;
    }

    for (size_t off = 0; off < fb_len; off += PAGE_SIZE) frame_init(fb_phys + off);

    struct inode *inode = inode_alloc(&fb_ops, S_IFCHR | 0666);
    if (!inode) panic("fb: unable to create /dev/fb0 -- out of memory?");

    inode->rdev = 0x1d00; /* 29:0 */
    inode->size = fb_len;

    int err = vfs_link_inode("/dev/fb0", inode);
    if (err) panic("fb: unable to create /dev/fb0 -- error %d", err);

    iput(inode);

    printk("fb: %ux%u, %u bits, at 0x%lx\n", fb->width, fb->height, fb->bpp, fb_phys);
}
