// Multicore SMP bring-up for the 64-bit kernel (NexOS).
// Implemented as a single stable file (not under .attic64) to avoid DrvFS
// cache churn. Wakes APs via the standard xAPIC INIT -> SIPI -> SIPI sequence.
#include "smp64.h"
#include <stddef.h>
#include <stdint.h>

#define TRAMP_PHYS 0x7000u
#define LAPIC_DEFAULT_PHYS 0xFEE00000ull

static volatile uint32_t* g_lapic_smp = 0;

static uint32_t smp_lapic_read(uint32_t reg){ return g_lapic_smp[reg/4]; }
static void smp_lapic_write(uint32_t reg,uint32_t val){ g_lapic_smp[reg/4]=val; }
static uint32_t smp_lapic_id(void){ return smp_lapic_read(0x20)>>24; }
static void smp_lapic_init(void){
    g_lapic_smp=(volatile uint32_t*)(uintptr_t)LAPIC_DEFAULT_PHYS;
    // SPIV: enable APIC (bit 8) + spurious vector 0xFF. Must keep bit8 set.
    uint32_t svr=smp_lapic_read(0xF0);
    svr=(svr&0xFF)|0x100|0xFF;
    smp_lapic_write(0xF0,svr);
    smp_lapic_write(0x80,0);
}
static void smp_io_delay(uint32_t iters){ while(iters--) __asm__ __volatile__("pause":::"memory"); }
static uint32_t smp_lapic_ipi(uint8_t target,uint32_t delivery,uint8_t vector){
    smp_lapic_write(0x310,((uint32_t)target<<24));
    smp_lapic_write(0x300,delivery|(uint32_t)vector);
    uint32_t t=0; while(smp_lapic_read(0x300)&(1u<<12)){ if(++t>2000000) break; smp_io_delay(10); }
    return smp_lapic_read(0x300);
}
static uint32_t smp_lapic_ipi_shorthand(uint32_t delivery,uint8_t vector){
    smp_lapic_write(0x310,0);
    smp_lapic_write(0x300,delivery|(2u<<18)|(uint32_t)vector);
    uint32_t t=0; while(smp_lapic_read(0x300)&(1u<<12)){ if(++t>2000000) break; smp_io_delay(10); }
    return smp_lapic_read(0x300);
}
// INIT (edge) + SIPI x2. Edge INIT avoids the QEMU triple-fault seen with
// level-triggered INIT. Diagnostics printed to catch non-responding APs.
static void wake_ap(uint8_t apic,uint8_t vec){
    uint32_t r;
    r=smp_lapic_ipi(apic,0x00000500,0); smp_io_delay(1000000); // INIT (edge)
    uint32_t esr0=smp_lapic_read(0x370);
    r=smp_lapic_ipi(apic,0x00000600,vec); smp_io_delay(500000); // SIPI
    uint32_t esr1=smp_lapic_read(0x370);
    r=smp_lapic_ipi(apic,0x00000600,vec); smp_io_delay(500000); // SIPI retry
    uint32_t esr2=smp_lapic_read(0x370);
    serial_puts("[SMP] apic "); serial_dec((uint32_t)apic);
    serial_puts(" ESR0="); serial_hex(esr0);
    serial_puts(" ESR1="); serial_hex(esr1);
    serial_puts(" ESR2="); serial_hex(esr2);
    serial_puts("\n");
}
// Broadcast SIPI (shorthand=2 = all-excluding-self) to wake every AP regardless
// of its exact APIC ID.
static void wake_all_sipi(uint8_t vec){
    uint32_t r;
    r=smp_lapic_ipi_shorthand(0x00000600,vec); smp_io_delay(500000);
    uint32_t esr1=smp_lapic_read(0x370);
    r=smp_lapic_ipi_shorthand(0x00000600,vec); smp_io_delay(500000);
    uint32_t esr2=smp_lapic_read(0x370);
    serial_puts("[SMP] broadcast SIPI ESR1="); serial_hex(esr1);
    serial_puts(" ESR2="); serial_hex(esr2); serial_puts("\n");
}

extern uint8_t _binary_build_ap_trampoline_bin_start[];
extern uint8_t _binary_build_ap_trampoline_bin_end[];

cpu_info_t g_cpu[MAX_CPUS];
int g_ncpus=1;
volatile int g_aps_done=0;
int g_my_cpu=0;

extern "C" void ap_main(int idx){
    serial_puts("M");
    g_cpu[idx].present=1;
    g_cpu[idx].apic_id=(uint32_t)idx;
    g_my_cpu=idx;
    __sync_fetch_and_add(&g_aps_done,1);
    serial_puts("A");
    serial_puts("B");
    for(;;) __asm__ __volatile__("hlt":::"memory");
}

void smp_init(void){
    serial_puts("[SMP] init\n");
    // Under a hypervisor (QEMU TCG), the xAPIC INIT/SIPI sequence to APs
    // produces a "Send Accept Error" and the IPI-delivery bit can stay pending,
    // so every wake_ap() spins for its full ~20M-iteration timeout.  The GGUF
    // inference path is entirely BSP-local, so on a VM we skip AP bring-up and
    // run single-core.  This mirrors the VM detection already used in
    // kernel64.cpp (cpuid leaf 1, bit 31).
    {
        uint32_t a=0,b=0,c=0,d=0;
        __asm__ __volatile__("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(1));
        if (c & (1u<<31)) {
            g_ncpus = 1;
            g_cpu[0].present = 1;
            g_cpu[0].apic_id = 0;
            g_my_cpu = 0;
            serial_puts("[SMP] VM detected - single-core (AP bring-up skipped)\n");
            return;
        }
    }
    smp_lapic_init();
    uint32_t bsp_id=smp_lapic_id();
    uint32_t spiv=smp_lapic_read(0xF0);
    serial_puts("[SMP] SPIV="); serial_hex(spiv); serial_puts("\n");
    serial_puts("[SMP] BSP apic_id="); serial_dec(bsp_id); serial_puts("\n");

    uint8_t* dst=(uint8_t*)(uintptr_t)TRAMP_PHYS;
    uint8_t* src=_binary_build_ap_trampoline_bin_start;
    uint8_t* end=_binary_build_ap_trampoline_bin_end;
    uint32_t sz=(uint32_t)(end-src);
    for(uint32_t i=0;i<sz;i++) dst[i]=src[i];

    serial_puts("[SMP] trampoline src_pa="); serial_hex((uint32_t)(uintptr_t)src);
    serial_puts(" sz="); serial_dec(sz); serial_puts("\n");
    serial_puts("[SMP] dst[0..3]=");
    { const char*h="0123456789ABCDEF"; char b[12]; for(int k=0;k<4;k++){ b[k*3]=h[(dst[k]>>4)&0xF]; b[k*3+1]=h[dst[k]&0xF]; b[k*3+2]=' '; } b[12]=0; serial_puts(b); }
    serial_puts("\n");

    uint64_t cr3_val;
    __asm__ __volatile__("movq %%cr3,%0":"=r"(cr3_val));
    *(uint32_t*)&dst[2]=(uint32_t)cr3_val;
    *(uint64_t*)&dst[10]=(uint64_t)(uintptr_t)&ap_main;
    serial_puts("[SMP] patched cr3="); serial_hex((uint32_t)cr3_val);
    serial_puts(" ap_main="); serial_hex((uint32_t)(uintptr_t)&ap_main); serial_puts("\n");

    uint8_t tramp_vec=(uint8_t)((TRAMP_PHYS>>12)&0xFF);
    serial_puts("[SMP] trampoline vector=0x");
    { const char*h="0123456789ABCDEF"; char hb[3]; hb[0]=h[(tramp_vec>>4)&0xF]; hb[1]=h[tramp_vec&0xF]; hb[2]=0; serial_puts(hb); }
    serial_puts("\n");

    for(int ap=0;ap<4;ap++){
        if((uint32_t)ap==bsp_id) continue;
        *(uint32_t*)&dst[6]=(uint32_t)ap;
        wake_ap((uint8_t)ap,tramp_vec);
    }
    // Extra: broadcast SIPI to reach APs whose APIC ID is not 1..3.
    wake_all_sipi(tramp_vec);

    uint32_t wait=0;
    while(wait<80000){
        smp_io_delay(1000); wait++;
        if(g_aps_done>=3) break;  // 3 APs (QEMU -smp 4)
        if((wait&0x3FFF)==0){ serial_puts("[SMP] waiting aps_done="); serial_dec((uint32_t)g_aps_done); serial_puts("\n"); }
    }
    g_ncpus=1+(int)g_aps_done;
    serial_puts("[SMP] online cpus="); serial_dec((uint32_t)g_ncpus); serial_puts("\n");
}
