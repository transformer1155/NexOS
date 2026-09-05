/* =====================================================================
 *  usr/libc.c  -  NexOS native user-runtime entry wrapper
 * ---------------------------------------------------------------------
 *  Provides the naked _start entry point (hidden, so a dynamically-linked
 *  guest can supply its own _start via dynlink_crt.c without a clash) and
 *  pulls in the full runtime implementation from libc_impl.c.
 * ===================================================================== */
#include "libc_impl.c"

/*  Entry point                                                       */
/* ----------------------------------------------------------------- */
/* visibility("hidden") keeps _start out of libc.so's dynamic symbol table
 * so a dynamically-linked guest can supply its own _start (dynlink_crt.c)
 * without a multiple-definition clash at link time.  For the static flat
 * binaries (linux_argv/linux_net) _start is still the entry point.        */
__attribute__((naked, visibility("hidden")))
void _start(void)
{
    /* On entry, the Linux loader (linux_run) has built the SysV startup
     * stack at the exact esp we were handed:
     *   [esp]      = argc
     *   [esp+4..]  = argv[0..argc-1] pointers, then NULL, then envp NULL
     * This function MUST be naked: a normal C prologue (push ebp / sub esp)
     * would shift esp before we read it, so argc/argv would be read from the
     * wrong location (classic freestanding _start bug -> argc came out 0). */
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
        "1: hlt\n"                    /* unreachable; keeps noreturn语义     */
        : : : "eax", "ebx", "ecx", "edx", "memory"
    );
}
