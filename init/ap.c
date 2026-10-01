#include <serika/arch.h>
#include <serika/boot.h>

void start_ap(struct core* info) {
    (void)info;
    for (;;) arch_halt();
}
