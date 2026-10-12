#pragma once

#include <serika/list.h>
#include <serika/lock.h>
#include <serika/wait.h>

#include <stdbool.h>
#include <stdint.h>

/* linux's input events, as /dev/input/event* give them */

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_SW  0x05
#define EV_LED 0x11
#define EV_SND 0x12
#define EV_REP 0x14
#define EV_FF  0x15
#define EV_MAX 0x1f

#define SYN_REPORT  0
#define SYN_DROPPED 3

#define KEY_MAX 0x2ff
#define REL_MAX 0x0f
#define ABS_MAX 0x3f
#define MSC_MAX 0x07
#define LED_MAX 0x0f
#define SW_MAX  0x10
#define SND_MAX 0x07
#define FF_MAX  0x7f
#define INPUT_PROP_MAX 0x1f

#define BITS_TO_BYTES(n) (((n) + 7) / 8)

struct input_absinfo {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
};

struct input_event {
    int64_t  tv_sec;
    int64_t  tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

_Static_assert(sizeof(struct input_event) == 24, "input_event");
_Static_assert(sizeof(struct input_absinfo) == 24, "input_absinfo");

/* a device that makes events. the driver fills it in and registers it (input_register(), task context); it then calls
 * input_event() from wherever it hears from the device, interrupts too */
struct input_dev {
    char     name[64];
    char     phys[32];
    uint16_t bustype, vendor, product, version;

    /* what it can do: a bit for each */
    uint8_t  evbit[BITS_TO_BYTES(EV_MAX + 1)];
    uint8_t  keybit[BITS_TO_BYTES(KEY_MAX + 1)];
    uint8_t  relbit[BITS_TO_BYTES(REL_MAX + 1)];
    uint8_t  absbit[BITS_TO_BYTES(ABS_MAX + 1)];
    uint8_t  mscbit[BITS_TO_BYTES(MSC_MAX + 1)];
    uint8_t  ledbit[BITS_TO_BYTES(LED_MAX + 1)];
    uint8_t  swbit[BITS_TO_BYTES(SW_MAX + 1)];
    uint8_t  propbit[BITS_TO_BYTES(INPUT_PROP_MAX + 1)];
    struct input_absinfo absinfo[ABS_MAX + 1];

    const char *parent_path; /* where it is in /sys/devices, without the leading slash; "" for none */

    /* the evdev node and what is kept for it */
    int        index;        /* event<index> */
    spinlock_t lock;         /* keys, clients */
    uint8_t    key[BITS_TO_BYTES(KEY_MAX + 1)]; /* pressed now */
    list_node  clients;
};

static inline void input_set_bit(uint8_t *bits, unsigned n) {
    bits[n / 8] |= 1u << (n % 8);
}

static inline bool input_test_bit(const uint8_t *bits, unsigned n) {
    return bits[n / 8] & (1u << (n % 8));
}

/* the sysfs and udev entries and /dev/input/event<n> for the device. task context, with the file system up */
int  input_register(struct input_dev *dev);

/* a device event, which goes to every file that has the node open. any context */
void input_event(struct input_dev *dev, uint16_t type, uint16_t code, int32_t value);

void virtio_input_init(void); /* a device for each virtio input, after input_register() can be used */
