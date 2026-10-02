#include <serika/buddy.h>
#include <tests/test.h>
#include <stdbool.h>

bool test_buddy_merge() {
    void *a = alloc_pages(4);
    void *b = alloc_pages(4);

    TEST_ASSERT(a != NULL);
    TEST_ASSERT(b != NULL);

    free_pages(a, 4);

    void *merged = alloc_pages(5);
    TEST_ASSERT(merged != NULL);

    free_pages(b, 4);

    void *parent = alloc_pages(5);
    TEST_ASSERT(parent != NULL);

    free_pages(merged, 5);
    free_pages(parent, 5);

    return true;
}
