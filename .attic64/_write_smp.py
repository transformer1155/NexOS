import io
path = r"d:\MyOS\bootloader\.attic64\smp64.cpp"
content = r'''// smp64.cpp - SMP bring-up for 64-bit NexOS (Stage 8.1)
#include "smp64.h"

static inline void outb(uint16_t port, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(port)); }

extern "C" uint8_t _binary_build_ap_trampoline_bin_start[];
extern "C" uint8_t _binary_build_ap_trampoline_bin_end[];

cpu_info_t g_cpu[MAX_CPUS];
int        g_ncpus = 1;
volatile int g_aps_done = 0;
static int g_my_cpu = 0;

static volatile uint32_t* g_lapic = nullptr;

#define LAPIC_REG_ID   0x20
#define LAPIC_REG_SVR  0xF0
#define LAPIC_REG_TPR  0x80
#define LAPIC_REG_ICR_LO 0x300
#define LAPIC_REG_ICR_HI 0x310
#define LAPIC_DEFAULT_PHYS 0xFEE00000ull
#define TRAMP_PHYS 0x7000u

static inline uint32_t rdmsr32(uint32_t msr, uint32_t* hi){
    uint32_t lo;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(*hi) : "c"(msr));
    return lo;
}
uint32_t lapic_read(uint32_t reg){ return g_lapic[reg/4]; }
void lapic_write(uint32_t reg, uint32_t val){ g_lapic[reg/4] = val; }
uint32_t lapic_id(void){ return lapic_read(LAPIC_REG_ID) >> 24; }

void lapic_init(void){
    uint32_t hi = 0;
    uint32_t lo = rdmsr32(0x1B, &hi);
    uint64_t base = ((uint64_t)lo & 0xFFFFF000ull) | ((uint64_t)hi << 32);
    if (base == 0) base = LAPIC_DEFAULT_PHYS;
    g_lapic = (volatile uint32_t*)(uintptr_t)base;
    lo |= (1u << 11);
    __asm__ __volatile__("wrmsr" :: "c"(0x1B), "a"(lo), "d"(hi));
    uint32_t svr = lapic_read(LAPIC_REG_SVR);
    svr |= (1u << 8);
    svr = (svr & 0xFF) | 0xFF;
    lapic_write(LAPIC_REG_SVR, svr);
    lapic_write(LAPIC_REG_TPR, 0);
}

static void io_delay(uint32_t iters){
    while (iters--) __asm__ __volatile__("pause" ::: "memory");
}

static void lapic_ipi(uint8_t target, uint32_t delivery, uint8_t vector){
    lapic_write(LAPIC_REG_ICR_HI, ((uint32_t)target << 24));
    lapic_write(LAPIC_REG_ICR_LO, delivery | (uint32_t)vector);
    uint32_t t = 0;
    while (lapic_read(LAPIC_REG_ICR_LO) & (1u << 12)) {
        if (++t > 1000000) break;
        io_delay(10);
    }
}

extern "C" void ap_main(int idx){
    if (idx >= 0 && idx < MAX_CPUS) g_cpu[idx].present = 1;
    serial_puts("CPU");
    serial_dec((uint32_t)idx);
    serial_puts(" online (apic_id=");
    serial_dec(lapic_id());
    serial_puts(")\n");
    atomic_inc((volatile int*)&g_aps_done);
    for (;;) { __asm__ __volatile__("hlt" ::: "memory"); }
}

void smp_init(void){
    serial_puts("[SMP] init\n");
    lapic_init();
    uint32_t bsp_id = lapic_id();
    serial_puts("[SMP] BSP apic_id=");
    serial_dec(bsp_id);
    serial_puts("\n");

    g_ncpus = 1;
    g_cpu[0].present = 1;
    g_cpu[0].apic_id = bsp_id;
    g_my_cpu = 0;

    uint8_t* dst = (uint8_t*)(uintptr_t)TRAMP_PHYS;
    uint8_t* src = _binary_build_ap_trampoline_bin_start;
    uint8_t* end = _binary_build_ap_trampoline_bin_end;
    uint32_t sz = (uint32_t)(end - src);
    for (uint32_t i = 0; i < sz; i++) dst[i] = src[i];

    uint32_t* p_cr3 = (uint32_t*)(TRAMP_PHYS + 2);
    uint32_t* p_cpu = (uint32_t*)(TRAMP_PHYS + 6);
    uint64_t* p_ap  = (uint64_t*)(TRAMP_PHYS + 10);
    *p_ap = (uint64_t)(uintptr_t)&ap_main;

    uint64_t cr3_val;
    __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3_val));
    *p_cr3 = (uint32_t)cr3_val;

    int ap_count = 0;
    serial_puts("[SMP] waking APs (bsp=");
    serial_dec(bsp_id);
    serial_puts(")\n");
    for (int ap = 0; ap < MAX_CPUS; ap++){
        if ((uint32_t)ap == bsp_id) continue;
        *p_cpu = (uint32_t)ap;
        lapic_ipi((uint8_t)ap, 0x00001500, 0);
        io_delay(200000);
        lapic_ipi((uint8_t)ap, 0x00001600, (TRAMP_PHYS >> 12) & 0xFF);
        io_delay(200000);
        lapic_ipi((uint8_t)ap, 0x00001600, (TRAMP_PHYS >> 12) & 0xFF);
        io_delay(200000);
        ap_count++;
        serial_puts("[SMP] sent INIT+SIPI to apic ");
        serial_dec((uint32_t)ap);
        serial_puts(" (aps_done=");
        serial_dec((uint32_t)g_aps_done);
        serial_puts(")\n");
    }

    uint32_t wait = 0;
    while ((int)g_aps_done < ap_count && wait < 2000000){
        io_delay(1000);
        wait++;
    }
    g_ncpus = 1 + (int)g_aps_done;
    serial_puts("[SMP] online cpus=");
    serial_dec((uint32_t)g_ncpus);
    serial_puts(" (BSP + ");
    serial_dec((uint32_t)g_aps_done);
    serial_puts(" APs)\n");
}

int cpu_index(void){ return g_my_cpu; }
'''
with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(content)
print("wrote", len(content), "bytes to", path)
