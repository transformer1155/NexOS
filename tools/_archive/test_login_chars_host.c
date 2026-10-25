#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
void* kmalloc(unsigned int size){return malloc(size);}
void  kfree(void* ptr){free(ptr);}
static unsigned char* g_ttf=NULL; static int g_ttf_len=0;
static int host_read_file(int t,const char* n,uint8_t* b,int bsz){(void)t;
  if(strcmp(n,"msyh.ttf"))return -1; int s=g_ttf_len<bsz?g_ttf_len:bsz; memcpy(b,g_ttf,s); return s;}
#define FONT_VEC_HOST_TEST
#include "font_vec.c"
static int load(const char*p){FILE*f=fopen(p,"rb");if(!f)return-1;fseek(f,0,SEEK_END);g_ttf_len=ftell(f);fseek(f,0,SEEK_SET);g_ttf=malloc(g_ttf_len);fread(g_ttf,1,g_ttf_len,f);fclose(f);return 0;}
static int cov(const uint8_t*b,int w,int h){int n=0;for(int i=0;i<3*w*h;i++)if(b[i]>8)n++;return n;}
int main(int argc,char**argv){const char*p=argc>1?argv[1]:"sfs_files/msyh.ttf";if(load(p)){printf("load fail\n");return 2;}
  int vr=vec_init(host_read_file);printf("vec_init=%d\n",vr);if(vr)return 3;
  struct { uint32_t cp; const char* name; } tests[18];
  const char* cs[18]={"用","码","录","户","名","密","登","续","登","录","切","换","输","默","认","账","户","切"};
  int nt=0;
  for(int i=0;i<18;i++){const char*q=cs[i];uint32_t cp=((q[0]&0x0f)<<12)|(((unsigned char)q[1]&0x3f)<<6)|((unsigned char)q[2]&0x3f);tests[nt].cp=cp;tests[nt].name=cs[i];nt++;}
  int fail=0;
  for(int i=0;i<nt;i++){
    uint32_t cp=tests[i].cp;
    int w,h,xo,yo;const uint8_t*g=vec_glyph(cp,16,&w,&h,&xo,&yo);
    int c=g?cov(g,w,h):-1;
    printf("px16 U+%04X %s : %s cov=%d\n",cp,tests[i].name, g?"OK":"NULL", c);
    if(!g||c<20)fail=1;
  }
  printf(fail?"RESULT: SOME FAIL\n":"RESULT: ALL OK\n");return fail;}