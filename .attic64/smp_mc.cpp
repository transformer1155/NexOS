// Multicore SMP bring-up for the 64-bit kernel (NexOS .attic64).
// Wakes APs via the standard xAPIC INIT -> SIPI -> SIPI sequence.
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
    uint32_t svr=smp_lapic_read(0xF0); svr|=(1u<<8); svr=(svr&0xFF)|0xFF; smp_lapic_write(0xF0,svr); smp_lapic_write(0x80,0);
}
static void smp_io_delay(uint32_t iters){ while(iters--) __asm__ __volatile__("pause":::"memory"); }
static uint32_t smp_lapic_ipi(uint8_t target,uint32_t delivery,uint8_t vector){
    smp_lapic_write(0x310,((uint32_t)target<<24));
    smp_lapic_write(0x300,delivery|(uint32_t)vector);
    uint32_t t=0; while(smp_lapic_read(0x300)&(1u<<12)){ if(++t>2000000) break; smp_io_delay(10); }
    return smp_lapic_read(0x300);
}
static void wake_ap(uint8_t apic,uint8_t vec){
    uint32_t r;
    r=smp_lapic_ipi(apic,0x00000500,0); smp_io_delay(300000); // INIT
    uint32_t esr0=smp_lapic_read(0x370);
    r=smp_lapic_ipi(apic,0x00000600,vec); smp_io_delay(300000); // SIPI
    uint32_t esr1=smp_lapic_read(0x370);
    serial_puts("[SMP] apic "); serial_dec((uint32_t)apic);
    serial_puts(" INITICR="); serial_hex(r);
    serial_puts(" ESR0="); serial_hex(esr0);
    serial_puts(" ESR1="); serial_hex(esr1);
    serial_puts("\n");
}

extern uint8_t _binary_build_ap_trampoline_bin_start[];
extern uint8_t _binary_build_ap_trampoline_bin_end[];

cpu_info_t g_cpu[MAX_CPUS];
int g_ncpus=1;
volatile int g_aps_done=0;
int g_my_cpu=0;

extern "C" void ap_main(int idx){
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
    smp_lapic_init();
    uint32_t bsp_id=smp_lapic_id();
    serial_puts("[SMP] BSP apic_id="); serial_dec(bsp_id); serial_puts("\n");

    uint8_t* dst=(uint8_t*)(uintptr_t)TRAMP_PHYS;
    uint8_t* src=_binary_build_ap_trampoline_bin_start;
    uint8_t* end=_binary_build_ap_trampoline_bin_end;
    uint32_t sz=(uint32_t)(end-src);
    for(uint32_t i=0;i<sz;i++) dst[i]=src[i];

    uint64_t cr3_val;
    __asm__ __volatile__("movq %%cr3,%0":"=r"(cr3_val));
    *(uint32_t*)&dst[2]=(uint32_t)cr3_val;
    *(uint64_t*)&dst[10]=(uint64_t)(uintptr_t)&ap_main;

    uint8_t tramp_vec=(uint8_t)((TRAMP_PHYS>>12)&0xFF);
    serial_puts("[SMP] trampoline vector=0x");
    { const char*h="0123456789ABCDEF"; char hb[3]; hb[0]=h[(tramp_vec>>4)&0xF]; hb[1]=h[tramp_vec&0xF]; hb[2]=0; serial_puts(hb); }
    serial_puts("\n");

    for(int ap=0;ap<MAX_CPUS;ap++){
        if((uint32_t)ap==bsp_id) continue;
        *(uint32_t*)&dst[6]=(uint32_t)ap;
        wake_ap((uint8_t)ap,tramp_vec);
    }

    uint32_t wait=0;
    while(wait<300000){ smp_io_delay(1000); wait++; if((wait&0xFFFF)==0){ serial_puts("[SMP] waiting aps_done="); serial_dec((uint32_t)g_aps_done); serial_puts("\n"); } }
    g_ncpus=1+(int)g_aps_done;
    serial_puts("[SMP] online cpus="); serial_dec((uint32_t)g_ncpus); serial_puts("\n");
}
