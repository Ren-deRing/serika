#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/pmap.h>
#include <stdint.h>
#include <tests/test.h>

#define MM_TEST_BASE 0x400000ULL

bool test_mm_fault() {
    struct mm *mm = mm_create();
    TEST_ASSERT(mm != NULL);

    TEST_ASSERT(mm_map(mm, MM_TEST_BASE, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE) == 0);
    TEST_ASSERT(mm_map(mm, MM_TEST_BASE + 4 * PAGE_SIZE, PAGE_SIZE, PROT_READ) == 0);

    /* bad ranges */
    TEST_ASSERT(mm_map(mm, MM_TEST_BASE + PAGE_SIZE, PAGE_SIZE, PROT_READ) == -EEXIST);
    TEST_ASSERT(mm_map(mm, MM_TEST_BASE - PAGE_SIZE, 2 * PAGE_SIZE, PROT_READ) == -EEXIST);
    TEST_ASSERT(mm_map(mm, MM_TEST_BASE + 0x10, PAGE_SIZE, PROT_READ) == -EINVAL);
    TEST_ASSERT(mm_map(mm, USER_SPACE_END - PAGE_SIZE, 2 * PAGE_SIZE, PROT_READ) == -EINVAL);

    /* nothing until the first fault */
    TEST_ASSERT(pmap_extract(mm->pmap, MM_TEST_BASE) == 0);

    TEST_ASSERT(mm_fault(mm, MM_TEST_BASE + PAGE_SIZE + 0x123, PROT_WRITE) == 0);
    TEST_ASSERT(pmap_extract(mm->pmap, MM_TEST_BASE + PAGE_SIZE) != 0);
    TEST_ASSERT(pmap_extract(mm->pmap, MM_TEST_BASE) == 0);
    TEST_ASSERT(pmap_extract(mm->pmap, MM_TEST_BASE + 2 * PAGE_SIZE) == 0);

    /* faulting a present page again keeps it */
    uintptr_t phys = pmap_extract(mm->pmap, MM_TEST_BASE + PAGE_SIZE);
    TEST_ASSERT(mm_fault(mm, MM_TEST_BASE + PAGE_SIZE, PROT_READ) == 0);
    TEST_ASSERT(pmap_extract(mm->pmap, MM_TEST_BASE + PAGE_SIZE) == phys);

    /* outside any vma, or not allowed by it */
    TEST_ASSERT(mm_fault(mm, MM_TEST_BASE - 1, PROT_READ) == -EFAULT);
    TEST_ASSERT(mm_fault(mm, MM_TEST_BASE + 5 * PAGE_SIZE, PROT_READ) == -EFAULT);
    TEST_ASSERT(mm_fault(mm, MM_TEST_BASE + 4 * PAGE_SIZE, PROT_WRITE) == -EFAULT);
    TEST_ASSERT(mm_fault(mm, MM_TEST_BASE, PROT_EXEC) == -EFAULT);

    /* copy across a page boundary, into a read-only vma too */
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;

    uintptr_t at = MM_TEST_BASE + 4 * PAGE_SIZE - 32;
    TEST_ASSERT(mm_copy_to(mm, at, buf, sizeof(buf)) == 0);

    uint8_t *lo = (uint8_t *)P2V(pmap_extract(mm->pmap, at));
    uint8_t *hi = (uint8_t *)P2V(pmap_extract(mm->pmap, at + 32));
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT(lo[i] == i);
        TEST_ASSERT(hi[i] == 32 + i);
    }

    TEST_ASSERT(mm_copy_to(mm, MM_TEST_BASE + 5 * PAGE_SIZE, buf, 1) == -EFAULT);

    mm_put(mm);
    return true;
}
