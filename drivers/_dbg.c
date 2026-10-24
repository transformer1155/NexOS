#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "driver_contract.h"
/* Executable heap for the harness: real kernels have no NX on kernel pages. */
void* kmalloc(unsigned int n){
  void* p=mmap(0, n<4096?4096:n, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  return (p==MAP_FAILED)?0:p;
}
void kfree(void* p){ /* leaked on purpose in the harness */ (void)p; }
void serial_puts(const char* s){fputs(s,stdout);fflush(stdout);}
void serial_putc(char c){fputc(c,stdout);fflush(stdout);}
static unsigned char g[4<<20]; static int gl=0,nofs=0;
int kern_fs_read(const char*n,unsigned char*b,int c){(void)n;if(!nofs)return -1;int x=gl<c?gl:c;memcpy(b,g,(size_t)x);return x;}
int kern_fs_ready(void){return nofs;}
DriverHeader sample_driver_v1, sample_driver_v2;
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); if(!f){fprintf(stderr,"no file\n");return 1;}
  gl=(int)fread(g,1,sizeof(g),f); fclose(f); nofs=1;
  driver_subsystem_init();
  int rc=driver_load(argv[1]);
  fprintf(stderr,"\n>>> driver_load rc=%d\n", rc);
  driver_dump();
  if(rc==0){
    const char* nm=driver_name_at(0);
    fprintf(stderr,"live=%s\n", nm?nm:"?");
    const DriverHeader* h=driver_find(nm);
    if(h) fprintf(stderr,"  magic=0x%08X ver=0x%08X\n",h->magic,h->version);
    fprintf(stderr,"unload rc=%d\n", driver_unload(nm));
    fprintf(stderr,"count=%d\n", driver_count());
    fprintf(stderr,"reload rc=%d\n", driver_load(argv[1]));
  }
  return 0;
}
