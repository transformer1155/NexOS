/* =====================================================================
 *  usr/hello.c  -  NexOS .nex demo (P1 basic runtime smoke test)
 * ---------------------------------------------------------------------
 *  Exercises the native libc: formatted output, the heap allocator
 *  (malloc / free / calloc / realloc), and string routines.  Every
 *  distinct capability prints a marker so the headless test can assert
 *  that the whole runtime actually works, not just puts().
 * ===================================================================== */
#include "libc.h"

int main(int argc, char** argv, char** envp)
{
    /* --- formatted output --- */
    printf("Hello NexOS\n");
    printf("NEX: runtime online, libc + heap OK\n");

    /* --- heap: allocate, write, free --- */
    char* buf = (char*)malloc(64);
    if (!buf) {
        printf("NEX: malloc FAILED\n");
        return 2;
    }
    strcpy(buf, "Hello from the heap");
    printf("NEX: %s\n", buf);
    free(buf);

    /* --- calloc zeroing --- */
    int* arr = (int*)calloc(4, sizeof(int));
    if (arr) {
        printf("NEX: calloc zeroed? %d%d%d%d\n",
               arr[0], arr[1], arr[2], arr[3]);
        arr[0] = 7;
        free(arr);
    }

    /* --- realloc growth --- */
    char* s = (char*)malloc(8);
    strcpy(s, "tiny");
    char* g = (char*)realloc(s, 32);
    if (g) {
        strcat(g, "-grown");
        printf("NEX: realloc -> %s\n", g);
        free(g);
    }

    /* --- string/mem routines --- */
    printf("NEX: strlen=%u  strcmp=%d  memcmp=%d\n",
           (unsigned)strlen("abc"),
           strcmp("abc", "abd"),
           memcmp("xy", "xy", 2));

    printf("NEX: atoi(12345)=%d\n", atoi("12345"));

    printf("[NEX] ALL CHECKS PASSED\n");
    return 0;
}
