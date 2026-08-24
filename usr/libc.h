/* =====================================================================
 *  usr/libc.h  -  NexOS native user-runtime (P1 basic runtime)
 * ---------------------------------------------------------------------
 *  Minimal freestanding C runtime for NexOS .nex executables.  A .nex is
 *  a flat Linux i386 ELF32 (PT_LOAD only) loaded by linux_run() and run
 *  in ring 0; it talks to the kernel through the unified int 0x80 ABI:
 *      sys_write (eax=4,  ebx=fd, ecx=buf, edx=count) -> COM1 for fd 1/2
 *      sys_exit  (eax=1,  ebx=code)                   -> unwind to shell
 *  No libc, no CRT, no relocations: just _start -> main -> exit.
 * ===================================================================== */
#ifndef NEX_LIBC_H
#define NEX_LIBC_H

#include <stdarg.h>

typedef unsigned long size_t;

/* ---- entry / syscall glue (provided by libc.c) ---- */
void _start(void);                 /* ELF entry; calls main(argc,argv), then nex_exit */
int  main(int argc, char** argv, char** envp);  /* user program entry (envp passed by _start) */

long   nex_write(int fd, const void* buf, unsigned long count);
void   nex_exit(int code);

/* ---- Linux socket bridge (guest TCP to host) ----
 * Thin wrappers over the int 0x80 socket syscalls (400-404) implemented by
 * the kernel's NE2000 stack.  A guest owns one socket at a time.
 *   nex_socket()  -> eax=400   alloc the shared guest socket
 *   nex_connect() -> eax=401   ebx=ip(host order) ecx=port(host order)
 *   nex_send()    -> eax=402   ebx=buf ecx=len
 *   nex_recv()    -> eax=403   ebx=buf ecx=len
 *   nex_sock_close()->eax=404  close the socket
 */
#define NEX_IP(a,b,c,d) (((unsigned long)(a) << 24) | ((unsigned long)(b) << 16) | \
                          ((unsigned long)(c) << 8)  |  (unsigned long)(d))
long   nex_socket(void);
long   nex_connect(unsigned long ip, unsigned long port);
long   nex_send(const void* buf, int len);
long   nex_recv(void* buf, int len);
void   nex_sock_close(void);

/* ---- cross-.so export test (Stage 6 dynamic linking) ----
 * nex_add lives in libc.so and is called by a dynamically-linked guest
 * through the PLT/GOT; the in-kernel linker must resolve it.             */
int    nex_add(int a, int b);

/* ---- stdio ---- */
int    putchar(int c);
int    puts(const char* s);
int    printf(const char* fmt, ...);
int    vprintf(const char* fmt, va_list ap);

/* ---- heap (first-fit free list over a static pool) ---- */
void*  malloc(size_t n);
void   free(void* p);
void*  calloc(size_t n, size_t sz);
void*  realloc(void* p, size_t n);

/* ---- string / memory ---- */
size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
char*  strcpy(char* d, const char* s);
char*  strcat(char* d, const char* s);
char*  strncpy(char* d, const char* s, size_t n);
void*  memcpy(void* d, const void* s, size_t n);
void*  memmove(void* d, const void* s, size_t n);
void*  memset(void* d, int v, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
int    atoi(const char* s);

#endif /* NEX_LIBC_H */
