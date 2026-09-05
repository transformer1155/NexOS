/* =====================================================================
 *  usr/dynlink_crt.c  -  entry point for dynamically-linked NexOS guests
 * ---------------------------------------------------------------------
 *  A dynamically-linked executable (DT_NEEDED -> libc.so) must NOT take
 *  _start from libc.so (which is built -shared and keeps _start hidden).
 *  This CRT provides the naked _start that reads argc/argv/envp off the
 *  kernel-built i386 SysV stack and calls main().  The in-kernel ELF
 *  dynamic linker (linux_compat.cpp) resolves printf/nex_add/etc. in
 *  libc.so before transferring control here.
 * ===================================================================== */
#include "libc.h"

__attribute__((naked))
void _start(void)
{
    /* On entry, the Linux loader (linux_run / execve) has built the SysV
     * startup stack at the exact esp we were handed:
     *   [esp]      = argc
     *   [esp+4..]  = argv[0..argc-1] pointers, then NULL, then envp NULL
     * This MUST be naked: a normal C prologue (push ebp / sub esp) would
     * shift esp before we read it, so argc/argv would be read wrong. */
    __asm__ volatile (
        "movl  (%%esp), %%eax\n"      /* eax = argc                          */
        "leal  4(%%esp), %%ecx\n"     /* ecx = &argv[0]                      */
        "movl  %%eax, %%edx\n"        /* edx = argc                          */
        "addl  $1, %%edx\n"           /* edx = argc + 1                      */
        "shll  $2, %%edx\n"           /* edx = (argc+1)*4                    */
        "leal  4(%%esp,%%edx), %%ebx\n" /* ebx = &envp[0]                    */
        "pushl %%ebx\n"               /* arg3: envp                          */
        "pushl %%ecx\n"               /* arg2: argv                          */
        "pushl %%eax\n"               /* arg1: argc                          */
        "call  main\n"                /* main(argc, argv, envp) -> eax       */
        "addl  $12, %%esp\n"          /* clean up the three pushed args      */
        "movl  %%eax, %%ebx\n"        /* ebx = exit code                     */
        "movl  $1, %%eax\n"           /* sys_exit                            */
        "int   $0x80\n"
        "1: hlt\n"                    /* unreachable                         */
        : : : "eax", "ebx", "ecx", "edx", "memory"
    );
}
