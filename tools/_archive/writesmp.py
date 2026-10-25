p = ".attic64/smp64.cpp"
content = """// smp64.cpp - SMP bring-up for 64-bit NexOS (Stage 8.1)
// SMP/AP bring-up disabled (single BSP) for a self-contained kernel link.
#include "smp64.h"

cpu_info_t g_cpu[MAX_CPUS];
int        g_ncpus = 1;
volatile int g_aps_done = 0;
static int g_my_cpu = 0;

extern "C" void ap_main(int idx){ (void)idx; for(;;){ __asm__ __volatile__("hlt":::"memory"); } }

void smp_init(void){
    serial_puts("[SMP] init (single-core BSP only)\\n");
    g_ncpus = 1;
    g_cpu[0].present = 1;
    g_cpu[0].apic_id = 0;
    g_my_cpu = 0;
    serial_puts("[SMP] online cpus=1 (BSP)\\n");
}

int cpu_index(void){ return g_my_cpu; }
"""
open(p, "w", encoding="utf-8").write(content)
t = open(p, encoding="utf-8").read()
print("written, contains 'single-core':", "single-core" in t)
print("contains 'waking APs':", "waking APs" in t)
