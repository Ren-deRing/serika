#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/list.h>
#include <serika/lock.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/slab.h>

/*
 * Every cpu has a slab of each cache that it allocates from (the active slab) and a few more it may take next (its
 * partial list). They are frozen: that cpu's, and on no list of the cache. An allocation takes the next object of the
 * cpu's freelist, and a free from the same cpu puts it back there: with irq off, which is all the lock either needs.
 *
 * A free from another cpu cannot touch that freelist. It goes to the slab's own freelist instead, with one
 * compare and swap on the slab's state: the head of that freelist, how many objects are in use and whether the slab
 * is frozen, in one word. The owner takes the whole freelist with another when its own is empty. Nothing is ever popped
 * from the slab's freelist one object at a time, so there is nothing to do against ABA.
 *
 * A slab nobody owns is on the cache's partial list (some objects free; the cache's lock) or on none (all in use).
 * The free that makes a full slab not full takes it for its own cpu and puts it on that cpu's partial list. The one
 * that empties a slab on the partial list gives it back to the buddy if there are enough there already.
 *
 * "In use" counts what is on a cpu's freelist, and what the slab hands out when its freelist is taken: it is the
 * objects minus the length of the slab's freelist.
 */

#define KMALLOC_CACHE_COUNT 12

#define MIN_PARTIAL      4 /* empty slabs the cache keeps. the rest go back to the buddy */
#define CPU_PARTIAL_MAX  4 /* slabs a cpu holds besides the active one */
#define CPU_PARTIAL_FILL 2 /* what it takes from the cache's list along with the active one */

static struct kmem_cache kmalloc_caches[KMALLOC_CACHE_COUNT] = {
    { .size = 8 },
    { .size = 16 },
    { .size = 32 },
    { .size = 64 },
    { .size = 96 },
    { .size = 128 },
    { .size = 192 },
    { .size = 256 },
    { .size = 512 },
    { .size = 1024 },
    { .size = 2048 },
    { .size = 4096 },
};

/* a cpu's use of a cache. irq off, and its own cpu's */
struct kmem_cpu {
    void       *freelist; /* free objects of the active slab, linked through their first word */
    struct page *page;    /* the active slab */
    list_node   partial;  /* frozen slabs, by page_list */
    unsigned    nr_partial;
};

/* a cpu's caches together, and on lines of their own: nobody else touches them */
struct cpu_slabs {
    struct kmem_cpu c[KMALLOC_CACHE_COUNT];
} __attribute__((aligned(64)));

static struct cpu_slabs cpu_slabs[MAX_CPUS];

/* the state word of a slab */
#define OFF_NONE  0xffffu          /* no head: nothing on the slab's freelist */
#define ST_FROZEN (1ULL << 32)

static inline uint64_t st_make(uint32_t head, uint32_t inuse, bool frozen) {
    return head | (uint64_t)inuse << 16 | (frozen ? ST_FROZEN : 0);
}

static inline uint32_t st_head(uint64_t s)   { return s & 0xffff; }
static inline uint32_t st_inuse(uint64_t s)  { return (s >> 16) & 0xffff; }
static inline bool     st_frozen(uint64_t s) { return s & ST_FROZEN; }

static inline uint64_t st_load(struct page *page) {
    return __atomic_load_n(&page->slab.state, __ATOMIC_ACQUIRE);
}

static inline bool st_cas(struct page *page, uint64_t *old, uint64_t new) {
    return __atomic_compare_exchange_n(&page->slab.state, old, new, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline uintptr_t slab_base(struct page *page) {
    return P2V(PFN_TO_PHYS(page_to_pfn(page)));
}

static inline struct page *obj_page(void *obj) {
    return pfn_to_page(PHYS_TO_PFN(V2P((uintptr_t)obj)));
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=rm"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile ("push %0; popfq" :: "rm"(flags) : "memory", "cc");
}

static inline struct kmem_cpu *this_cpu_cache(struct kmem_cache *cache) {
    return &cpu_slabs[this_cpu_read(id)].c[cache->index];
}

struct kmem_cache *find_cache(size_t size) {
    for (size_t i = 0; i < KMALLOC_CACHE_COUNT; i++) {
        if (size <= kmalloc_caches[i].size)
            return &kmalloc_caches[i];
    }

    return NULL;
}

struct kmem_cache *find_cache_aligned(size_t size, size_t align) {
    size = ALIGN_UP(size, align);

    for (size_t i = 0; i < KMALLOC_CACHE_COUNT; i++) {
        if (kmalloc_caches[i].size >= size && kmalloc_caches[i].size % align == 0)
            return &kmalloc_caches[i];
    }

    return NULL;
}

/* a slab with no owner and nothing in use goes back */
static void discard_slab(struct page *page) {
    page->slab.cache = NULL;
    free_pages((void *)PFN_TO_PHYS(page_to_pfn(page)), 0);
}

/* all of a new slab is handed to the cpu: frozen, everything in use. *chain is its freelist */
static struct page *new_slab(struct kmem_cache *cache, void **chain) {
    uintptr_t phys = (uintptr_t)alloc_pages(0);
    if (!phys) return NULL;

    struct page *page = pfn_to_page(PHYS_TO_PFN(phys));
    uintptr_t base = P2V(phys);

    for (unsigned i = 0; i < cache->objects; i++) {
        void *obj = (void *)(base + i * cache->size);

        *(void **)obj = i + 1 < cache->objects ? (void *)(base + (i + 1) * cache->size) : NULL;
    }

    list_init(&page->page_list);

    page->slab.cache   = cache;
    page->slab.on_list = false;
    page->slab.state   = st_make(OFF_NONE, cache->objects, true);

    *chain = (void *)base;
    return page;
}

/* the slab's freelist, all of it: what other cpus freed. NULL if there is none. what is taken is in use */
static void *take_freelist(struct page *page) {
    uint64_t old = st_load(page);

    do {
        if (st_head(old) == OFF_NONE) return NULL;
    } while (!st_cas(page, &old, st_make(OFF_NONE, page->slab.cache->objects, st_frozen(old))));

    return (void *)(slab_base(page) + st_head(old));
}

static void freeze(struct page *page) {
    uint64_t old = st_load(page);

    while (!st_cas(page, &old, st_make(st_head(old), st_inuse(old), true))) ;
}

/* the owner's slab has all in use: it is let go, unless a free got in. false: it did, take it */
static bool release_full(struct page *page) {
    uint64_t old = st_load(page);

    do {
        if (st_head(old) != OFF_NONE) return false;
    } while (!st_cas(page, &old, st_make(OFF_NONE, st_inuse(old), false)));

    return true;
}

/* a slab that was taken from a cpu and is not full: the cache's, or the buddy's if it is empty and there are enough */
static void unfreeze(struct kmem_cache *cache, struct page *page) {
    uint64_t old = st_load(page);

    while (!st_cas(page, &old, st_make(st_head(old), st_inuse(old), false))) ;

    /* full: on no list. whoever frees an object of it next takes it */
    if (st_head(old) == OFF_NONE) return;

    uint64_t flags = spin_lock_irqsave(&cache->lock);

    /* frees may have emptied it since. not on the list yet, so none of them could take it off */
    if (st_inuse(st_load(page)) == 0 && cache->nr_partial >= MIN_PARTIAL) {
        spin_unlock_irqrestore(&cache->lock, flags);
        discard_slab(page);
        return;
    }

    list_add_tail(&page->page_list, &cache->partial);
    page->slab.on_list = true;
    cache->nr_partial++;

    spin_unlock_irqrestore(&cache->lock, flags);
}

/* a free emptied a slab of the cache's list. it goes if the cache has more than it keeps */
static void maybe_discard(struct kmem_cache *cache, struct page *page) {
    uint64_t flags = spin_lock_irqsave(&cache->lock);

    /* an allocating cpu may have taken it meanwhile: it left the list then, under this lock */
    if (!page->slab.on_list || cache->nr_partial <= MIN_PARTIAL || st_inuse(st_load(page)) != 0) {
        spin_unlock_irqrestore(&cache->lock, flags);
        return;
    }

    list_del(&page->page_list);
    page->slab.on_list = false;
    cache->nr_partial--;

    spin_unlock_irqrestore(&cache->lock, flags);

    discard_slab(page);
}

/* irq off. a slab the cpu took in a free, or more than it keeps: the oldest go back to the cache */
static void put_cpu_partial(struct kmem_cache *cache, struct kmem_cpu *cpu, struct page *page) {
    list_add(&page->page_list, &cpu->partial);
    cpu->nr_partial++;

    while (cpu->nr_partial > CPU_PARTIAL_MAX) {
        struct page *old = container_of(cpu->partial.prev, struct page, page_list);

        list_del(&old->page_list);
        cpu->nr_partial--;

        unfreeze(cache, old);
    }
}

/* irq off. some slabs from the cache's list, frozen: the first is the active one, the others are for the partial list */
static bool get_partial_node(struct kmem_cache *cache, struct kmem_cpu *cpu) {
    struct page *first = NULL;

    uint64_t flags = spin_lock_irqsave(&cache->lock);

    for (unsigned n = 0; n < 1 + CPU_PARTIAL_FILL && !list_empty(&cache->partial); n++) {
        struct page *page = list_first(&cache->partial, struct page, page_list);

        list_del(&page->page_list);
        page->slab.on_list = false;
        cache->nr_partial--;

        freeze(page);

        if (!first) {
            first = page;
        } else {
            list_add(&page->page_list, &cpu->partial);
            cpu->nr_partial++;
        }
    }

    spin_unlock_irqrestore(&cache->lock, flags);

    if (!first) return false;

    cpu->page = first;
    return true;
}

/* irq off, the cpu's freelist is empty */
static void *alloc_slow(struct kmem_cache *cache, struct kmem_cpu *cpu) {
    for (;;) {
        struct page *page = cpu->page;

        if (page) {
            void *chain = take_freelist(page);

            if (chain) {
                cpu->freelist = *(void **)chain;
                return chain;
            }

            if (!release_full(page)) continue; /* a free came in: take that */

            cpu->page = NULL;
        }

        if (!list_empty(&cpu->partial)) {
            page = list_first(&cpu->partial, struct page, page_list);

            list_del(&page->page_list);
            cpu->nr_partial--;
            cpu->page = page;
            continue;
        }

        if (get_partial_node(cache, cpu)) continue;

        void *chain;

        page = new_slab(cache, &chain);
        if (!page) return NULL;

        cpu->page = page;
        cpu->freelist = *(void **)chain;
        return chain;
    }
}

void *slab_alloc(struct kmem_cache *cache) {
    uint64_t flags = irq_save();

    struct kmem_cpu *cpu = this_cpu_cache(cache);
    void *obj = cpu->freelist;

    if (obj) cpu->freelist = *(void **)obj;
    else     obj = alloc_slow(cache, cpu);

    irq_restore(flags);
    return obj;
}

/* irq off. an object of a slab that is not the cpu's active one: onto the slab's own freelist */
static void free_slow(struct kmem_cache *cache, struct kmem_cpu *cpu, struct page *page, void *obj) {
    uintptr_t base = slab_base(page);
    uintptr_t off = (uintptr_t)obj - base;

    if (off % cache->size) panic("slab: free of 0x%lx -- not at an object of a %lu byte cache", (uintptr_t)obj, cache->size);

    uint64_t old = st_load(page), new;
    bool was_full;

    for (;;) {
        uint32_t head = st_head(old);

        *(void **)obj = head == OFF_NONE ? NULL : (void *)(base + head);

        /* in use all and on no list: this free is the one that finds it a home */
        was_full = head == OFF_NONE && !st_frozen(old);

        new = st_make(off, st_inuse(old) - 1, st_frozen(old) || was_full);

        if (st_cas(page, &old, new)) break;
    }

    if (st_frozen(old)) return; /* its cpu takes it when it needs it */

    if (was_full) {
        put_cpu_partial(cache, cpu, page);
        return;
    }

    if (st_inuse(new) == 0) maybe_discard(cache, page);
}

void slab_free(void *obj) {
    struct page *page = obj_page(obj);
    struct kmem_cache *cache = page->slab.cache;

    if (!cache) panic("slab: free of 0x%lx -- not in a slab", (uintptr_t)obj);

    uint64_t flags = irq_save();

    struct kmem_cpu *cpu = this_cpu_cache(cache);

    if (page == cpu->page) {
        *(void **)obj = cpu->freelist;
        cpu->freelist = obj;
    } else {
        free_slow(cache, cpu, page, obj);
    }

    irq_restore(flags);
}

void *kmalloc(size_t size) {
    struct kmem_cache *cache = find_cache(size);

    if (!cache)
        return NULL;

    return slab_alloc(cache);
}

void *kmalloc_aligned(size_t align, size_t size) {
    if (!align || (align & (align - 1))) return NULL;

    struct kmem_cache *cache = find_cache_aligned(size, align);
    if (!cache) return NULL;

    return slab_alloc(cache);
}

void kfree(void *ptr) {
    if (!ptr) return;

    slab_free(ptr);
}

void slab_init() {
    for (int i = 0; i < KMALLOC_CACHE_COUNT; i++) {
        struct kmem_cache *cache = &kmalloc_caches[i];

        cache->objects = PAGE_SIZE / cache->size;
        cache->index   = i;

        spin_lock_init(&cache->lock);
        list_init(&cache->partial);

        for (int c = 0; c < MAX_CPUS; c++) list_init(&cpu_slabs[c].c[i].partial);
    }
}
