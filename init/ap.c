#include <serika/arch.h>
#include <serika/boot.h>

void start_ap(coreinfo_t* info) {
    for (;;) arch_halt();
}
