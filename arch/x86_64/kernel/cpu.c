#include <serika/cpu.h>
#include <serika/intc.h>
#include <serika/panic.h>

#include <asm/cpu.h>
#include <asm/arch.h>
#include <serika/printk.h>

struct cpu cpus[MAX_CPUS];
struct arch_cpu arch_cpus[MAX_CPUS];

static inline uint32_t get_apic_id() {
    uint32_t eax, ebx, ecx, edx;
    asm volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return (ebx >> 24) & 0xFF; /* APIC ID here */
}

void init_cpu_gs(struct cpu *cpu_ptr) {
    uint64_t addr = (uint64_t)cpu_ptr;

    wrmsr(0xC0000101, addr); /* IA32_GS_BASE */
    wrmsr(0xC0000102, addr); /* IA32_KERNEL_GS_BASE */
}

int enable_xsaves() {
    uint32_t eax, ebx, ecx, edx;

    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1 << 26))) return -1; /* XSAVE */

    cpuid(0xD, 0, &eax, &ebx, &ecx, &edx);
    if (!(eax & (1 << 0))) return -1; /* x87 */
    if (!(eax & (1 << 1))) return -1; /* SSE */
    if (!(eax & (1 << 2))) return -1; /* AVX */

    cpuid(0xD, 1, &eax, &ebx, &ecx, &edx);
    if (!(eax & (1 << 3))) return -1; /* XSAVES/XRSTORS */

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 18) | (1ULL << 9);
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    asm volatile("xsetbv" :: "a"(0x7), "d"(0), "c"(0));
    wrmsr(0xDA0, 0);

    curcpu->arch_cpu_data->xsave_size = ebx;

    return 0;
}

int enable_fsgsbase() {
    uint32_t eax, ebx, ecx, edx;

    cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    if (!(ebx & (1 << 0))) return -1;
    
    uint64_t cr4;
    
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 16);
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    return 0;
}

struct cpu* get_this_core() {
    struct cpu* ptr;
    /* struct cpu located at gs[0] */
    asm volatile ("movq %%gs:0, %0" : "=r"(ptr));
    return ptr;
}

static inline int is_intel() {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    /* ebx == "Genu", edx == "ineI", ecx == "ntel" */
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

    panic("cpu: unable to determine TSC frequency -- what kind of CPU is this?"); // what is this?
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
    extern void syscall_init();
    gdt_init();
    idt_init();
    syscall_init();

    lapic_init();

    if(enable_fsgsbase() == -1) panic("cpu: unable to enable FSGSBASE -- does your cpu support FSGSBASE?");
    if(enable_xsaves() == -1)   panic("cpu: unable to enable XSAVES -- does your cpu support XSAVES?");
}
