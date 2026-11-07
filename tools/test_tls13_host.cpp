/* Host-side TLS 1.3 client test: talks to openssl s_server. */
#define TLS13_HOST_TEST 1
#include <cstdint>
extern "C" {
extern uint16_t tls13_dbg_scheme;
extern unsigned char tls13_dbg_th[32];
extern int tls13_dbg_siglen;
extern unsigned char tls13_dbg_sig[512];
}
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tls13.h"

static int s_send(void* ctx, const uint8_t* b, int n){
    int fd = *(int*)ctx, off = 0;
    while (off < n){
        int w = (int)send(fd, b + off, n - off, 0);
        if (w <= 0) return -1;
        off += w;
    }
    return n;
}
static int s_recv(void* ctx, uint8_t* b, int n){
    int fd = *(int*)ctx;
    for (;;){
        int r = (int)recv(fd, b, n, 0);
        if (r == 0) return 0;
        if (r < 0){ if (errno == EINTR) continue; return -1; }
        return r;
    }
}
int main(int argc, char** argv){
    int port = argc > 1 ? atoi(argv[1]) : 4433;
    const char* host = argc > 2 ? argv[2] : "127.0.0.1";

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(fd, (sockaddr*)&a, sizeof(a)) != 0){ perror("connect"); return 2; }
    printf("[client] tcp connected\n");

    static tls13_conn c;
    c.io.send = s_send; c.io.recv = s_recv; c.io.ctx = &fd;

    uint8_t anchor[32];
    if (argc < 4){ printf("need root fingerprint arg\n"); return 2; }
    for (int i = 0; i < 32; i++){
        auto v=[](char ch){ if(ch>='0'&&ch<='9')return ch-'0'; if(ch>='a'&&ch<='f')return ch-'a'+10; if(ch>='A'&&ch<='F')return ch-'A'+10; return 0;};
        anchor[i] = (uint8_t)((v(argv[3][2*i])<<4) | v(argv[3][2*i+1]));
    }
    int rc = tls13_connect(&c, "localhost", anchor, 1);
    if (rc != 0){
        printf("[client] handshake FAILED rc=%d err=%s\n", rc, c.error);
        printf("[dbg] scheme=0x%04x siglen=%d\n", tls13_dbg_scheme, tls13_dbg_siglen);
        printf("[dbg] th=");
        for (int i = 0; i < 32; i++) printf("%02x", tls13_dbg_th[i]);
        printf("\n");
        FILE* sf = fopen("/tmp/dbg_sig.bin", "wb");
        if (sf){ fwrite(tls13_dbg_sig, 1, tls13_dbg_siglen, sf); fclose(sf); }
        FILE* tf = fopen("/tmp/dbg_th.bin", "wb");
        if (tf){ fwrite(tls13_dbg_th, 1, 32, tf); fclose(tf); }
        return 3;
    }
    printf("[client] handshake OK (TLS 1.3, ChaCha20-Poly1305, X25519)\n");
    printf("[client] transcript bytes = %d\n", c.transcript_len);

    const char* req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (tls13_write(&c, req, (int)strlen(req)) < 0){ printf("write failed\n"); return 4; }

    char buf[8192];
    int total = 0;
    for (;;){
        int n = tls13_read(&c, buf + total, (int)sizeof(buf) - total - 1);
        if (n <= 0) break;
        total += n;
        if (total > 4000) break;
    }
    buf[total] = 0;
    printf("[client] received %d bytes\n", total);
    printf("[client] first bytes: ");
    for (int i = 0; i < total && i < 32; i++) printf("%02x ", (unsigned char)buf[i]);
    printf("\n---- response head ----\n%.400s\n-----------------------\n", buf);

    int ok = (total > 0) && (strstr(buf, "HTTP/1.") != NULL);
    printf("[client] %s\n", ok ? "PASS: TLS 1.3 application data round-trip" : "FAIL: no HTTP response");
    tls13_close(&c);
    close(fd);
    return ok ? 0 : 5;
}
