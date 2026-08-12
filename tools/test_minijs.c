/* tools/test_minijs.c - host-side unit tests for the mini JS interpreter.
   Build:  gcc -O2 -DMINIJS_HOST -o /tmp/tm $(find winpe -name minijs.c) \
             tools/test_minijs.c && /tmp/tm
   This verifies the interpreter logic on the host (has libc for ease),
   BEFORE wiring it into the freestanding PE browser.
*/
#include "../winpe/minijs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int run(const char* src, int expect){
    int out = -999; char err[240]; err[0]=0;
    int rc = minijs_run(src, &out, err, (int)sizeof(err));
    int ok = (rc==0) && (out==expect);
    printf("[%s] rc=%d out=%-4d expect=%-4d  src=%.45s%s\n",
           ok?"PASS":"FAIL", rc, out, expect, src,
           (!ok && err[0]) ? "  err=" : "");
    if (!ok && err[0]) printf("          %s\n", err);
    return ok;
}

int main(void){
    int all=1;
    all &= run("var x = 6 * 7;", 42);
    all &= run("1 + 2 * 3", 7);
    all &= run("(1 + 2) * 3", 9);
    all &= run("var a = 10; var b = 4; a % b", 2);
    all &= run("var a = 10; a = a / 3", 3);
    all &= run("var x = 5; if (x == 5) { x = 100; } x", 100);
    all &= run("var x = 5; if (x > 6) { x = 1; } else { x = 2; } x", 2);
    all &= run("var t = 0; var i = 0; while (i < 3) { t = t + i; i = i + 1; } t", 3);
    all &= run("var x = 2 + 3 * 4 - 1", 13);
    all &= run("-5 + 10", 5);
    all &= run("!0", 1);
    all &= run("var ok = (3 < 4) && (5 > 2); ok", 1);
    all &= run("return 7 * 6; var unused = 0;", 42);
    /* compound assignment (x -= 1) is NOT in the subset; removed. */
    all &= run("var x = 100; x = x - 1; x", 99);
    printf("\nALL: %s\n", all ? "PASS" : "FAIL");
    return all ? 0 : 1;
}
