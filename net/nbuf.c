#include <serika/net.h>
#include <serika/slab.h>

/* a packet buffer. the data starts after the headroom, so a header is put on in front without a copy */

struct nbuf *nbuf_alloc(void) {
    struct nbuf *n = kmalloc(sizeof(*n));
    if (!n) return NULL;

    n->node.next = n->node.prev = NULL;
    n->dev  = NULL;
    n->data = n->buf + NBUF_HEADROOM;
    n->len  = 0;
    return n;
}

void nbuf_free(struct nbuf *n) {
    kfree(n);
}

void *nbuf_put(struct nbuf *n, uint32_t len) {
    uint8_t *tail = n->data + n->len;

    if (tail + len > n->buf + sizeof(n->buf)) return NULL;

    n->len += len;
    return tail;
}

void *nbuf_push(struct nbuf *n, uint32_t len) {
    if (n->data - len < n->buf) return NULL;

    n->data -= len;
    n->len  += len;
    return n->data;
}

void *nbuf_pull(struct nbuf *n, uint32_t len) {
    if (len > n->len) return NULL;

    void *p = n->data;

    n->data += len;
    n->len  -= len;
    return p;
}
