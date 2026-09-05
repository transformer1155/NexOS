#!/bin/bash
set -e
cd /mnt/d/MyOS/bootloader
# NOTE: GGUF model loading + AUTOTEST must run on BSP only. Waking APs under
# QEMU TCG hangs smp_init forever, blocking the demo. We emit a single-core
# (BSP-only) smp_init on purpose.
cat > .attic64/smp_bringup.cpp <<'NEXEOF'
#include "smp64.h"
#include <stddef.h>
#include <stdint.h>

extern "C" void serial_puts(const char* s);

static void s_puts(const char* s){ serial_puts(s); }

cpu_info_t g_cpu[MAX_CPUS];
int        g_ncpus = 1;
volatile int g_aps_done = 0;
static int g_my_cpu = 0;

void smp_init(void){
    g_ncpus = 1;
    g_cpu[0].present = 1;
    g_cpu[0].apic_id = 0;
    g_my_cpu = 0;
    s_puts("[SMP] init (single-core BSP only)\n");
    s_puts("[SMP] online cpus=1 (BSP)\n");
}

int cpu_index(void){ return g_my_cpu; }
NEXEOF
echo "WROTE .attic64/smp_bringup.cpp (single-core)"
