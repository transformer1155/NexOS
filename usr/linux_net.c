/* =====================================================================
 *  usr/linux_net.c  -  Stage 5 Linux socket bridge test
 * ---------------------------------------------------------------------
 *  Freestanding ELF32 guest that exercises the NexOS Linux-compat socket
 *  syscalls (400-404) end-to-end against a real TCP echo server running
 *  on the host (QEMU user-net presents the host as 10.0.2.2):
 *
 *      nex_socket()  -> fd 0
 *      nex_connect(10.0.2.2, 18099)
 *      nex_send("HELLO_FROM_GUEST")
 *      nex_recv(...) -> must echo the same bytes back
 *
 *  Run:  linux linux_net
 *  Success markers (serial): LXNET: ECHO_OK [HELLO_FROM_GUEST]
 * ===================================================================== */
#include "libc.h"

#define ECHO_IP   NEX_IP(10,0,2,2)   /* QEMU user-net host address */
#define ECHO_PORT 18099

int main(int argc, char** argv, char** envp)
{
    (void)argc; (void)argv; (void)envp;

    printf("LXNET: start\n");

    long fd = nex_socket();
    printf("LXNET: socket fd=%d\n", (int)fd);
    if (fd != 0){
        printf("LXNET: socket FAILED (fd=%d)\n", (int)fd);
        return 1;
    }

    long rc = nex_connect(ECHO_IP, ECHO_PORT);
    printf("LXNET: connect rc=%d\n", (int)rc);
    if (rc != 0){
        printf("LXNET: connect FAILED\n");
        return 1;
    }

    const char* msg = "HELLO_FROM_GUEST";
    int mlen = (int)strlen(msg);
    int sent = (int)nex_send(msg, mlen);
    printf("LXNET: sent %d bytes\n", sent);
    if (sent != mlen){
        printf("LXNET: send FAILED (want %d got %d)\n", mlen, sent);
        nex_sock_close();
        return 1;
    }

    char buf[64];
    int got = (int)nex_recv(buf, mlen);
    printf("LXNET: recv %d bytes\n", got);
    if (got != mlen){
        printf("LXNET: recv MISMATCH (want %d got %d)\n", mlen, got);
        nex_sock_close();
        return 1;
    }
    buf[got] = 0;

    int same = 1;
    for (int i = 0; i < mlen; i++)
        if (buf[i] != msg[i]) { same = 0; break; }

    if (same) printf("LXNET: ECHO_OK [%s]\n", buf);
    else      printf("LXNET: ECHO_MISMATCH got[%s]\n", buf);

    nex_sock_close();
    printf("LXNET: all done\n");
    return same ? 0 : 1;
}
