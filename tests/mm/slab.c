#include <serika/page.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <stdint.h>
#include <tests/test.h>

#define STRESS_SLOTS 256
#define STRESS_ITERS 100000

#define STATE_TEST_SIZE 15
#define STATE_TEST_OBJECTS (PAGE_SIZE / 16)

bool test_slab_reuse() {
    void *a = kmalloc(24);
    TEST_ASSERT(a != NULL);

    kfree(a);

    void *b = kmalloc(17);
    TEST_ASSERT(b == a);

    kfree(b);
    return true;
}

bool test_slab_multi() {
    void *a = kmalloc(24);
    void *b = kmalloc(24);
    void *c = kmalloc(24);

    TEST_ASSERT(a != NULL);
    TEST_ASSERT(b != NULL);
    TEST_ASSERT(c != NULL);

    TEST_ASSERT(a != b);
    TEST_ASSERT(a != c);
    TEST_ASSERT(b != c);

    kfree(a);
    kfree(b);
    kfree(c);

    return true;
}

bool test_slab_stress() {
    void *slots[STRESS_SLOTS] = { 0 };
    size_t sizes[STRESS_SLOTS] = { 0 };

    for (size_t i = 0; i < STRESS_ITERS; i++) {
        size_t idx = rand_u32() % STRESS_SLOTS;

        if (slots[idx]) {
            uint8_t *ptr = slots[idx];

            for (size_t j = 0; j < sizes[idx]; j++)
                TEST_ASSERT(ptr[j] == 0xA5);

            kfree(slots[idx]);
            slots[idx] = NULL;
        } else {
            size_t size = (rand_u32() % 4096) + 1;
            void *ptr = kmalloc(size);

            TEST_ASSERT(ptr != NULL);

            memset(ptr, 0xA5, size);

            slots[idx] = ptr;
            sizes[idx] = size;
        }
    }

    for (size_t i = 0; i < STRESS_SLOTS; i++) {
        if (slots[i])
            kfree(slots[i]);
    }

    return true;
}

bool test_slab_state() {
    void *objects[STATE_TEST_OBJECTS];

    /* empty -> partial */
    objects[0] = kmalloc(STATE_TEST_SIZE);
    TEST_ASSERT(objects[0] != NULL);

    /* partial -> full */
    for (size_t i = 1; i < STATE_TEST_OBJECTS; i++) {
        objects[i] = kmalloc(STATE_TEST_SIZE);
        TEST_ASSERT(objects[i] != NULL);
    }

    /* full -> partial */
    kfree(objects[0]);

    /* partial -> empty */
    for (size_t i = 1; i < STATE_TEST_OBJECTS; i++)
        kfree(objects[i]);

    /* empty -> partial again */
    void *reuse = kmalloc(STATE_TEST_SIZE);
    TEST_ASSERT(reuse != NULL);

    kfree(reuse);

    return true;
}
