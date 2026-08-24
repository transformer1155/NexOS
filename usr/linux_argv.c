/* =====================================================================
 *  usr/linux_argv.c  -  Stage 4 argv / envp transparency test
 * ---------------------------------------------------------------------
 *  Freestanding ELF32 guest verifying that the NexOS Linux-compat layer
 *  delivers BOTH argv and envp to the new program:
 *    - launched via `linux linux_argv a b c`: argc/argv reflect the
 *      command-line words and the default envp (envp0) is visible;
 *    - launched via `linux linux_argv spawn`: it execve()s ITSELF with a
 *      custom envp (STAGE4=ENV_PASSED), proving execve's r->edx envp is
 *      really parsed and passed through (not the hardcoded default).
 *
 *  Build: freestanding i686-elf (same recipe as other .nex guests)
 *  Run:   linux linux_argv [spawn|word1 word2 ...]
 * ===================================================================== */
#include "libc.h"

#define SYS_EXECVE 11

static inline long sys_execve(const char* path, char** argv, char** envp)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"((long)SYS_EXECVE), "b"((long)path),
          "c"((long)argv), "d"((long)envp)
        : "memory", "cc");
    return ret;
}

/* Find an environment variable of the form "NAME=value" and print its value. */
static void print_env_var(char** envp, const char* name)
{
    int nl = 0; while (name[nl]) nl++;
    for (int i = 0; envp[i]; i++){
        int ok = 1;
        for (int j = 0; j < nl; j++)
            if (envp[i][j] != name[j]) { ok = 0; break; }
        if (ok && envp[i][nl] == '='){          /* exact name match + '=' */
            printf("LXARG: %s=%s\n", name, envp[i] + nl + 1);
            return;
        }
    }
    printf("LXARG: %s=(not set)\n", name);
}

int main(int argc, char** argv, char** envp)
{
    printf("LXARG: start\n");
    printf("LXARG: argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("LXARG: argv[%d]=%s\n", i, argv[i]);

    printf("LXARG: -- envp --\n");
    for (int i = 0; envp[i]; i++)
        printf("LXARG: env[%s]\n", envp[i]);

    print_env_var(envp, "STAGE4");
    print_env_var(envp, "PATH");

    /* Self-spawn: execve() ourselves with a custom envp to prove that the
     * envp passed through sys_execve (r->edx) really reaches the new image,
     * instead of the hardcoded default set. */
    if (argc >= 2 && strcmp(argv[1], "spawn") == 0){
        printf("LXARG: execve self with custom envp\n");
        char* cargv[3];
        cargv[0] = "/linux_argv";
        cargv[1] = "via_execve";
        cargv[2] = 0;
        char* cenvp[3];
        cenvp[0] = "STAGE4=ENV_PASSED";
        cenvp[1] = "FOO=BAR";
        cenvp[2] = 0;
        long r = sys_execve("/linux_argv", cargv, cenvp);
        /* Should not return on success. */
        printf("LXARG: execve FAILED (r=%d)\n", (int)r);
        return 1;
    }

    printf("LXARG: all done\n");
    return 0;
}
