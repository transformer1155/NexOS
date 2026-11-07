#include <cstdio>
#include <cstring>
#include "tls_crypto.h"
int main(){
    char msg[128];
    for (int len = 0; len <= 70; len++){
        for (int i = 0; i < len; i++) msg[i] = (char)('a' + (i % 26));
        uint8_t d[32];
        sha256(msg, len, d);
        printf("%d ", len);
        for (int i = 0; i < 32; i++) printf("%02x", d[i]);
        printf("\n");
    }
    return 0;
}
