#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/net.h>
#include <serika/percpu.h>
#include <serika/preempt.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/string.h>
#include <serika/tcp.h>
#include <serika/task.h>
#include <serika/timer.h>

#include <asm/arch.h>

/* the interfaces, and netd: the thread that takes what the interfaces received through the stack, and keeps the time of
 * ARP. an interrupt only says that there is something (net_schedule()) */

#define NETD_IRQ  (1U << 0)   /* an interface has frames */
#define NETD_TICK (1U << 1)

#define TICK_MS 100

struct mutex      net_lock;
struct wait_queue net_wq;

static struct wait_queue netd_wq;
static uint32_t          netd_work;
static struct timer      tick_timer;

static struct netdev *devices;       /* in the order they were registered */

uint64_t net_ms_to_tsc(uint32_t ms) {
    return rdtsc() + (uint64_t)ms * this_cpu_read(tsc_freq_hz) / 1000;
}

int netdev_register(struct netdev *dev) {
    mutex_lock(&net_lock);

    struct netdev **pp = &devices;
    bool first = !(dev->flags & NETDEV_LOOPBACK);

    while (*pp) {
        if (!((*pp)->flags & NETDEV_LOOPBACK)) first = false;
        pp = &(*pp)->next;
    }

    dev->next = NULL;
    *pp = dev;

    /* the address until DHCP gives one */
    if (first && !dev->ip) {
        dev->ip   = IP4(10, 0, 2, 15);
        dev->mask = IP4(255, 255, 255, 0);
        dev->gw   = IP4(10, 0, 2, 2);
    }

    dev->flags |= NETDEV_UP;
    mutex_unlock(&net_lock);
    return 0;
}

struct netdev *netdev_find(const char *name) {
    mutex_lock(&net_lock);

    struct netdev *dev = devices;
    while (dev && strcmp(dev->name, name) != 0) dev = dev->next;

    mutex_unlock(&net_lock);
    return dev;
}

struct netdev *netdev_first(void) {
    return devices;
}

void netd_kick(uint32_t work) {
    __atomic_fetch_or(&netd_work, work, __ATOMIC_SEQ_CST);
    wake_up(&netd_wq);
}

void net_schedule(struct netdev *dev) {
    __atomic_store_n(&dev->pending, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&netd_work, NETD_IRQ, __ATOMIC_SEQ_CST);
    wake_up(&netd_wq);
}

void net_rx(struct netdev *dev, struct nbuf *n) {
    n->dev = dev;

    if (dev->flags & NETDEV_LOOPBACK) ip_rx(dev, n);
    else eth_rx(dev, n);
}

/* from the timer interrupt */
static void tick_fn(struct timer *t) {
    (void)t;

    __atomic_fetch_or(&netd_work, NETD_TICK, __ATOMIC_SEQ_CST);
    wake_up(&netd_wq);
}

static void tick_arm(void) {
    tick_timer.expires = net_ms_to_tsc(TICK_MS);
    tick_timer.fn      = tick_fn;
    timer_add(&tick_timer);
}

static void netd(void) {
    tick_arm();

    for (;;) {
        wait_event(netd_wq, __atomic_load_n(&netd_work, __ATOMIC_SEQ_CST) != 0);

        uint32_t work = __atomic_exchange_n(&netd_work, 0, __ATOMIC_SEQ_CST);

        mutex_lock(&net_lock);

        if (work & NETD_IRQ) {
            for (struct netdev *dev = devices; dev; dev = dev->next) {
                if (__atomic_exchange_n(&dev->pending, 0, __ATOMIC_SEQ_CST) && dev->poll) dev->poll(dev);
            }
        }

        if (work & NETD_TICK) {
            arp_tick();
            tick_arm();
        }

        if (work & NETD_TCP) tcp_tick();

        wake_up(&net_wq);
        mutex_unlock(&net_lock);
    }
}

extern void loop_init(void);

void netdev_init(void) {
    mutex_init(&net_lock);
    wait_queue_init(&net_wq);
    wait_queue_init(&netd_wq);

    loop_init();
    tcp_init();

    struct task *t = task_create(netd, 10);

    if (t) sched_add_task(t);
    else printk("net: no thread for netd\n");
}
