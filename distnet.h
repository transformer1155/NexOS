// =====================================================================
//  distnet.h  -  Minimal distributed compute network (NexOS/MiniOS)
// ---------------------------------------------------------------------
//  A tiny peer-to-peer compute fabric built directly on the kernel UDP
//  layer (net.cpp).  Two roles:
//
//    * compute   - listens for discovery beacons + task datagrams,
//                  executes them, ships results back.
//    * scheduler - discovers compute nodes, dispatches a task, waits
//                  for the result.
//
//  Wire protocol (UDP, newline-terminated ASCII):
//    QUERY                 scheduler -> compute   (BEACON_PORT)
//    BEACON <node>         compute   -> scheduler (BEACON_PORT)
//    TASK <id> <type> ...  scheduler -> compute   (TASK_PORT)
//    RESULT <id> <st> ...  compute   -> scheduler (RESULT_PORT)
//
//  Task types:
//    sum     <ints...>          -> sum of space-separated integers
//    echo    <text...>          -> echo the text
//    compute <N>                -> N*N
//    neg     <int>              -> -N
//    fib     <N>                -> Nth Fibonacci (F_0=0, F_1=1)
//    prime   <N>                -> count of primes in [2..N]
//    gcd     <a> <b>            -> greatest common divisor
//    ai      <prompt...>        -> native GGUF/Qwen inference (RESULT text, or
//                                  "err no_model" when no /boot/model.gguf)
//
//  No libc, no timers: nodes are event-driven and the scheduler spins
//  net_poll() with a bounded iteration guard instead of sleeping.
// =====================================================================

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Network API re-declared from net.cpp (C linkage) ----
extern void     net_poll(void);
extern void     net_log(const char* s);
extern uint32_t net_get_ip(void);
extern void     net_set_ip(uint32_t ip);
extern int      net_udp_bind(uint16_t port,
                             void (*cb)(uint32_t src_ip, uint16_t src_port,
                                        const uint8_t* data, int len));
extern void     net_udp_send(uint32_t dst_ip, uint16_t src_port,
                             uint16_t dst_port, const uint8_t* data, int len);
extern void     net_udp_broadcast(uint16_t src_port, uint16_t dst_port,
                                  const uint8_t* data, int len);

// ---- distnet public API ----
//  Port choice: 5355/5356/5357 were used originally, but UDP 5355 is the
//  IANA-registered LLMNR port (RFC 4795) and is permanently occupied by the
//  DNS Client service on every Windows host -- our beacons collided with it
//  (and host-side test peers could never receive a QUERY).  Moved to the
//  unassigned 5455..5457 block.
#define DN_BEACON_PORT  5455
#define DN_TASK_PORT    5456
#define DN_RESULT_PORT  5457
// Weight-transfer ports (separate so a weight stream never collides with the
// compute task/result traffic during a live demo).
#define DN_WREQ_PORT    5458   // scheduler -> node:  "WREQ <seed> <nbytes>"
#define DN_WGHT_PORT    5459   // node -> scheduler:  "WGHT <idx> <ntotal> <bytes...>"
#define DN_WDONE_PORT   5460   // node -> scheduler:  "WDONE <seed> <nbytes> <crc>"

void distnet_compute(void);
// Discover a compute node (broadcast if target_ip==NULL), dispatch one task,
// and log the RESULT.  `type`+`args` build the wire line "TASK 1 <type> <args>"
// (spaces in args are preserved, so it can carry an "ai" prompt).  If type is
// NULL the default "sum 1 2 3 4 5" demo is used.
void distnet_scheduler(const char* target_ip, const char* type, const char* args);
void distnet_scheduler_ai(const char* prompt,    // dispatch "TASK 1 ai <prompt>"
                          const char* target_ip);// NULL => broadcast discovery
void distnet_nodes(const char* target_ip);       // discovery-only: list peers
void cmd_distnet(const char* args);             // kernel shell command

#ifdef __cplusplus
}
#endif
