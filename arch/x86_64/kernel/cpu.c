#include <serika/cpu.h>
#include <serika/panic.h>

#include <asm/cpu.h>
#include <asm/arch.h>

struct cpu cpus[MAX_CPUS];
struct arch_cpu arch_cpus[MAX_CPUS];

static inline uint32_t get_apic_id(void) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (ebx >> 24) & 0xFF; /* APIC ID here */
}

void init_cpu_gs(struct cpu* cpu_ptr) {
    uint64_t addr = (uint64_t)cpu_ptr;
    /* IA32_GS_BASE MSR = 0xC0000101 */
    asm volatile ("wrmsr" : : "c"(0xC0000101), "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)): "memory"); // gs base
    asm volatile ("wrmsr" : : "c"(0xC0000102), "a"((uint32_t)addr), "d"((uint32_t)(addr >> 32)): "memory"); // kernel gs base
}

struct cpu* get_this_core(void) {
    struct cpu* ptr;
    /* struct cpu located at gs[0] */
    asm volatile ("movq %%gs:0, %0" : "=r"(ptr));
    return ptr;
}

static inline int is_intel(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    // ebx == "Genu", edx == "ineI", ecx == "ntel"
    return (ebx == 0x756e6547 && edx == 0x49656e69 && ecx == 0x6c65746e);
}

void tsc_init(uint64_t* tsc_freq_hz) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

    /* hypervisor */
    cpuid(0x40000010, 0, &eax, &ebx, &ecx, &edx);
    if (eax != 0) {
        /* eax: tsc freq (kHz) */
        *tsc_freq_hz = (uint64_t)eax * 1000ULL;
        return;
    }

    /* tsc / clock info */
    cpuid(0x15, 0, &eax, &ebx, &ecx, &edx);
    if (eax != 0 && ebx != 0 && ecx != 0) {
        *tsc_freq_hz = ((uint64_t)ecx * ebx) / eax;
        return;
    }

    if (is_intel()) {
        /* intel base freq */
        cpuid(0x16, 0, &eax, &ebx, &ecx, &edx);
        if (eax != 0) {
            *tsc_freq_hz = (uint64_t)eax * 1000000ULL;
            return;
        }

        /* intel platform info */
        uint64_t platform_info = rdmsr(0xCE);
        uint64_t ratio = (platform_info >> 8) & 0xFF;
        if (ratio != 0) {
            *tsc_freq_hz = ratio * 100000000ULL; /* base clock 100MHz */
            return;
        }
    }
    else {
        uint64_t pstate0 = rdmsr(0xC0010064);
        if (pstate0 & (1ULL << 63)) { /* P-state valid */
            uint64_t cpu_fid = (pstate0 >> 8) & 0x3F;
            uint64_t cpu_did = pstate0 & 0x3F;
            if (cpu_did != 0) {
                /* clock = (200 * cpuFid) / cpuDid (MHz) */
                uint64_t freq_mhz = (200 * cpu_fid) / cpu_did;
                *tsc_freq_hz = freq_mhz * 1000000ULL;
                return;
            }
        }
    }

    panic("failed to determine TSC frequency"); // what is this?
}

volatile uint32_t logic_id = 0;

void cpu_init() {
    uint32_t hw_id = get_apic_id();
    uint32_t my_id = __atomic_fetch_add(&logic_id, 1, 5); /* __ATOMIC_SEQ_CST */
    /*  
        this code is run by multiple processors simultaneously.
        that means, logic_id could be corrupted. so atomic is used.
    */

    struct cpu *c = &cpus[my_id];

    c->self  = c;
    c->id    = my_id;
    c->hw_id = hw_id;

    c->arch_cpu_data = &arch_cpus[my_id];

    init_cpu_gs(c);
    tsc_init(&c->tsc_freq_hz);

    extern void gdt_init();
    extern void idt_init();
    gdt_init();
    idt_init();
}
