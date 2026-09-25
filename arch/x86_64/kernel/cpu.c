#include <serika/cpu.h>
#include <asm/cpu.h>

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

    extern void gdt_init();
    extern void idt_init();
    gdt_init();
    idt_init();
}
