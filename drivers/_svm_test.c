#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "driver_contract.h"
void* kmalloc(unsigned int n){
  void* p=mmap(0,n<4096?4096:n,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  return (p==MAP_FAILED)?0:p;
}
void kfree(void* p){(void)p;}
void serial_puts(const char* s){fputs(s,stdout);fflush(stdout);}
void serial_putc(char c){fputc(c,stdout);fflush(stdout);}
static unsigned char g[4<<20]; static int gl=0,nofs=0;
int kern_fs_read(const char*n,unsigned char*b,int c){(void)n;if(!nofs)return -1;int x=gl<c?gl:c;memcpy(b,g,(size_t)x);return x;}
int kern_fs_ready(void){return nofs;}
DriverHeader sample_driver_v1, sample_driver_v2;
/* pmm/cpuid stubs: the host CPU has no SVM, so these just satisfy the link. */
static unsigned char pagepool[64][4096]; static int npages=0;
uint32_t pmm_alloc_page(void){ if(npages>=64) return 0; return (uint32_t)(unsigned long)pagepool[npages++]; }
void pmm_free_page(uint32_t pa){(void)pa;}
uint32_t pmm_alloc_range(uint32_t pages){ if(npages+pages>64) return 0; uint32_t b=(uint32_t)(unsigned long)pagepool[npages]; npages+=pages; return b; }
void pmm_free_range(uint32_t pa,uint32_t n){(void)pa;(void)n;}
void nexos_cpuid(uint32_t leaf,uint32_t sub,uint32_t*a,uint32_t*b,uint32_t*c,uint32_t*d){
  uint32_t ra=leaf,rb=0,rc=sub,rd=0;
  __asm__ __volatile__("cpuid":"+a"(ra),"=b"(rb),"+c"(rc),"=d"(rd));
  if(a)*a=ra; if(b)*b=rb; if(c)*c=rc; if(d)*d=rd;
}
int main(int argc,char**argv){
  int fails=0;
  FILE*f=fopen(argv[1],"rb"); if(!f){fprintf(stderr,"no file\n");return 1;}
  gl=(int)fread(g,1,sizeof(g),f); fclose(f); nofs=1;
  printf("=== loaded svm.drv = %d bytes ===\n\n", gl);
  driver_subsystem_init();

  printf("\n--- TEST 1: driver_load(svm.drv) ---\n");
  int rc=driver_load(argv[1]);
  printf(">>> rc=%d\n", rc);
  if(rc==0){ printf("TEST1 PASS: loaded, rc=0\n"); }
  else if(rc==DRIVER_ERR_INIT_FAILED){ printf("TEST1 PASS(cond): init failed gracefully rc=%d (host has no SVM) - no crash\n",rc); }
  else { printf("TEST1 FAIL: rc=%d\n",rc); fails++; }
  driver_dump();

  printf("\n--- TEST 2: registry state ---\n");
  if(rc==0){
    const DriverHeader* h=driver_find("svm");
    if(h && h->magic==DRV_MAGIC){ printf("TEST2 PASS: found svm, magic=0x%08X ver=0x%08X\n",h->magic,h->version); }
    else { printf("TEST2 FAIL: header missing/bad\n"); fails++; }
  } else printf("TEST2 SKIP (load failed on this host)\n");

  printf("\n--- TEST 3: unload ---\n");
  rc=driver_unload("svm");
  printf(">>> unload rc=%d count=%d\n",rc,driver_count());
  if(rc==0 && driver_count()==0){ printf("TEST3 PASS: unloaded cleanly\n"); }
  else if(rc==DRIVER_ERR_NOT_FOUND){ printf("TEST3 PASS(cond): not loaded, nothing to unload\n"); }
  else { printf("TEST3 FAIL: rc=%d count=%d\n",rc,driver_count()); fails++; }

  printf("\n--- TEST 4: hot reload (load after unload) ---\n");
  rc=driver_load(argv[1]);
  printf(">>> reload rc=%d\n",rc);
  if(rc==0 || rc==DRIVER_ERR_INIT_FAILED){ printf("TEST4 PASS: reload handled without crash\n"); }
  else { printf("TEST4 FAIL: rc=%d\n",rc); fails++; }

  printf("\n=== RESULT: %d failure(s) ===\n", fails);
  return fails?1:0;
}
