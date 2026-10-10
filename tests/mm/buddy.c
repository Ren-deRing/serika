#include <serika/buddy.h>
#include <serika/page.h>
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

/* buddy's first page is free, but only as an order-0 block. must not merge */
bool test_buddy_partial_buddy() {
    uintptr_t y = (uintptr_t)alloc_pages(2);
    TEST_ASSERT(y != 0);

    uintptr_t y2 = y + 2 * PAGE_SIZE;
    uintptr_t y3 = y + 3 * PAGE_SIZE;

    free_pages((void *)y2, 0); /* y+2 is free, y+3 stays in use */
    free_pages((void *)y, 1);  /* its buddy is y+2, free but only 1 page of 2 */

    struct page *head  = pfn_to_page(PHYS_TO_PFN(y));
    struct page *other = pfn_to_page(PHYS_TO_PFN(y2));

    TEST_ASSERT(head->is_free && head->order == 1);   /* this is order 2 */
    TEST_ASSERT(other->is_free && other->order == 0);

    free_pages((void *)y3, 0); /* everything merges back */

    return true;
}
