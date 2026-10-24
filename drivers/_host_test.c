/* host harness: proves the framework logic actually runs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver_contract.h"

void* kmalloc(unsigned int n){ return malloc(n); }
void  kfree(void* p){ free(p); }
void  serial_puts(const char* s){ fputs(s, stdout); }
void  serial_putc(char c){ fputc(c, stdout); }

int main(void){
    printf("===== driver_subsystem_init =====\n");
    driver_subsystem_init();

    printf("\n===== driver_load(\"sample\") =====\n");
    int rc = driver_load("sample");
    printf("driver_load rc=%d (0=OK)\n", rc);
    driver_dump();

    printf("\n===== reject bad magic =====\n");
    rc = driver_load("nope_bad");
    printf("bad-magic rc=%d (want %d)\n", rc, DRIVER_ERR_IO);

    printf("\n===== duplicate load =====\n");
    rc = driver_load("sample");
    printf("duplicate rc=%d (want %d DUPLICATE)\n", rc, DRIVER_ERR_DUPLICATE);

    printf("\n===== hot reload v1 -> v2 =====\n");
    rc = driver_hot_reload("sample_sio", "sample_v2");
    printf("hot_reload rc=%d (0=OK)\n", rc);
    const DriverHeader* h = driver_find("sample_sio");
    if (h) printf("live version now = 0x%08X (want 0x00020000)\n", h->version);
    driver_dump();

    printf("\n===== rollback test: reload with SAME version =====\n");
    rc = driver_hot_reload("sample_sio", "sample_v2");
    printf("same-version rc=%d (want %d BAD_VERSION)\n", rc, DRIVER_ERR_BAD_VERSION);

    printf("\n===== unload =====\n");
    rc = driver_unload("sample_sio");
    printf("unload rc=%d (0=OK)\n", rc);
    printf("count after unload = %d (want 0)\n", driver_count());

    printf("\n===== reload immediately after unload =====\n");
    rc = driver_load("sample");
    printf("re-load rc=%d (0=OK)\n", rc);
    driver_dump();

    printf("\n===== status strings =====\n");
    printf("  ERR_INVALID_MAGIC -> %s\n", driver_status_str(DRIVER_ERR_INVALID_MAGIC));
    printf("  ERR_SYMBOL_NOT_FOUND -> %s\n", driver_status_str(DRIVER_ERR_SYMBOL_NOT_FOUND));
    printf("  ERR_INIT_FAILED -> %s\n", driver_status_str(DRIVER_ERR_INIT_FAILED));
    return 0;
}
