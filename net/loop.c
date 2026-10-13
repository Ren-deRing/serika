#include <serika/net.h>
#include <serika/string.h>

/* lo, 127.0.0.1/8: what goes out comes in again, through netd. no Ethernet, a packet is an IP packet */

static struct netdev lo;
static list_node     queue = LIST_HEAD_INIT(queue);

static unsigned drop_every, count, dropped;

void loop_set_drop(unsigned every) {
    drop_every = every;
    count = 0;
}

unsigned loop_dropped(void) {
    return dropped;
}

static int loop_xmit(struct netdev *dev, struct nbuf *n) {
    if (drop_every && ++count % drop_every == 0) {
        dropped++;
        nbuf_free(n);
        return 0;
    }

    list_add_tail(&n->node, &queue);
    net_schedule(dev);
    return 0;
}

static void loop_poll(struct netdev *dev) {
    while (!list_empty(&queue)) {
        struct nbuf *n = list_first(&queue, struct nbuf, node);

        list_del(&n->node);
        net_rx(dev, n);
    }
}

void loop_init(void) {
    strcpy(lo.name, "lo");
    lo.mtu   = 1500;
    lo.flags = NETDEV_LOOPBACK | NETDEV_LINK;
    lo.ip    = IP4(127, 0, 0, 1);
    lo.mask  = IP4(255, 0, 0, 0);
    lo.xmit  = loop_xmit;
    lo.poll  = loop_poll;

    netdev_register(&lo);
}
