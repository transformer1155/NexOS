// Standalone verification of the RRE + Hextile decode logic copied from
// plugins/vnc_client.cpp (stage-3).  The decode LOOPS are copied verbatim;
// only the transport (mock reader) and the pixel sink (test framebuffer) are
// swapped so the algorithm can be exercised off-target.
#include <cstdint>
#include <cstring>
#include <cstdio>

// ---- mock socket reader ----
static const uint8_t* g_in = nullptr;
static size_t g_pos = 0, g_len = 0;
static int mock_read(void* buf, int n){
    if (g_pos + (size_t)n > g_len) return -1;
    memcpy(buf, g_in + g_pos, n);
    g_pos += n;
    return 0;
}
#define vnc_read_exact(buf, n)  mock_read((buf), (n))

// ---- test framebuffer (stand-in for the GUI backbuffer) ----
static const int W = 64, H = 32;
static uint32_t fb[W*H];
static void fb_clear(uint32_t v){ for (int i=0;i<W*H;i++) fb[i]=v; }
static uint32_t fb_get(int x,int y){ return fb[(uint64_t)y*W + x]; }

// pixel reconstruction MUST match vnc_read_pixel() in vnc_client.cpp
static uint32_t vnc_read_pixel(void){
    uint8_t b[4];
    if (vnc_read_exact(b,4) != 0) return 0;
    return ((uint32_t)b[0]) | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
}
static void vnc_fill_rect(int x,int y,int w,int h,uint32_t c){
    for (int r=0;r<h;r++) for (int cc=0;cc<w;cc++){
        int xx=x+cc, yy=y+r; if (xx<0||yy<0||xx>=W||yy>=H) continue;
        fb[(uint64_t)yy*W+xx]=c;
    }
}
static void vnc_blit_raw(int x,int y,int w,int h,const uint8_t* buf){
    const uint32_t* p = (const uint32_t*)buf;
    for (int r=0;r<h;r++) for (int cc=0;cc<w;cc++){
        int xx=x+cc, yy=y+r; if (xx<0||yy<0||xx>=W||yy>=H) continue;
        fb[(uint64_t)yy*W+xx]=p[(uint64_t)r*w+cc];
    }
}

// ---- RRE decode (copied from vnc_client.cpp) ----
static int vnc_decode_rre(int x,int y,int w,int h){
    uint32_t bg = vnc_read_pixel();
    uint8_t nbuf[4]; if (vnc_read_exact(nbuf,4)!=0) return -1;
    uint32_t n = ((uint32_t)nbuf[0]<<24)|((uint32_t)nbuf[1]<<16)|((uint32_t)nbuf[2]<<8)|(uint32_t)nbuf[3];
    vnc_fill_rect(x,y,w,h,bg);
    for (uint32_t i=0;i<n;i++){
        uint32_t pix = vnc_read_pixel();
        uint8_t s[8]; if (vnc_read_exact(s,8)!=0) return -1;
        uint16_t rx=(uint16_t)(((uint16_t)s[0]<<8)|s[1]);
        uint16_t ry=(uint16_t)(((uint16_t)s[2]<<8)|s[3]);
        uint16_t rw=(uint16_t)(((uint16_t)s[4]<<8)|s[5]);
        uint16_t rh=(uint16_t)(((uint16_t)s[6]<<8)|s[7]);
        vnc_fill_rect(x+rx,y+ry,rw,rh,pix);
    }
    return 0;
}

// ---- Hextile decode (copied from vnc_client.cpp) ----
static int vnc_decode_hextile(int x,int y,int w,int h){
    uint32_t bg=0, fg=0;
    uint16_t tx=0;
    while (tx<w){
        uint16_t tw=(w-tx>16)?16:(uint16_t)(w-tx);
        uint16_t ty=0;
        while (ty<h){
            uint16_t th=(h-ty>16)?16:(uint16_t)(h-ty);
            uint8_t mask; if (vnc_read_exact(&mask,1)!=0) return -1;
            if (mask & 0x01){
                int n=(int)tw*(int)th*4;
                uint8_t* buf=new uint8_t[n];
                int rem=n; while(rem>0){ int c=rem>4096?4096:rem; if(vnc_read_exact(buf+(n-rem),c)!=0){delete[]buf;return -1;} rem-=c; }
                vnc_blit_raw(x+tx,y+ty,tw,th,buf); delete[]buf; ty+=16; continue;
            }
            if (mask & 0x02) bg=vnc_read_pixel();
            if (mask & 0x04) fg=vnc_read_pixel();
            vnc_fill_rect(x+tx,y+ty,tw,th,bg);
            if (mask & 0x08){
                uint8_t cnt; if (vnc_read_exact(&cnt,1)!=0) return -1;
                for (uint8_t s=0;s<cnt;s++){
                    uint32_t col=fg;
                    if (mask & 0x10) col=vnc_read_pixel();
                    uint8_t sub[2]; if (vnc_read_exact(sub,2)!=0) return -1;
                    uint16_t rx=(uint16_t)((sub[0]>>4)&0xF);
                    uint16_t ry=(uint16_t)(sub[0]&0xF);
                    uint16_t rw=(uint16_t)(((sub[1]>>4)&0xF)+1);
                    uint16_t rh=(uint16_t)((sub[1]&0xF)+1);
                    vnc_fill_rect(x+tx+rx,y+ty+ry,rw,rh,col);
                }
            }
            ty+=16;
        }
        tx+=16;
    }
    return 0;
}

// ---- test scaffolding ----
static int g_fail=0;
static void expect(int x,int y,uint32_t want,const char* msg){
    uint32_t got=fb_get(x,y);
    if (got!=want){ printf("FAIL %s @(%d,%d): want %08X got %08X\n",msg,x,y,want,got); g_fail++; }
}
static void put32(uint8_t*& p, uint32_t v){ *p++=(uint8_t)(v&0xFF);*p++=(uint8_t)((v>>8)&0xFF);*p++=(uint8_t)((v>>16)&0xFF);*p++=(uint8_t)((v>>24)&0xFF); }
static void put16(uint8_t*& p, uint16_t v){ *p++=(uint8_t)(v&0xFF);*p++=(uint8_t)((v>>8)&0xFF); }
// Big-endian writers (RFB integers are big-endian on the wire)
static void put32be(uint8_t*& p, uint32_t v){ *p++=(uint8_t)((v>>24)&0xFF);*p++=(uint8_t)((v>>16)&0xFF);*p++=(uint8_t)((v>>8)&0xFF);*p++=(uint8_t)(v&0xFF); }
static void put16be(uint8_t*& p, uint16_t v){ *p++=(uint8_t)((v>>8)&0xFF);*p++=(uint8_t)(v&0xFF); }

static uint32_t px(uint8_t r,uint8_t g,uint8_t b){ return (uint32_t)r | ((uint32_t)g<<8) | ((uint32_t)b<<16); }

int main(){
    // ===== RRE test: rect 20x10, bg + 2 subrects =====
    fb_clear(0xDEADBEEF);
    uint8_t buf[8192]; uint8_t* p=buf;
    put32(p, px(10,20,30));                 // bg (pixel, LE)
    put32be(p, 2);                          // nsub (big-endian integer)
    put32(p, px(40,50,60)); put16be(p,2);put16be(p,3);put16be(p,4);put16be(p,5);   // sub1 (coords BE)
    put32(p, px(70,80,90)); put16be(p,10);put16be(p,1);put16be(p,3);put16be(p,2);  // sub2 (coords BE)
    g_in=buf; g_pos=0; g_len=(size_t)(p-buf);
    if (vnc_decode_rre(0,0,20,10)!=0){ printf("FAIL RRE decode returned -1\n"); g_fail++; }
    expect(0,0, px(10,20,30), "RRE bg");
    expect(8,0, px(10,20,30), "RRE outside subrects");  // (8,0): clear of sub1(x2-5) & sub2(x10-12)
    expect(2+1,3+1, px(40,50,60), "RRE sub1 inside");
    expect(2,3, px(40,50,60), "RRE sub1 top-left");
    expect(10+1,1+1, px(70,80,90), "RRE sub2 inside");

    // ===== Hextile test: rect 32x16 = tiles (0,0) and (16,0) =====
    fb_clear(0xDEADBEEF);
    p=buf;
    // tile (0,0): bg + 2 coloured subrects
    *p++ = 0x02|0x08|0x10;                  // bg|anysub|coloured
    put32(p, px(1,2,3));                     // bg
    *p++ = 2;                                // count
    put32(p, px(100,110,120)); *p++=0x12; *p++=0x23;  // x=1,y=2,w=3,h=4
    put32(p, px(130,140,150)); *p++=0x88; *p++=0x11;  // x=8,y=8,w=2,h=2
    // tile (16,0): bg only (no subrects -> fill tile with bg2)
    *p++ = 0x02;                             // bg only
    put32(p, px(4,5,6));                      // bg2
    g_in=buf; g_pos=0; g_len=(size_t)(p-buf);
    if (vnc_decode_hextile(0,0,32,16)!=0){ printf("FAIL Hex decode returned -1\n"); g_fail++; }
    // tile0 checks
    expect(0,0, px(1,2,3), "Hex tile0 bg");
    expect(0+1+1, 0+2+1, px(100,110,120), "Hex tile0 sub1");   // tile0 tx=0; x=1,y=2
    expect(0+8,   0+8,   px(130,140,150), "Hex tile0 sub2");   // tile0 tx=0; x=8,y=8
    // tile1 (tx=16) all bg2
    expect(16+5, 0+5, px(4,5,6), "Hex tile1 bg2");
    expect(31,15, px(4,5,6), "Hex tile1 corner");

    // ===== Hextile Raw tile test: rect 16x16 single tile, Raw =====
    fb_clear(0xDEADBEEF);
    p=buf;
    *p++ = 0x01;                             // Raw
    for (int i=0;i<16*16;i++) put32(p, (uint32_t)(0x111111*i));  // deterministic
    g_in=buf; g_pos=0; g_len=(size_t)(p-buf);
    if (vnc_decode_hextile(0,0,16,16)!=0){ printf("FAIL HexRaw decode -1\n"); g_fail++; }
    expect(0,0, 0x111111*(0), "HexRaw (0,0)");
    expect(5,3, 0x111111*(3*16+5), "HexRaw (5,3)");

    if (g_fail==0) printf("ALL RFB DECODE TESTS PASSED\n");
    else printf("%d FAILURES\n", g_fail);
    return g_fail?1:0;
}
