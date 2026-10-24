#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver_contract.h"
void* kmalloc(unsigned int n){return malloc(n);}
void kfree(void* p){if(p)free(p);}
void serial_puts(const char* s){fputs(s,stdout);fflush(stdout);}
void serial_putc(char c){fputc(c,stdout);fflush(stdout);}
static unsigned char g[4<<20]; static int gl=0,nofs=0;
int kern_fs_read(const char*n,unsigned char*b,int c){(void)n;if(!nofs)return -1;int x=gl<c?gl:c;memcpy(b,g,(size_t)x);return x;}
int kern_fs_ready(void){return nofs;}
DriverHeader sample_driver_v1, sample_driver_v2;
extern int driver_elf_read(const char*, void**, unsigned int*, void**);
void* elf_image_header(const void*);
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); if(!f)return 1;
  gl=(int)fread(g,1,sizeof(g),f); fclose(f); nofs=1;
  void* base=0; unsigned int size=0; void* img=0;
  int rc=driver_elf_read(argv[1],&base,&size,&img);
  if(rc!=0){printf("elf rc=%d\n",rc);return 1;}
  void* h=elf_image_header(img);
  DriverHeader* dh=(DriverHeader*)h;
  printf("header=%p init=%p unload=%p\n", h, (void*)dh->init, (void*)dh->unload);
  printf("symbols=%p count=%u\n", (void*)dh->symbols, dh->symbol_count);
  for(unsigned i=0;i<dh->symbol_count;i++)
    printf("  sym[%u] name=%s addr=%p\n", i, dh->symbols[i].name, dh->symbols[i].addr);
  /* dump first 48 bytes of init code */
  unsigned char* code=(unsigned char*)dh->init;
  printf("init code bytes: ");
  for(int i=0;i<48;i++) printf("%02x ", code[i]);
  printf("\n");
  return 0;
}
