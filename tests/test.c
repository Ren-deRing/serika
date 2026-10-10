#include <serika/printk.h>
#include <tests/test.h>

#include <stddef.h>

typedef bool (*test_fn_t)(void);

struct test {
    const char *name;
    test_fn_t fn;
};

static struct test tests[] = {
    { "buddy merge", test_buddy_merge },
    { "buddy partial", test_buddy_partial_buddy },
    { "slab reuse",  test_slab_reuse },
    { "slab multi",  test_slab_multi },
    { "slab stress", test_slab_stress },
    { "slab state",  test_slab_state },
    { "tick rate",   test_tick_rate },
    { "sched fair",  test_sched_fair },
    { "sched nice",  test_sched_nice },
    { "smp online",  test_smp_online },
    { "smp spread",  test_smp_spread },
    { "smp balance", test_smp_balance },
};

void test_run_all() {
    size_t passed = 0;
    size_t total = sizeof(tests) / sizeof(tests[0]);

    printk("[TEST] running %zu tests\n", total);

    for (size_t i = 0; i < total; i++) {
        bool result = tests[i].fn();

        printk("[TEST] %-24s %s\n", tests[i].name, result ? "PASS" : "FAIL");

        if (result) passed++;
    }

    printk("[TEST] %zu/%zu passed\n", passed, total);
}
