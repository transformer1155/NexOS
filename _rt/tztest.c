#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static bool tz_json_int(const char* body, const char* key, int* out) {
    int klen = 0; while (key[klen]) klen++;
    for (const char* p = body; *p; p++) {
        int i = 0; while (i < klen && p[i] == key[i]) i++;
        if (i != klen) continue;
        const char* q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        int sign = 1;
        if (*q == '-') { sign = -1; q++; } else if (*q == '+') q++;
        if (*q < '0' || *q > '9') return false;
        int v = 0;
        while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
        *out = sign * v;
        return true;
    }
    return false;
}
static bool tz_json_str(const char* body, const char* key, char* out, int cap) {
    int klen = 0; while (key[klen]) klen++;
    for (const char* p = body; *p; p++) {
        int i = 0; while (i < klen && p[i] == key[i]) i++;
        if (i != klen) continue;
        const char* q = p + klen;
        if (*q != '"') return false;
        q++;
        int o = 0;
        while (*q && *q != '"' && o < cap - 1) out[o++] = *q++;
        out[o] = 0;
        return o > 0;
    }
    return false;
}

int main(void) {
    const char* s = "{\"status\":\"success\",\"offset\":28800,\"timezone\":\"Asia/Shanghai\"}";
    int off = 123; char nm[40] = ""; int ok;
    ok = tz_json_int(s, "\"offset\":", &off);
    printf("int ok=%d off=%d (expect 28800)\n", ok, off);
    ok = tz_json_str(s, "\"timezone\":", nm, 40);
    printf("str ok=%d nm=%s\n", ok, nm);

    const char* neg = "{\"offset\":-18000,\"timezone\":\"America/New_York\"}";
    int o2 = 0; char n2[40] = "";
    ok = tz_json_int(neg, "\"offset\":", &o2);
    printf("neg ok=%d off=%d (expect -18000)\n", ok, o2);
    ok = tz_json_str(neg, "\"timezone\":", n2, 40);
    printf("neg str ok=%d nm=%s\n", ok, n2);

    const char* fail = "{\"status\":\"fail\",\"message\":\"reserved\"}";
    int o3 = 777;
    ok = tz_json_int(fail, "\"offset\":", &o3);
    printf("fail ok=%d off=%d (expect 777)\n", ok, o3);
    return 0;
}
