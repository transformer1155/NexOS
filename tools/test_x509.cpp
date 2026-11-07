#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "tls_x509.h"

static uint8_t g_buf[65536];

int main(int argc, char** argv){
    if (argc < 3){ printf("usage: %s certs.bin rootfp.hex\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    int n = (int)fread(g_buf, 1, sizeof(g_buf), f);
    fclose(f);

    uint8_t anchor[32];
    const char* fp = argv[2];
    for (int i = 0; i < 32; i++){
        auto v=[](char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return 0;};
        anchor[i] = (uint8_t)((v(fp[2*i])<<4)|v(fp[2*i+1]));
    }

    const char* reason = "?";
    int rc = x509_verify_chain(g_buf, n, 2, "localhost", anchor, 1, &reason);
    printf("chain(localhost) rc=%d reason=%s  %s\n", rc, reason, rc==0?"PASS":"FAIL");
    int ok1 = (rc == 0);

    rc = x509_verify_chain(g_buf, n, 2, "evil.example.com", anchor, 1, &reason);
    printf("wrong hostname   rc=%d reason=%s  %s\n", rc, reason, rc!=0?"PASS":"FAIL");
    int ok2 = (rc != 0);

    uint8_t bad[32]; memcpy(bad, anchor, 32); bad[0] ^= 1;
    rc = x509_verify_chain(g_buf, n, 2, "localhost", bad, 1, &reason);
    printf("wrong anchor     rc=%d reason=%s  %s\n", rc, reason, rc!=0?"PASS":"FAIL");
    int ok3 = (rc != 0);

    rc = x509_verify_chain(g_buf, n, 2, "localhost", NULL, 0, &reason);
    printf("no anchors       rc=%d reason=%s  %s\n", rc, reason, rc!=0?"PASS":"FAIL");
    int ok4 = (rc != 0);

    printf("\n%s\n", (ok1&&ok2&&ok3&&ok4)?"ALL PASS":"SOME FAILED");
    return (ok1&&ok2&&ok3&&ok4)?0:1;
}
