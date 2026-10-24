#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver_contract.h"

/* ---- kernel service stubs ---- */
void* kmalloc(unsigned int n){ return malloc(n); }
void  kfree(void* p){ if(p) free(p); }
void  serial_puts(const char* s){ fputs(s, stdout); }
void  serial_putc(char c){ fputc(c, stdout); }

/* ---- SFS backend: load the .drv from the host filesystem ---- */
static unsigned char g_file[4*1024*1024];
static int g_file_len = 0;
static int g_fs_ready = 0;

static void load_drv_file(const char* p){
    FILE* f = fopen(p, "rb");
    if(!f){ printf("[harness] cannot open %s\n", p); return; }
    g_file_len = (int)fread(g_file, 1, sizeof(g_file), f);
    fclose(f);
    g_fs_ready = 1;
    printf("[harness] loaded %s = %d bytes\n", p, g_file_len);
}

int kern_fs_read(const char* name, unsigned char* buf, int bufsize){
    (void)name;
    if(!g_fs_ready || g_file_len<=0) return -1;
    int n = g_file_len < bufsize ? g_file_len : bufsize;
    memcpy(buf, g_file, (size_t)n);
    return n;
}
int kern_fs_ready(void){ return g_fs_ready; }

/* ---- mock fallback symbols the loader may reference ---- */
DriverHeader sample_driver_v1;
DriverHeader sample_driver_v2;

int main(int argc, char** argv){
    if(argc>1) load_drv_file(argv[1]);
    printf("===== subsystem init =====\n");
    driver_subsystem_init();
    printf("\n===== driver_load(\"%s\") =====\n", argc>1?argv[1]:"sample");
    int rc = driver_load(argc>1?argv[1]:"sample");
    printf("\n>>> driver_load rc=%d (0=OK)\n", rc);
    driver_dump();
    if(rc==0){
        const char* nm = driver_name_at(0);
        printf("\nlive driver name=%s\n", nm?nm:"(none)");
        const DriverHeader* h = driver_find(nm?nm:"");
        if(h) printf("magic=0x%08X version=0x%08X abi=0x%08X\n", h->magic, h->version, h->abi_version);
        int urc = driver_unload(nm?nm:"");
        printf("unload rc=%d count=%d\n", urc, driver_count());
    }
    return 0;
}
