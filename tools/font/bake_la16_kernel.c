/*
 * bake_la16_kernel.c - tiny bare-metal 64-bit "bake kernel" used ONLY at build
 * time to rasterize the ASCII range (0x20-0x7F) of msyh.ttf into a 16x16
 * grayscale (coverage) bitmap, emitted over the COM1 serial port.
 *
 * Why: the 32-bit OS has no host compiler / no network to run a normal bake
 * tool, but it already ships stb_truetype (the same engine the 64-bit kernel
 * uses).  We compile this file (with the real stb) as a freestanding 64-bit
 * ELF, embed msyh.ttf via objcopy, and boot it directly under
 * qemu-system-x86_64 -kernel.  The host then parses the serial dump and assembles
 * sfs_files/font_la16.bin.  No OS, no libc, no filesystem needed.
 *
 * Serial wire format (per glyph, newline-terminated):
 *   <cp:2hex> <w:2hex> <h:2hex> <w*h bytes, each 2 hex digits>
 * Bracketed by BAKE_START / BAKE_END.
 */
#include <stdint.h>

/* bump allocator (stb frees are no-ops; we never reuse) */
static uint8_t g_heap[1<<20];
static unsigned long g_hp = 0;
static void* my_malloc(unsigned long n){ void*p=(void*)&g_heap[g_hp]; g_hp += (n+15)&~15u; return p; }

/* stb uses STBTT_malloc/free (mapped) AND plain malloc/free/memset/strlen
 * in some code paths, so provide both. */
#define STBTT_malloc(x,u) my_malloc((unsigned long)(x))
#define STBTT_free(x,u)   ((void)0)
void* malloc(size_t n){ return my_malloc((unsigned long)n); }
void  free(void*){ }
void* memset(void* d,int v,size_t n){ uint8_t* p=(uint8_t*)d; for(size_t i=0;i<n;i++)p[i]=(uint8_t)v; return d; }
void* memcpy(void* d,const void* s,size_t n){ return kmemcpy(d,s,(unsigned long)n); }
size_t strlen(const char* s){ size_t n=0; while(s[n])n++; return n; }

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* ---- minimal freestanding shims ---------------------------------------- */
static inline uint8_t inb(uint16_t p){ uint8_t v; asm volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void  outb(uint16_t p,uint8_t v){ asm volatile("outb %0,%1"::"a"(v),"Nd"(p)); }

static void serial_putc(char c){
    while(!(inb(0x3F8+5)&0x20)){}
    outb(0x3F8,c);
}
static void serial_puts(const char*s){ while(*s) serial_putc(*s++); }
static void serial_puthex(uint8_t v){
    static const char h[]="0123456789ABCDEF";
    serial_putc(h[v>>4]); serial_putc(h[v&0xF]);
}

/* freestanding string/mem (stb needs memcpy) */
static void* kmemcpy(void*d,const void*s,size_t n){ uint8_t*a=(uint8_t*)d;const uint8_t*b=(const uint8_t*)s; for(size_t i=0;i<n;i++)a[i]=b[i]; return d; }

/* math: stb needs sqrt/floor/fabs/pow/cos/sin; map to the vecmath shims. */
#include "vecmath/math.h"
#define sqrt  vec_m_sqrt
#define fabs  vec_m_fabs
#define floor vec_m_floor
#define ceil  vec_m_ceil
#define fmod  vec_m_fmod
#define pow   vec_m_pow
#define cos   vec_m_cos
#define sin   vec_m_sin

/* msyh.ttf embedded via objcopy -> _binary_sfs_files_msyh_ttf_start */
extern unsigned char _binary_sfs_files_msyh_ttf_start[];
extern unsigned char _binary_sfs_files_msyh_ttf_end[];

int main(void){
    stbtt_fontinfo info;
    unsigned char* ttf = _binary_sfs_files_msyh_ttf_start;
    uint32_t ttf_len = (uint32_t)(_binary_sfs_files_msyh_ttf_end - _binary_sfs_files_msyh_ttf_start);
    if(!stbtt_InitFont(&info, ttf, 0)){ serial_puts("INITFAIL\n"); return 1; }

    serial_puts("BAKE_START\n");
    int px = 16;
    float scale = stbtt_ScaleForPixelHeight(&info, (float)px);
    for(int i=0;i<96;i++){
        int cp = 0x20 + i;
        int gl = stbtt_FindGlyphIndex(&info, cp);
        int w=0,h=0,xo=0,yo=0;
        unsigned char* bmp = gl ? stbtt_GetGlyphBitmap(&info, scale, scale, gl, &w, &h, &xo, &yo) : 0;
        if(!bmp){ w=px; h=px; }
        serial_puthex((uint8_t)(cp>>8)); serial_puthex((uint8_t)cp);
        serial_puthex((uint8_t)w); serial_puthex((uint8_t)h);
        if(bmp){
            for(int r=0;r<h;r++) for(int c=0;c<w;c++) serial_puthex(bmp[r*w+c]);
            STBTT_free(bmp,0);
        } else {
            for(int k=0;k<w*h;k++) serial_puthex(0);
        }
        serial_puts("\n");
    }
    serial_puts("BAKE_END\n");
    return 0;
}

void _start(void){ main(); asm volatile("hlt"); for(;;); }
