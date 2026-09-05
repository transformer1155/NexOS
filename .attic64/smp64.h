// =====================================================================
//  smp64.h  -  Minimal SMP (multiprocessor) support for the 64-bit
//              NexOS kernel.  Provides:
//                * LAPIC detection / init
//                * AP bring-up via INIT-SIPI-SIPI (real-mode trampoline)
//                * per-CPU info table
//                * spinlocks (xchg-based) + atomic ops
//
//  Designed to be a low-risk, verifiable Stage-8.1 foundation: the
//  BSP brings APs online, each AP prints "CPUx online" on the serial
//  port and then parks itself in a halt loop waiting for IPIs later.
// =====================================================================
#ifndef NEXOS_SMP64_H
#define NEXOS_SMP64_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CPUS 8

// Per-CPU bookkeeping.  Indexed by logical APIC id order (0..n-1).
typedef struct {
    int      present;     // 1 if this CPU came online
    uint32_t apic_id;     // local APIC id
    uint64_t stack_top;   // per-CPU kernel stack top (for later scheduling)
    char     name[8];
} cpu_info_t;

extern cpu_info_t g_cpu[MAX_CPUS];
extern int        g_ncpus;          // total CPUs online (BSP + APs)
extern volatile int g_aps_done;     // count of APs that reached ap_main

// ---- LAPIC / SMP bring-up ----
void lapic_init(void);              // map & enable local APIC on BSP
uint32_t lapic_read(uint32_t reg);
void     lapic_write(uint32_t reg, uint32_t val);
uint32_t lapic_id(void);            // this CPU's APIC id
void     smp_init(void);            // BSP: detect & wake all APs

// ---- AP entry (called from ap_trampoline.asm) ----
void ap_main(int idx);                 // runs on each Application Processor

// ---- per-CPU helpers ----
int  cpu_index(void);               // logical index of current CPU (BSP=0)

// ---- spinlocks ----
typedef volatile uint32_t spinlock_t;
static inline void spin_init(spinlock_t* l){ *l = 0; }
static inline void spin_lock(spinlock_t* l){
    while (__sync_lock_test_and_set((volatile int*)l, 1)) {
        // relax: hint the CPU we are spinning
        __asm__ __volatile__("pause" ::: "memory");
    }
    __asm__ __volatile__("" ::: "memory");
}
static inline void spin_unlock(spinlock_t* l){
    __asm__ __volatile__("" ::: "memory");
    __sync_lock_release((volatile int*)l);
}

// ---- atomic ops ----
static inline int atomic_inc(volatile int* p){
    return __sync_add_and_fetch(p, 1);
}
static inline int atomic_dec(volatile int* p){
    return __sync_sub_and_fetch(p, 1);
}

// ---- serial helpers used by smp code (provided by smp64.cpp itself) ----
void serial_puts(const char* s);
void serial_putc(char c);
void serial_dec(uint32_t v);
void serial_hex(uint32_t v);
void serial_hex64(uint64_t v);

// ---- symbols defined in ap_trampoline.asm ----
extern uint8_t  ap_trampoline_start[];
extern uint8_t  ap_trampoline_end[];
extern uint32_t tramp_cr3;
extern uint32_t tramp_cpu;

#ifdef __cplusplus
}
#endif

#endif // NEXOS_SMP64_H
