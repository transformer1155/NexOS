/* =====================================================================
 *  usr/linux_dynlink.c  -  Stage 6 dynamic-linking test guest
 * ---------------------------------------------------------------------
 *  Built as a non-PIE ELF32 with DT_NEEDED=libc.so.  Every external
 *  reference (printf, nex_add, putchar, ...) is resolved by the in-kernel
 *  dynamic linker against libc.so at load time.  Success markers:
 *      LXDL: printf_ok          (printf resolved from libc.so)
 *      LXDL: resolved_sym_ok    (nex_add(2,3)==5 resolved from libc.so)
 *  are asserted by tools/verify_linux_dynlink.py.
 * ===================================================================== */
#include "libc.h"

int main(int argc, char** argv, char** envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    /* printf itself is imported from libc.so -- exercising GLOB_DAT/PLT
     * resolution into the shared object. */
    printf("LXDL: hello from dynamically-linked guest\n");
    if (1) printf("LXDL: printf_ok\n");

    /* nex_add is a function defined INSIDE libc.so; calling it across the
     * shared-object boundary forces the linker to fill a GOT/PLT slot. */
    int s = nex_add(2, 3);
    printf("LXDL: nex_add(2,3)=%d\n", s);
    if (s == 5) printf("LXDL: resolved_sym_ok\n");
    else        printf("LXDL: resolved_sym_FAIL\n");

    /* A second cross-.so call to make sure the GOT slot is stable. */
    printf("LXDL: nex_add(10,20)=%d\n", nex_add(10, 20));

    return 0;
}
