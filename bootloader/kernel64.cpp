// =====================================================================
//  kernel64.cpp  -  Freestanding C++ 64-bit kernel with shell, mouse & scrollback
// ---------------------------------------------------------------------
//  Runs in 64-bit long mode, no standard library.
//
//  Features:
//   * VGA text output at 0xB8000 with a scrollback history buffer.
//   * PS/2 keyboard driver (Scan Code Set 1) + arrow keys.
//   * PS/2 mouse driver (Intellimouse 4-byte packets, wheel support).
//   * Mouse wheel / Up-Down arrows scroll back through history.
//   * Mini command shell with directory navigation (cd/mkdir/pwd).
//   * ATA PIO disk driver for persistent storage.
//   * MKFS: custom writable file system with directory support.
//   * SFS:  compatible read-only file system (pre-built by Makefile).
//   * MBR partition table reader + FAT32 file system reader.
//   * .sh script execution from MKFS or SFS.
//   * Path separators: both / and \ are accepted.
//
//  Build:
//    g++ -m32 -ffreestanding -fno-exceptions -fno-rtti -nostdlib
//        -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -O2 -c
// =====================================================================

#include <stdint.h>

// =====================================================================
//  Port I/O helpers
// =====================================================================
static inline uint8_t  inb(uint16_t p){ uint8_t v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p){ uint16_t v; __asm__ __volatile__("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outb(uint16_t p,uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void outw(uint16_t p,uint16_t v){ __asm__ __volatile__("outw %0,%1"::"a"(v),"Nd"(p)); }

// =====================================================================
//  Serial debug output (port 0x3F8)  -  for UEFI boot tracing
// =====================================================================
static void serial_putc(char c){ outb(0x3F8, (uint8_t)c); }
static void serial_puts(const char* s){ while(*s) outb(0x3F8, (uint8_t)*s++); }

// =====================================================================
//  Tiny libc (freestanding)
// =====================================================================
static int   strlen_(const char* s){ int n=0; while(s[n]) n++; return n; }
static int   strcmp_(const char* a,const char* b){ while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
static int   strncmp_(const char* a,const char* b,int n){ while(n>0&&*a&&*a==*b){a++;b++;n--;} return n==0?0:(unsigned char)*a-(unsigned char)*b; }
static bool  startswith_(const char* s,const char* prefix){ while(*prefix){ if(*s!=*prefix) return false; s++; prefix++; } return true; }
static void* memset_(void* d,int v,int n){ unsigned char* p=(unsigned char*)d; while(n--) *p++=(unsigned char)v; return d; }
static void* memcpy_(void* d,const void* s,int n){ unsigned char* dp=(unsigned char*)d; const unsigned char* sp=(const unsigned char*)s; while(n--) *dp++=*sp++; return d; }

// =====================================================================
//  AI Engine interface (implemented in ai_engine.cpp)
// =====================================================================
extern "C" {
    int   ai_init(const char* model_path);
    char* ai_generate(const char* prompt, uint32_t max_tokens);
    void  ai_cleanup(void);
    int   ai_get_info(char* buf, int bufsize);
    int   ai_set_mode(int mode);
    int   ai_transformer_test(void);
    void  agent_init(void);
    int   agent_run(const char* goal, char* output, int outsize);
    int   agent_get_status(char* buf, int bufsize);
}
static bool g_ai_initialized = false;

// =====================================================================
//  Network stack interface (implemented in net.cpp)
// =====================================================================
extern "C" {
    int  net_init(void);
    void net_poll(void);
    int  net_status(char* buf, int bufsize);
}
static bool g_net_initialized = false;

// =====================================================================
//  32-bit kernel switcher (implemented in switch64to32.asm)
// =====================================================================
extern "C" void switch_to_32bit(void);

#define KERNEL32_LBA     33       // LBA where kernel.bin is stored on disk
#define KERNEL32_SECTORS 256      // 128KB max (256 * 512 bytes)
#define KERNEL32_ADDR    0x10000  // Load address: 64KB (matches linker.ld)

// =====================================================================
//  ATA PIO disk driver (primary master, LBA28)
//  QEMU exposes the raw image here. Used to persist the command file.
// =====================================================================
static bool ata_wait_bsy(){
    for(int i=0;i<100000;i++){ if(!(inb(0x1F7)&0x80)) return true; }
    return false;  // timeout (no disk)
}
static bool ata_wait_drq(){
    for(int i=0;i<100000;i++){ if(inb(0x1F7)&0x08) return true; }
    return false;  // timeout (no disk)
}

static void ata_read_sector(uint32_t lba, uint16_t* buf){
    if(!ata_wait_bsy()) return;
    outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
    outb(0x1F1, 0x00);
    outb(0x1F2, 1);
    outb(0x1F3, lba & 0xFF);
    outb(0x1F4, (lba>>8) & 0xFF);
    outb(0x1F5, (lba>>16) & 0xFF);
    outb(0x1F7, 0x20);                 // READ SECTORS
    if(!ata_wait_bsy()) return;
    if(!ata_wait_drq()) return;
    for(int i=0;i<256;i++) buf[i]=inw(0x1F0);
}
static void ata_write_sector(uint32_t lba, const uint16_t* buf){
    if(!ata_wait_bsy()) return;
    outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
    outb(0x1F1, 0x00);
    outb(0x1F2, 1);
    outb(0x1F3, lba & 0xFF);
    outb(0x1F4, (lba>>8) & 0xFF);
    outb(0x1F5, (lba>>16) & 0xFF);
    outb(0x1F7, 0x30);                 // WRITE SECTORS
    if(!ata_wait_drq()) return;
    for(int i=0;i<256;i++) outw(0x1F0, buf[i]);
    outb(0x1F7, 0xE7);                 // CACHE FLUSH
    ata_wait_bsy();
}

// =====================================================================
//  VGA text mode 3 setup (80x25, 16 colors)
//  Needed for UEFI boot: OVMF leaves the VGA in a graphics mode.
//  BIOS boot: VGA is already in text mode, so this is harmless.
// =====================================================================
static void vga_set_text_mode(){
    outb(0x3C2, 0x67);
    static const uint8_t seq_data[5] = {0x03, 0x01, 0x03, 0x00, 0x02};
    for (int i = 0; i < 5; i++) { outb(0x3C4, i); outb(0x3C5, seq_data[i]); }
    outb(0x3D4, 0x11); outb(0x3D5, 0x0E);
    static const uint8_t crtc_data[25] = {
        0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
        0x00, 0x4F, 0x0E, 0x0F, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
        0xFF
    };
    for (int i = 0; i < 25; i++) { outb(0x3D4, i); outb(0x3D5, crtc_data[i]); }
    static const uint8_t gc_data[9] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
    };
    for (int i = 0; i < 9; i++) { outb(0x3CE, i); outb(0x3CF, gc_data[i]); }
    static const uint8_t ac_data[21] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x0C, 0x00, 0x0F, 0x08, 0x00
    };
    inb(0x3DA);
    for (int i = 0; i < 21; i++) { outb(0x3C0, i); outb(0x3C0, ac_data[i]); }
    outb(0x3C0, 0x20);
}

// =====================================================================
//  Clipboard  -  copy/paste buffer with history (needed by Terminal)
// =====================================================================
#define CLIP_LEN       256
#define CLIP_HIST_MAX  8

static char g_clipboard[CLIP_LEN];
static int  g_clipboard_len = 0;
static char g_clip_hist[CLIP_HIST_MAX][CLIP_LEN];
static int  g_clip_hist_count = 0;
static int  g_clip_hist_idx = 0;

static void clipboard_set(const char* text, int len){
    if(len >= CLIP_LEN) len = CLIP_LEN - 1;
    memcpy_(g_clipboard, text, len);
    g_clipboard[len] = 0;
    g_clipboard_len = len;
    if(g_clip_hist_count < CLIP_HIST_MAX){
        memcpy_(g_clip_hist[g_clip_hist_count], g_clipboard, len+1);
        g_clip_hist_count++;
    } else {
        for(int i=0; i<CLIP_HIST_MAX-1; i++)
            memcpy_(g_clip_hist[i], g_clip_hist[i+1], CLIP_LEN);
        memcpy_(g_clip_hist[CLIP_HIST_MAX-1], g_clipboard, len+1);
    }
    g_clip_hist_idx = g_clip_hist_count - 1;
}

static void clipboard_hist_prev(){
    if(g_clip_hist_count == 0) return;
    if(g_clip_hist_idx > 0) g_clip_hist_idx--;
    memcpy_(g_clipboard, g_clip_hist[g_clip_hist_idx], CLIP_LEN);
    g_clipboard_len = strlen_(g_clipboard);
}

static void clipboard_hist_next(){
    if(g_clip_hist_count == 0) return;
    if(g_clip_hist_idx < g_clip_hist_count - 1) g_clip_hist_idx++;
    memcpy_(g_clipboard, g_clip_hist[g_clip_hist_idx], CLIP_LEN);
    g_clipboard_len = strlen_(g_clipboard);
}

namespace {

// ----- VGA constants -----
constexpr int VGA_WIDTH  = 80;
constexpr int VGA_HEIGHT = 25;
volatile uint16_t* const VGA_MEMORY = reinterpret_cast<volatile uint16_t*>(0xB8000);

constexpr int SCROLLBACK_LINES = 200;

enum VgaColor : uint8_t {
    BLACK=0, BLUE=1, GREEN=2, CYAN=3, RED=4, MAGENTA=5,
    BROWN=6, LIGHT_GREY=7, WHITE=15, YELLOW=14,
};
inline uint8_t  make_color(VgaColor fg, VgaColor bg){ return (uint8_t)(fg | (bg<<4)); }
inline uint16_t make_entry(unsigned char c, uint8_t color){ return (uint16_t)c | ((uint16_t)color<<8); }

struct Line { int len; char data[VGA_WIDTH]; };

// =====================================================================
//  Terminal  -  VGA output with scrollback history + view scrolling
// =====================================================================
class Terminal {
public:
    void init(){
        m_head=0; m_count=0; m_cur_len=0; m_cur_pos=0; m_prompt_len=0; m_at_bottom=true; m_view=0;
        m_color=make_color(LIGHT_GREY,BLACK);
        clear_screen();
    }
    void set_color(uint8_t c){ m_color=c; }

    void clear_screen(){
        m_head=0; m_count=0; m_cur_len=0; m_cur_pos=0; m_prompt_len=0; m_at_bottom=true; m_view=0;
        for(int i=0;i<VGA_WIDTH*VGA_HEIGHT;i++) VGA_MEMORY[i]=make_entry(' ',m_color);
        render();
    }

    void put_char(char c){
        if(c=='\n'){ commit_line(); return; }
        if(c=='\b'){
            if(m_cur_pos>0){
                // Shift characters left from cursor position
                for(int i=m_cur_pos-1; i<m_cur_len-1; i++)
                    m_cur[i]=m_cur[i+1];
                m_cur_len--;
                m_cur_pos--;
            }
            return;
        }
        if(m_cur_len<VGA_WIDTH){
            // Insert at cursor position: shift characters right
            for(int i=m_cur_len; i>m_cur_pos; i--)
                m_cur[i]=m_cur[i-1];
            m_cur[m_cur_pos++]=c;
            m_cur_len++;
        }
    }
    void write(const char* s){ while(*s) put_char(*s++); }

    void write_dec(int v){
        char b[12]; int i=0;
        if(v==0){ put_char('0'); return; }
        if(v<0){ put_char('-'); v=-v; }
        while(v){ b[i++]='0'+v%10; v/=10; }
        while(i) put_char(b[--i]);
    }

    void write_hex(uint64_t v){
        put_char('0'); put_char('x');
        char b[16]; int i=0;
        if(v==0){ put_char('0'); return; }
        while(v){ uint8_t d=v&0xF; b[i++]=(d<10)?('0'+d):('A'+d-10); v>>=4; }
        while(i) put_char(b[--i]);
    }

    void scroll_view(int delta){
        m_at_bottom=false;
        m_view += delta;
        int bot=bottom_view();
        if(m_view<0) m_view=0;
        if(m_view>bot) { m_view=bot; m_at_bottom=true; }
        render();
    }
    void snap_bottom(){ m_at_bottom=true; render(); }
    bool is_at_bottom() const { return m_at_bottom; }

    // ----- Cursor movement (PowerShell-style) -----
    void cursor_left(){  if(m_cur_pos>m_prompt_len)    m_cur_pos--; render(); }
    void cursor_right(){ if(m_cur_pos<m_cur_len)       m_cur_pos++; render(); }
    void cursor_home(){  m_cur_pos=m_prompt_len; render(); }
    void cursor_end(){   m_cur_pos=m_cur_len; render(); }
    int  cursor_pos() const { return m_cur_pos; }
    // Mark where user input begins (after prompt)
    void begin_input(){ m_prompt_len = m_cur_len; m_cur_pos = m_cur_len; }
    void set_cursor_col(int col){
        int abs_col = m_prompt_len + col;
        if(abs_col < m_prompt_len) abs_col = m_prompt_len;
        if(abs_col > m_cur_len) abs_col = m_cur_len;
        m_cur_pos = abs_col;
        render();
    }
    // Replace entire current input line content (for history recall)
    // 's' is the user input only (prompt is preserved)
    void set_line(const char* s, int len){
        m_cur_len = m_prompt_len;
        m_cur_pos = m_prompt_len;
        for(int i=0; i<len && m_cur_len<VGA_WIDTH-1; i++){
            m_cur[m_cur_len++]=s[i];
        }
        m_cur_pos=m_cur_len;
        render();
    }
    // Get current input line content (user input only, excluding prompt)
    void get_line(char* buf, int* len){
        int n = m_cur_len - m_prompt_len;
        if(n < 0) n = 0;
        for(int i=0; i<n; i++) buf[i]=m_cur[m_prompt_len + i];
        buf[n]=0;
        *len=n;
    }

    // ----- Mouse cursor & text selection -----
    void update_mouse(int dx, int dy){
        m_mouse_visible = true;
        m_mouse_x += dx / 3;
        m_mouse_y -= dy / 3;       // PS/2 Y is inverted
        if(m_mouse_x < 0) m_mouse_x = 0;
        if(m_mouse_x >= VGA_WIDTH)  m_mouse_x = VGA_WIDTH - 1;
        if(m_mouse_y < 0) m_mouse_y = 0;
        if(m_mouse_y >= VGA_HEIGHT) m_mouse_y = VGA_HEIGHT - 1;
        render();
    }
    void mouse_left_down(){
        m_selecting = true;
        m_sel_sx = m_sel_ex = m_mouse_x;
        m_sel_sy = m_sel_ey = m_mouse_y;
        render();
    }
    void mouse_left_drag(){
        if(m_selecting){
            m_sel_ex = m_mouse_x;
            m_sel_ey = m_mouse_y;
            render();
        }
    }
    void mouse_left_up(){
        if(m_selecting){
            m_selecting = false;
            m_has_selection = (m_sel_sx != m_sel_ex) || (m_sel_sy != m_sel_ey);
            if(m_has_selection) copy_selection_to_clipboard();
            render();
        }
    }
    void mouse_click(){
        // Mouse click: if on the current input line, move cursor to mouse X
        m_has_selection = false;
        m_selecting = false;
        snap_bottom();
        // Check if click is on the last screen row (current input line)
        int input_row = (m_count - m_view);
        if(m_mouse_y >= VGA_HEIGHT - 1 || (m_at_bottom && m_mouse_y == input_row)){
            // Click is on or near the input line - set cursor relative to prompt
            int rel = m_mouse_x - m_prompt_len;
            set_cursor_col(rel);
        }
    }
    void clear_selection(){
        m_selecting = false;
        m_has_selection = false;
        render();
    }
    bool has_selection() const { return m_has_selection || m_selecting; }
    bool mouse_visible() const { return m_mouse_visible; }
    void hide_mouse(){ m_mouse_visible = false; render(); }

    void render();

private:
    int bottom_view() const { return (m_count>VGA_HEIGHT-1)?(m_count-(VGA_HEIGHT-1)):0; }
    Line& line_at(int i){ int idx=((m_head-m_count+i)%SCROLLBACK_LINES+SCROLLBACK_LINES)%SCROLLBACK_LINES; return m_lines[idx]; }
    void commit_line(){
        Line& L=m_lines[m_head];
        L.len=m_cur_len;
        for(int i=0;i<m_cur_len;i++) L.data[i]=m_cur[i];
        m_head=(m_head+1)%SCROLLBACK_LINES;
        if(m_count<SCROLLBACK_LINES) m_count++;
        m_cur_len=0;
        m_cur_pos=0;
        m_prompt_len=0;
    }
    void show_cursor(){
        outb(0x3D4,0x0A); outb(0x3D5,0x0E);
        outb(0x3D4,0x0B); outb(0x3D5,0x0F);
    }
    void hide_cursor(){ outb(0x3D4,0x0A); outb(0x3D5,0x2E); }
    void set_cursor_pos(int row,int col){
        uint16_t pos=(uint16_t)(row*VGA_WIDTH+col);
        outb(0x3D4,0x0F); outb(0x3D5,(uint8_t)(pos&0xFF));
        outb(0x3D4,0x0E); outb(0x3D5,(uint8_t)((pos>>8)&0xFF));
    }

    Line    m_lines[SCROLLBACK_LINES];
    int     m_head;
    int     m_count;
    int     m_view;
    bool    m_at_bottom;
    int     m_cur_len;
    int     m_cur_pos;       // cursor position within current line (PowerShell-style)
    int     m_prompt_len;    // length of prompt text (user input starts after this)
    char    m_cur[VGA_WIDTH];
    uint8_t m_color;

    // Mouse cursor & selection
    int     m_mouse_x = 0;
    int     m_mouse_y = 0;
    bool    m_mouse_visible = false;
    bool    m_selecting = false;
    bool    m_has_selection = false;
    int     m_sel_sx=0, m_sel_sy=0;   // selection start
    int     m_sel_ex=0, m_sel_ey=0;   // selection end

    void copy_selection_to_clipboard(){
        // Normalize: ensure start <= end
        int sx=m_sel_sx, sy=m_sel_sy, ex=m_sel_ex, ey=m_sel_ey;
        if(sy > ey || (sy==ey && sx > ex)){
            int tx=sx; sx=ex; ex=tx;
            int ty=sy; sy=ey; ey=ty;
        }
        char buf[CLIP_LEN];
        int blen = 0;
        for(int row=sy; row<=ey && blen<CLIP_LEN-1; row++){
            int col_start = (row==sy) ? sx : 0;
            int col_end   = (row==ey) ? ex : VGA_WIDTH-1;
            // Get the line text for this screen row
            int li = m_view + row;
            if(m_at_bottom && row == (m_count - m_view)){
                // Current input line
                for(int c=col_start; c<=col_end && c<m_cur_len && blen<CLIP_LEN-1; c++)
                    buf[blen++] = m_cur[c];
            } else if(li >= 0 && li < m_count){
                Line& L = line_at(li);
                for(int c=col_start; c<=col_end && c<L.len && blen<CLIP_LEN-1; c++)
                    buf[blen++] = L.data[c];
            }
            if(row < ey && blen < CLIP_LEN-1)
                buf[blen++] = '\n';
        }
        if(blen > 0){
            buf[blen] = 0;
            clipboard_set(buf, blen);
        }
    }
};

// =====================================================================
//  Keyboard  -  PS/2, Scan Code Set 1, with extended (arrow) keys
// =====================================================================
enum KbdType {
    K_NONE, K_CHAR, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_TAB,
    K_CTRL_C, K_CTRL_V, K_CTRL_L, K_CTRL_UP, K_CTRL_DOWN,
    K_PAGEUP, K_PAGEDN, K_HOME, K_END
};
struct KbdEvent { KbdType type; char ch; };

enum ScanCode : uint8_t {
    SC_BACKSPACE=0x0E, SC_TAB=0x0F, SC_ENTER=0x1C, SC_LSHIFT=0x2A,
    SC_RSHIFT=0x36, SC_CAPSLOCK=0x3A, SC_SPACE=0x39,
    SC_LCTRL=0x1D,
};
const char SC_ASCII_NORMAL[128]={
    0,0x1B,'1','2','3','4','5','6','7','8','9','0','-','=',0x08,'\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0,
};
const char SC_ASCII_SHIFT[128]={
    0,0x1B,'!','@','#','$','%','^','&','*','(',')','_','+',0x08,'\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0,
};

class Keyboard {
public:
    KbdEvent process(uint8_t sc){
        KbdEvent e; e.type=K_NONE; e.ch=0;
        if(sc==0xE0){ m_ext=true; return e; }
        bool brk = sc&0x80;
        uint8_t key= sc&0x7F;
        if(brk){
            if(key==SC_LSHIFT||key==SC_RSHIFT) m_shift=false;
            if(key==SC_LCTRL) m_ctrl=false;
            m_ext=false;
            return e;
        }
        if(m_ext){
            m_ext=false;
            if(key==0x48){
                if(m_ctrl) e.type=K_CTRL_UP;
                else       e.type=K_UP;
            }
            else if(key==0x50){
                if(m_ctrl) e.type=K_CTRL_DOWN;
                else       e.type=K_DOWN;
            }
            else if(key==0x4B) e.type=K_LEFT;
            else if(key==0x4D) e.type=K_RIGHT;
            else if(key==0x49) e.type=K_PAGEUP;
            else if(key==0x51) e.type=K_PAGEDN;
            else if(key==0x47) e.type=K_HOME;
            else if(key==0x4F) e.type=K_END;
            return e;
        }
        if(key==SC_LSHIFT||key==SC_RSHIFT){ m_shift=true; return e; }
        if(key==SC_LCTRL){ m_ctrl=true; return e; }
        if(key==SC_CAPSLOCK){ m_caps=!m_caps; return e; }

        // Ctrl+key combinations
        if(m_ctrl){
            if(key==0x2E){ e.type=K_CTRL_C; return e; }   // Ctrl+C
            if(key==0x2F){ e.type=K_CTRL_V; return e; }   // Ctrl+V
            if(key==0x26){ e.type=K_CTRL_L; return e; }   // Ctrl+L
            return e;  // swallow other Ctrl combos
        }

        if(key==SC_ENTER){ e.type=K_CHAR; e.ch='\n'; return e; }
        if(key==SC_BACKSPACE){ e.type=K_CHAR; e.ch='\b'; return e; }
        if(key==SC_TAB){ e.type=K_TAB; return e; }
        if(key==SC_SPACE){ e.type=K_CHAR; e.ch=' '; return e; }
        if(key>=128) return e;
        char c = m_shift?SC_ASCII_SHIFT[key]:SC_ASCII_NORMAL[key];
        if(c==0) return e;
        if(m_caps && c>='a'&&c<='z') c-=32;
        else if(m_caps && c>='A'&&c<='Z') c+=32;
        e.type=K_CHAR; e.ch=c; return e;
    }
private:
    bool m_shift=false, m_caps=false, m_ext=false, m_ctrl=false;
};

// =====================================================================
//  Mouse  -  PS/2 Intellimouse (4-byte packets with wheel / Z axis)
// =====================================================================
struct MouseEvent {
    int  dx;       // X movement delta
    int  dy;       // Y movement delta
    int  dz;       // wheel delta
    bool left;     // left button
    bool right;    // right button
    bool middle;   // middle button
    bool valid;    // complete packet received
};

class Mouse {
public:
    void init(){
        outb(0x64,0xAD);
        outb(0x64,0xA7);
        while(inb(0x64)&0x01) inb(0x60);
        outb(0x64,0xA8);
        cmd(0xF3); param(200);
        cmd(0xF3); param(100);
        cmd(0xF3); param(80);
        cmd(0xF3); param(60);
        cmd(0xF4);
        outb(0x64,0xAE);
    }
    MouseEvent process(uint8_t b){
        MouseEvent ev={0,0,0,false,false,false,false};
        if(m_i==0 && !(b&0x08)) return ev;
        m_pkt[m_i++]=b;
        if(m_i>=4){
            m_i=0;
            ev.valid  = true;
            ev.left   = m_pkt[0]&0x01;
            ev.right  = m_pkt[0]&0x02;
            ev.middle = m_pkt[0]&0x04;
            ev.dx     = (int8_t)m_pkt[1];
            ev.dy     = (int8_t)m_pkt[2];
            ev.dz     = (int8_t)m_pkt[3];
        }
        return ev;
    }
private:
    void wait_write(){ while(inb(0x64)&0x02){} }
    void cmd(uint8_t c){ outb(0x64,0xD4); wait_write(); outb(0x60,c); ack(); }
    void param(uint8_t p){ outb(0x64,0xD4); wait_write(); outb(0x60,p); ack(); }
    void ack(){ for(int i=0;i<1000;i++){ if(inb(0x64)&0x01){ if(inb(0x60)==0xFA) return; } } }
    uint8_t m_pkt[4];
    int     m_i=0;
};

} // namespace

// Global instances
static Terminal  term;
static Keyboard  kbd;
static Mouse     mouse;

// ---- GUI functions (implemented in gui.cpp) ----
// GUI Callback interface - allows kernel to provide data to GUI
struct GuiCallbacks {
    uint32_t (*get_total_mem_kb)(void);
    uint32_t (*get_free_pages)(void);
    uint32_t (*get_used_pages)(void);
    uint32_t (*get_total_pages)(void);
    uint32_t (*get_heap_alloc_bytes)(void);
    uint32_t (*get_heap_free_bytes)(void);
    uint32_t (*get_heap_alloc_count)(void);
    uint32_t (*get_heap_free_count)(void);
    void     (*optimize_memory)(void);
    int      (*list_files)(int fs_type, char* buf, int bufsize);
    int      (*read_file)(int fs_type, const char* name, uint8_t* buf, int bufsize);
    void     (*get_time)(int* h, int* m, int* s);
    const char* (*get_os_name)(void);
    bool     (*is_64bit)(void);
    // Browser callbacks
    int      (*browser_navigate)(const char* url);
    int      (*browser_status)(void);
    int      (*browser_get_page)(char* buf, int bufsize);
    void     (*browser_reset)(void);
    // Terminal command execution
    void     (*exec_command)(const char* cmd, char* output, int outsize);
    // Hardware info callbacks
    const char* (*get_cpu_vendor)(void);
    const char* (*get_disk_model)(void);
    uint32_t (*get_disk_size_mb)(void);
    int      (*get_nic_present)(void);
    int      (*get_mouse_present)(void);
    int      (*get_keyboard_present)(void);
    uint32_t (*get_pci_count)(void);
    int      (*get_bga_available)(void);
    int      (*get_vbe_mode_set)(void);
    int      (*get_cpu_64bit_capable)(void);
};

extern "C" {
    void gui_set_callbacks(const GuiCallbacks* cb);
    int  gui_init(void);
    int  gui_available(void);
    void gui_enter(void);
    int  gui_is_active(void);
    void gui_mouse_move(int dx, int dy);
    void gui_mouse_down(void);
    void gui_mouse_up(void);
    int  gui_handle_key(char ch);
    void gui_create_window(int x, int y, int w, int h, const char* title);
    void gui_draw_text(int x, int y, const char* text);
    void gui_fill_rect(int x, int y, int w, int h, uint32_t color);
    int  gui_get_width(void);
    int  gui_get_height(void);
    void gui_render_text_mode(void);
    void gui_tick(void);
    void gui_exit(void);
    void gui_render(void);
}

// Global VBE flag (set from 0x500D in kernel main)
static bool g_vbe_active = false;

// =====================================================================
//  File System Layer
// =====================================================================
//  Three file systems coexist on the same disk:
//
//  MKFS (Mini Kernel File System) - custom, writable, with directories
//    LBA 512:     Superblock  (magic "MKFS", file_count, free_lba)
//    LBA 513-528: File table   (16 sectors, 16 entries/sector = 256 max)
//    LBA 529-799: Data area    (271 sectors = 135 KB)
//
//  SFS (Simple File System) - compatible, read-only
//    Pre-built by Makefile from files in sfs_files/ directory.
//    LBA 800:     Superblock  (magic "SFS", file_count)
//    LBA 801-816: Directory   (16 sectors, 256 max entries)
//    LBA 817-1023:Data area   (207 sectors = 103 KB)
//
//  FAT32 (Windows-compatible) - read-only, mounted from MBR partition
//    Detected via MBR partition table at LBA 0.
//    Read BPB, follow FAT chains, parse 8.3 directory entries.
//
//  File entry (32 bytes, shared by MKFS and SFS):
//    name[20] + size(4) + start_lba(4) + type(1) + parent(2) + reserved(1)
//    type: 0=file, 1=directory
//    parent: entry index of parent directory (0xFFFF = root)

#define MKFS_SUPER_LBA    512
#define MKFS_TABLE_LBA    513
#define MKFS_TABLE_SECT   16
#define MKFS_DATA_LBA     529
#define MKFS_DATA_SECTORS 271

#define SFS_SUPER_LBA     800
#define SFS_DIR_LBA       801
#define SFS_DIR_SECT      16
#define SFS_DATA_LBA      817

#define FS_NAME_LEN       20
#define FS_ENTRY_SIZE     32
#define FS_ENTRY_PER_SEC  16
#define FS_IOBUF_SIZE     8192
#define FS_WRITEBUF_SIZE  8192

#define FS_TYPE_FILE      0
#define FS_TYPE_DIR       1
#define FS_ROOT_PARENT    0xFFFF

struct FileEntry {
    char     name[FS_NAME_LEN];
    uint32_t size;
    uint32_t start_lba;
    uint8_t  type;
    uint16_t parent;
    uint8_t  reserved;
};

struct Superblock {
    char     magic[4];
    uint16_t version;
    uint16_t file_count;
    uint32_t data_start;
    uint32_t free_lba;
    uint32_t total_sectors;
};

// Shared buffers
static uint8_t g_fsbuf[512];
static uint8_t g_iobuf[FS_IOBUF_SIZE];
static char    g_writebuf[FS_WRITEBUF_SIZE];
static int     g_write_len = 0;
static char    g_write_name[FS_NAME_LEN];

// Current working directory (entry index in MKFS file table, 0xFFFF=root)
static uint16_t g_cwd = FS_ROOT_PARENT;

// Shell modes
enum ShellMode { MODE_NORMAL, MODE_WRITE };
static ShellMode g_mode = MODE_NORMAL;

// =====================================================================
//  Path utilities  -  normalize \ to /, parse path components
// =====================================================================

// Convert all backslashes to forward slashes in-place
static void normalize_path(char* s){
    for(int i=0; s[i]; i++)
        if(s[i]=='\\') s[i]='/';
}

// Check if character is a path separator (/ or \)
static bool is_path_sep(char c){ return c=='/' || c=='\\'; }

// =====================================================================
//  Mkfs - Mini Kernel File System (custom, writable, with directories)
// =====================================================================
class Mkfs {
public:
    bool       mounted;
    Superblock sb;

    void init(){
        ata_read_sector(MKFS_SUPER_LBA, (uint16_t*)g_fsbuf);
        memcpy_(&sb, g_fsbuf, sizeof(sb));
        mounted = (sb.magic[0]=='M' && sb.magic[1]=='K' &&
                   sb.magic[2]=='F' && sb.magic[3]=='S');
    }

    void format(){
        sb.magic[0]='M'; sb.magic[1]='K'; sb.magic[2]='F'; sb.magic[3]='S';
        sb.version = 1;
        sb.file_count = 0;
        sb.data_start = MKFS_DATA_LBA;
        sb.free_lba = MKFS_DATA_LBA;
        sb.total_sectors = MKFS_DATA_SECTORS;
        memset_(g_fsbuf, 0, 512);
        memcpy_(g_fsbuf, &sb, sizeof(sb));
        ata_write_sector(MKFS_SUPER_LBA, (const uint16_t*)g_fsbuf);

        memset_(g_fsbuf, 0, 512);
        for (int s = 0; s < MKFS_TABLE_SECT; s++)
            ata_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);

        mounted = true;
    }

    // List files/dirs in current directory (g_cwd)
    void ls(){
        if (!mounted) { term.write("MKFS not formatted. Use 'mkfs' first.\n"); return; }
        int count = 0;
        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0 && fe->parent == g_cwd) {
                    if (fe->type == FS_TYPE_DIR) {
                        term.set_color(make_color(CYAN, BLACK));
                        term.write("  [DIR]  "); term.write(fe->name);
                        term.write("\n");
                        term.set_color(make_color(LIGHT_GREY, BLACK));
                    } else {
                        term.write("  "); term.write(fe->name);
                        term.write("  ("); term.write_dec((int)fe->size);
                        term.write(" bytes)\n");
                    }
                    count++;
                }
            }
        }
        if (count == 0) term.write("  (empty)\n");
    }

    // Find entry by name in current directory
    int find(const char* name){
        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0 && fe->parent == g_cwd &&
                    strcmp_(fe->name, name) == 0)
                    return s * FS_ENTRY_PER_SEC + e;
            }
        }
        return -1;
    }

    int read(const char* name, void* buf, int bufsize){
        int idx = find(name);
        if (idx < 0) return -1;
        read_entry(idx);
        FileEntry* fe = (FileEntry*)g_fsbuf;
        int size = (int)fe->size;
        if (size > bufsize) size = bufsize;
        int sectors = ((int)fe->size + 511) / 512;
        uint8_t* dst = (uint8_t*)buf;
        int remaining = size;
        for (int i = 0; i < sectors && remaining > 0; i++) {
            ata_read_sector(fe->start_lba + i, (uint16_t*)g_fsbuf);
            int to_copy = (remaining > 512) ? 512 : remaining;
            memcpy_(dst, g_fsbuf, to_copy);
            dst += 512;
            remaining -= to_copy;
        }
        return size;
    }

    int create(const char* name, const void* data, int size){
        if (!mounted) return -1;
        int existing = find(name);
        if (existing >= 0) remove_idx(existing);

        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] == 0) {
                    int sectors = (size + 511) / 512;
                    if (sectors == 0) sectors = 1;
                    uint32_t lba = sb.free_lba;
                    if (lba + sectors > MKFS_DATA_LBA + MKFS_DATA_SECTORS)
                        return -5;  // disk full

                    const uint8_t* src = (const uint8_t*)data;
                    for (int i = 0; i < sectors; i++) {
                        memset_(g_fsbuf, 0, 512);
                        int off = i * 512;
                        int to_copy = (size - off > 512) ? 512 : (size - off);
                        if (to_copy > 0) memcpy_(g_fsbuf, src + off, to_copy);
                        ata_write_sector(lba + i, (const uint16_t*)g_fsbuf);
                    }

                    // Re-read table sector and write entry
                    ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
                    fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                    memset_((void*)fe, 0, FS_ENTRY_SIZE);
                    int i=0; while(name[i] && i<FS_NAME_LEN-1){ fe->name[i]=name[i]; i++; }
                    fe->name[i] = 0;
                    fe->size = (uint32_t)size;
                    fe->start_lba = lba;
                    fe->type = FS_TYPE_FILE;
                    fe->parent = g_cwd;
                    fe->reserved = 0;
                    ata_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);

                    sb.file_count++;
                    sb.free_lba += sectors;
                    flush_sb();
                    return size;
                }
            }
        }
        return -4;  // table full
    }

    // Create a directory entry
    int mkdir(const char* name){
        if (!mounted) return -1;
        int existing = find(name);
        if (existing >= 0) return -2;  // already exists

        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] == 0) {
                    memset_((void*)fe, 0, FS_ENTRY_SIZE);
                    int i=0; while(name[i] && i<FS_NAME_LEN-1){ fe->name[i]=name[i]; i++; }
                    fe->name[i] = 0;
                    fe->size = 0;
                    fe->start_lba = 0;
                    fe->type = FS_TYPE_DIR;
                    fe->parent = g_cwd;
                    fe->reserved = 0;
                    ata_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);

                    sb.file_count++;
                    flush_sb();
                    return 0;
                }
            }
        }
        return -4;  // table full
    }

    // Change current directory
    int cd(const char* name){
        if (!mounted) return -1;

        // Handle root
        if (strcmp_(name, "/")==0 || strcmp_(name, "\\")==0 || name[0]==0) {
            g_cwd = FS_ROOT_PARENT;
            return 0;
        }
        // Handle parent
        if (strcmp_(name, "..")==0) {
            if (g_cwd == FS_ROOT_PARENT) return 0;  // already at root
            read_entry(g_cwd);
            FileEntry* fe = (FileEntry*)g_fsbuf;
            g_cwd = fe->parent;
            return 0;
        }
        // Handle current
        if (strcmp_(name, ".")==0) return 0;

        int idx = find(name);
        if (idx < 0) return -2;  // not found
        read_entry(idx);
        FileEntry* fe = (FileEntry*)g_fsbuf;
        if (fe->type != FS_TYPE_DIR) return -3;  // not a directory
        g_cwd = (uint16_t)idx;
        return 0;
    }

    // Print working directory path
    void pwd(){
        if (g_cwd == FS_ROOT_PARENT) {
            term.write("/\n");
            return;
        }
        // Walk up parent chain to build path
        uint16_t path[16];
        int depth = 0;
        uint16_t cur = g_cwd;
        while (cur != FS_ROOT_PARENT && depth < 16) {
            path[depth++] = cur;
            read_entry(cur);
            FileEntry* fe = (FileEntry*)g_fsbuf;
            cur = fe->parent;
        }
        term.write("/");
        for (int i = depth - 1; i >= 0; i--) {
            read_entry(path[i]);
            FileEntry* fe = (FileEntry*)g_fsbuf;
            term.write(fe->name);
            if (i > 0) term.write("/");
        }
        term.write("\n");
    }

    // Get current directory name for prompt
    void cwd_name(char* buf, int maxlen){
        if (g_cwd == FS_ROOT_PARENT) {
            buf[0] = '/'; buf[1] = 0;
            return;
        }
        read_entry(g_cwd);
        FileEntry* fe = (FileEntry*)g_fsbuf;
        int i=0; while(fe->name[i] && i<maxlen-1){ buf[i]=fe->name[i]; i++; }
        buf[i] = 0;
    }

    int remove(const char* name){
        if (!mounted) return -1;
        int idx = find(name);
        if (idx < 0) return -2;
        // Don't allow removing non-empty directories
        read_entry(idx);
        FileEntry* fe = (FileEntry*)g_fsbuf;
        if (fe->type == FS_TYPE_DIR) {
            // Check if directory has children
            for (int s = 0; s < MKFS_TABLE_SECT; s++) {
                ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
                for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                    FileEntry* child = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                    if (child->name[0] != 0 && child->parent == (uint16_t)idx)
                        return -3;  // directory not empty
                }
            }
        }
        remove_idx(idx);
        return 0;
    }

    // Copy a file: read source, create destination with same content
    int copy(const char* src, const char* dst){
        if (!mounted) return -1;
        int ret = read(src, g_iobuf, FS_IOBUF_SIZE);
        if (ret < 0) return -2;  // source not found
        return create(dst, g_iobuf, ret);
    }

private:
    void read_entry(int idx){
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        memcpy_(g_fsbuf, g_fsbuf + e * FS_ENTRY_SIZE, FS_ENTRY_SIZE);
    }

    void remove_idx(int idx){
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        memset_((void*)fe, 0, FS_ENTRY_SIZE);
        ata_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);
        if (sb.file_count > 0) sb.file_count--;
        flush_sb();
    }

    void flush_sb(){
        memset_(g_fsbuf, 0, 512);
        memcpy_(g_fsbuf, &sb, sizeof(sb));
        ata_write_sector(MKFS_SUPER_LBA, (const uint16_t*)g_fsbuf);
    }
};

// =====================================================================
//  Sfs - Simple File System (compatible, read-only)
// =====================================================================
class Sfs {
public:
    bool       mounted;
    Superblock sb;

    void init(){
        ata_read_sector(SFS_SUPER_LBA, (uint16_t*)g_fsbuf);
        memcpy_(&sb, g_fsbuf, sizeof(sb));
        mounted = (sb.magic[0]=='S' && sb.magic[1]=='F' && sb.magic[2]=='S');
    }

    void ls(){
        if (!mounted) { term.write("SFS not found on disk.\n"); return; }
        int count = 0;
        for (int s = 0; s < SFS_DIR_SECT; s++) {
            ata_read_sector(SFS_DIR_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0) {
                    term.write("  "); term.write(fe->name);
                    term.write("  ("); term.write_dec((int)fe->size);
                    term.write(" bytes)\n");
                    count++;
                }
            }
        }
        if (count == 0) term.write("  (empty)\n");
    }

    int find(const char* name){
        for (int s = 0; s < SFS_DIR_SECT; s++) {
            ata_read_sector(SFS_DIR_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0 && strcmp_(fe->name, name) == 0)
                    return s * FS_ENTRY_PER_SEC + e;
            }
        }
        return -1;
    }

    int read(const char* name, void* buf, int bufsize){
        int idx = find(name);
        if (idx < 0) return -1;
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        ata_read_sector(SFS_DIR_LBA + s, (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        int size = (int)fe->size;
        if (size > bufsize) size = bufsize;
        int sectors = ((int)fe->size + 511) / 512;
        uint8_t* dst = (uint8_t*)buf;
        int remaining = size;
        for (int i = 0; i < sectors && remaining > 0; i++) {
            ata_read_sector(fe->start_lba + i, (uint16_t*)g_fsbuf);
            int to_copy = (remaining > 512) ? 512 : remaining;
            memcpy_(dst, g_fsbuf, to_copy);
            dst += 512;
            remaining -= to_copy;
        }
        return size;
    }
};

// =====================================================================
//  MBR partition table reader
// =====================================================================
//  MBR is at LBA 0. Partition table starts at offset 446,
//  4 entries of 16 bytes each. Signature 0x55AA at offset 510.

struct MbrPartition {
    uint8_t  boot_flag;       // 0x80 = bootable
    uint8_t  start_chs[3];
    uint8_t  type;            // partition type
    uint8_t  end_chs[3];
    uint32_t start_lba;
    uint32_t total_sectors;
};

static const char* part_type_name(uint8_t type){
    switch(type){
        case 0x00: return "Empty";
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 <32M";
        case 0x05: return "Extended";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS/exFAT";
        case 0x0B: return "FAT32";
        case 0x0C: return "FAT32 LBA";
        case 0x0E: return "FAT16 LBA";
        case 0x0F: return "Ext LBA";
        case 0x82: return "Linux Swap";
        case 0x83: return "Linux";
        case 0x8E: return "Linux LVM";
        case 0xA5: return "FreeBSD";
        case 0xEE: return "GPT Protective";
        case 0xEF: return "EFI System";
        default:   return "Unknown";
    }
}

static bool is_fat_type(uint8_t type){
    return type==0x01 || type==0x04 || type==0x06 ||
           type==0x0B || type==0x0C || type==0x0E;
}

// Read MBR partition table and display it
// Returns partition start LBA for a given partition index (1-4), or 0 if not found
static uint32_t read_mbr_partition(int part_num, MbrPartition* out){
    ata_read_sector(0, (uint16_t*)g_fsbuf);
    uint8_t* mbr = g_fsbuf;

    // Check MBR signature
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return 0;

    // Partition table at offset 446, 4 entries x 16 bytes
    MbrPartition* parts = (MbrPartition*)(mbr + 446);
    for (int i = 0; i < 4; i++) {
        if (i + 1 == part_num) {
            *out = parts[i];
            return parts[i].start_lba;
        }
    }
    return 0;
}

// =====================================================================
//  Fat32 - Windows-compatible FAT32 file system reader (read-only)
// =====================================================================
class Fat32 {
public:
    bool      mounted;
    uint32_t  part_start;       // partition start LBA
    uint16_t  bytes_per_sector;
    uint8_t   sectors_per_cluster;
    uint16_t  reserved_sectors;
    uint8_t   num_fats;
    uint32_t  fat_size;         // sectors per FAT
    uint32_t  root_cluster;
    uint32_t  data_start;       // first data sector LBA
    uint32_t  total_sectors;

    void init(){ mounted = false; }

    bool mount(uint32_t start_lba){
        part_start = start_lba;
        ata_read_sector(start_lba, (uint16_t*)g_fsbuf);
        uint8_t* bpb = g_fsbuf;

        // Check FAT signature
        if (bpb[510] != 0x55 || bpb[511] != 0xAA)
            return false;

        bytes_per_sector   = *(uint16_t*)(bpb + 11);
        sectors_per_cluster = bpb[13];
        reserved_sectors    = *(uint16_t*)(bpb + 14);
        num_fats            = bpb[16];
        uint16_t root_entries = *(uint16_t*)(bpb + 17);
        uint16_t fat_size_16  = *(uint16_t*)(bpb + 22);
        uint32_t total_sec_32 = *(uint32_t*)(bpb + 32);

        // FAT32 has root_entries == 0 and fat_size_16 == 0
        if (root_entries != 0) return false;

        fat_size = fat_size_16 ? fat_size_16 : *(uint32_t*)(bpb + 36);
        root_cluster = *(uint32_t*)(bpb + 44);
        total_sectors = total_sec_32 ? total_sec_32 : *(uint16_t*)(bpb + 19);

        data_start = part_start + reserved_sectors + num_fats * fat_size;

        mounted = (bytes_per_sector == 512 && sectors_per_cluster > 0);
        return mounted;
    }

    void info(){
        if (!mounted) { term.write("No FAT32 partition mounted.\n"); return; }
        term.write("FAT32 partition info:\n");
        term.write("  Start LBA:       "); term.write_dec((int)part_start); term.write("\n");
        term.write("  Bytes/sector:    "); term.write_dec((int)bytes_per_sector); term.write("\n");
        term.write("  Sectors/cluster: "); term.write_dec((int)sectors_per_cluster); term.write("\n");
        term.write("  Reserved:        "); term.write_dec((int)reserved_sectors); term.write(" sectors\n");
        term.write("  FATs:            "); term.write_dec((int)num_fats); term.write("\n");
        term.write("  FAT size:        "); term.write_dec((int)fat_size); term.write(" sectors\n");
        term.write("  Root cluster:    "); term.write_dec((int)root_cluster); term.write("\n");
        term.write("  Data start:      LBA "); term.write_dec((int)data_start); term.write("\n");
        term.write("  Total sectors:   "); term.write_dec((int)total_sectors); term.write("\n");

        // Volume label (offset 71 in boot sector, 11 chars)
        ata_read_sector(part_start, (uint16_t*)g_fsbuf);
        term.write("  Volume label:    ");
        for (int i = 0; i < 11; i++) {
            char c = (char)g_fsbuf[71 + i];
            if (c >= 0x20) term.put_char(c);
        }
        term.put_char('\n');
    }

    // List files in root directory
    void ls(){
        if (!mounted) { term.write("No FAT32 partition mounted.\n"); return; }
        int count = 0;
        uint32_t cluster = root_cluster;

        for (int cl = 0; cl < 32; cl++) {  // limit clusters
            if (cluster < 2 || cluster >= 0x0FFFFFF8) break;

            uint32_t lba = data_start + (cluster - 2) * sectors_per_cluster;
            // Read cluster into g_iobuf
            for (int s = 0; s < sectors_per_cluster && s * 512 < FS_IOBUF_SIZE; s++)
                ata_read_sector(lba + s, (uint16_t*)(g_iobuf + s * 512));

            // Parse directory entries (32 bytes each)
            int entries = (sectors_per_cluster * 512) / 32;
            if (entries > FS_IOBUF_SIZE / 32) entries = FS_IOBUF_SIZE / 32;

            for (int e = 0; e < entries; e++) {
                uint8_t* de = g_iobuf + e * 32;
                if (de[0] == 0x00) goto done;       // end of directory
                if (de[0] == 0xE5) continue;          // deleted entry
                uint8_t attr = de[11];
                if (attr & 0x08) continue;            // volume label
                if (attr & 0x0F) continue;            // long filename entry

                bool is_dir = (attr & 0x10) != 0;

                if (is_dir) {
                    term.set_color(make_color(CYAN, BLACK));
                    term.write("  [DIR]  ");
                } else {
                    term.write("  ");
                }

                // Print 8.3 name
                for (int i = 0; i < 8; i++) {
                    if (de[i] != ' ') term.put_char(de[i]);
                }
                if (de[8] != ' ') {
                    term.put_char('.');
                    for (int i = 8; i < 11; i++) {
                        if (de[i] != ' ') term.put_char(de[i]);
                    }
                }

                if (!is_dir) {
                    uint32_t fsize = *(uint32_t*)(de + 28);
                    term.write("  ("); term.write_dec((int)fsize); term.write(" bytes)");
                }
                term.write("\n");
                if (is_dir) term.set_color(make_color(LIGHT_GREY, BLACK));
                count++;
            }

            // Follow FAT chain
            cluster = next_cluster(cluster);
        }
    done:
        if (count == 0) term.write("  (empty)\n");
    }

private:
    uint32_t next_cluster(uint32_t cluster){
        uint32_t fat_offset = cluster * 4;
        uint32_t fat_sector = part_start + reserved_sectors + fat_offset / 512;
        ata_read_sector(fat_sector, (uint16_t*)g_fsbuf);
        uint32_t entry = *(uint32_t*)(g_fsbuf + (fat_offset % 512));
        return entry & 0x0FFFFFFF;
    }
};

static Mkfs   mkfs;
static Sfs    sfs;
static Fat32  fat32;

// =====================================================================
//  Memory Management: Physical (PMM) + Virtual (VMM) + Heap
// =====================================================================
//  PMM:  Bitmap page-frame allocator for 4 KiB pages (1 MB+).
//  VMM:  x86 32-bit 2-level paging (4 MB identity-map + 4 KiB map).
//  Heap: Linked-list first-fit allocator on a 1 MiB region at 2 MB.
// ---------------------------------------------------------------------

// --- Constants ---
constexpr uint32_t PAGE_SIZE       = 4096;
constexpr uint32_t PAGE_SHIFT      = 12;
constexpr uint32_t PMM_BASE_ADDR   = 0x100000;     // 1 MiB – managed start
constexpr uint32_t PMM_BITMAP_ADDR = 0x80000;      // 8 KiB bitmap (256 MB max)
constexpr uint32_t PMM_MAX_PAGES   = 65536;        // 256 MB / 4 KiB
constexpr uint32_t HEAP_START      = 0x200000;     // 2 MiB
constexpr uint32_t HEAP_SIZE       = 0x1000000;    // 16 MiB (expanded for AI engine)
constexpr uint32_t HEAP_END        = HEAP_START + HEAP_SIZE;

// Page-table / PDE flags
constexpr uint32_t PG_PRESENT  = 0x001;
constexpr uint32_t PG_RW       = 0x002;
constexpr uint32_t PG_USER     = 0x004;
constexpr uint32_t PG_PSE      = 0x080;   // 4 MiB page (PS bit in PDE)

// --- CRx / MSR helpers (64-bit) ---
static inline uint64_t read_cr0(){ uint64_t v; __asm__ __volatile__("mov %%cr0,%0":"=r"(v)); return v; }
static inline uint64_t read_cr3(){ uint64_t v; __asm__ __volatile__("mov %%cr3,%0":"=r"(v)); return v; }
static inline uint64_t read_cr4(){ uint64_t v; __asm__ __volatile__("mov %%cr4,%0":"=r"(v)); return v; }
static inline void write_cr0(uint64_t v){ __asm__ __volatile__("mov %0,%%cr0"::"r"(v)); }
static inline void write_cr3(uint64_t v){ __asm__ __volatile__("mov %0,%%cr3"::"r"(v)); }
static inline void write_cr4(uint64_t v){ __asm__ __volatile__("mov %0,%%cr4"::"r"(v)); }
static inline void invlpg(uint64_t addr){ __asm__ __volatile__("invlpg (%0)"::"r"(addr)); }

// =====================================================================
//  Physical Memory Manager (PMM)
// =====================================================================
static uint32_t* pmm_bitmap   = (uint32_t*)PMM_BITMAP_ADDR;
static uint32_t  pmm_total_pages = 0;
static uint32_t  pmm_free_pages  = 0;
static uint32_t  pmm_used_pages  = 0;
static uint32_t  pmm_mem_kb      = 0;

// Detect total memory via CMOS RTC (ports 0x70/0x71)
static uint32_t detect_memory_kb(){
    // Offset 0x17/0x18 = extended memory in KiB (15-65 MiB range)
    outb(0x70, 0x17); uint8_t lo = inb(0x71);
    outb(0x70, 0x18); uint8_t hi = inb(0x71);
    uint32_t ext = ((uint32_t)hi << 8) | lo;
    if (ext > 0 && ext < 65535) return 1024 + ext;
    // Fallback: offset 0x30/0x31
    outb(0x70, 0x30); lo = inb(0x71);
    outb(0x70, 0x31); hi = inb(0x71);
    ext = ((uint32_t)hi << 8) | lo;
    if (ext > 0) return 1024 + ext;
    return 64 * 1024;  // assume 64 MiB
}

static void pmm_init(){
    pmm_mem_kb = detect_memory_kb();
    uint32_t total_bytes = pmm_mem_kb * 1024;
    pmm_total_pages = (total_bytes - PMM_BASE_ADDR) / PAGE_SIZE;
    if (pmm_total_pages > PMM_MAX_PAGES) pmm_total_pages = PMM_MAX_PAGES;

    // Clear bitmap (all free)
    uint32_t bm_words = (pmm_total_pages + 31) / 32;
    for (uint32_t i = 0; i < bm_words; i++) pmm_bitmap[i] = 0;

    pmm_free_pages = pmm_total_pages;
    pmm_used_pages = 0;

    // Mark kernel binary region as used (64-bit kernel is at 0x100000 = PMM_BASE_ADDR)
    // Reserve 256 KB (64 pages) for kernel code + data + BSS
    uint32_t kernel_pages = 64;
    for (uint32_t i = 0; i < kernel_pages; i++) {
        if (i < pmm_total_pages) {
            pmm_bitmap[i / 32] |= (1 << (i % 32));
            pmm_free_pages--;
            pmm_used_pages++;
        }
    }

    // Mark heap region (2-18 MiB) as used
    uint32_t heap_pages = HEAP_SIZE / PAGE_SIZE;
    for (uint32_t i = 0; i < heap_pages; i++) {
        uint32_t page_idx = (HEAP_START - PMM_BASE_ADDR) / PAGE_SIZE + i;
        if (page_idx < pmm_total_pages) {
            pmm_bitmap[page_idx / 32] |= (1 << (page_idx % 32));
            pmm_free_pages--;
            pmm_used_pages++;
        }
    }
    serial_puts("[PMM] Initialised\n");
}

static uint32_t pmm_alloc_page(){
    for (uint32_t i = 0; i < pmm_total_pages; i++){
        uint32_t w = i / 32, b = i % 32;
        if (!(pmm_bitmap[w] & (1u << b))){
            pmm_bitmap[w] |= (1u << b);
            pmm_free_pages--;
            pmm_used_pages++;
            return PMM_BASE_ADDR + i * PAGE_SIZE;
        }
    }
    return 0;  // OOM
}

static void pmm_free_page(uint32_t phys){
    if (phys < PMM_BASE_ADDR) return;
    uint32_t i = (phys - PMM_BASE_ADDR) / PAGE_SIZE;
    if (i >= pmm_total_pages) return;
    uint32_t w = i / 32, b = i % 32;
    if (pmm_bitmap[w] & (1u << b)){
        pmm_bitmap[w] &= ~(1u << b);
        pmm_free_pages++;
        pmm_used_pages--;
    }
}

static uint32_t pmm_alloc_range(uint32_t count){
    // Contiguous allocation
    for (uint32_t i = 0; i <= pmm_total_pages - count; i++){
        bool ok = true;
        for (uint32_t j = 0; j < count; j++){
            if (pmm_bitmap[(i+j)/32] & (1u << ((i+j)%32))){ ok = false; break; }
        }
        if (ok){
            for (uint32_t j = 0; j < count; j++){
                pmm_bitmap[(i+j)/32] |= (1u << ((i+j)%32));
                pmm_free_pages--;
                pmm_used_pages++;
            }
            return PMM_BASE_ADDR + i * PAGE_SIZE;
        }
    }
    return 0;
}

// =====================================================================
//  Virtual Memory Manager (VMM)
// =====================================================================
//  Page directory at 0x70000 (4 KiB).  Uses 4 MiB PSE pages for
//  identity-mapping the first 8 MiB, with the ability to split
//  individual 4 MiB regions into 4 KiB pages for fine-grained mapping.
static uint32_t* const page_directory = (uint32_t*)0x70000;

static bool vmm_paging_on  = false;
static bool vmm_long_mode  = false;
static bool vmm_our_paging = false;  // true only when WE set up paging (BIOS path)

static bool vmm_check_long_mode(){
    // EFER MSR (0xC0000080), bit 8 = LME (in EAX, not EDX)
    uint32_t eax, edx;
    __asm__ __volatile__("rdmsr":"=a"(eax),"=d"(edx):"c"(0xC0000080u));
    return (eax & (1u << 8)) != 0;
}

static void vmm_init(){
    vmm_long_mode = vmm_check_long_mode();
    vmm_paging_on = (read_cr0() & 0x80000000ull) != 0;

    if (vmm_long_mode){
        // 64-bit kernel: we are in long mode with 4-level identity-mapped paging
        // set up by switch32to64.asm (BIOS) or UEFI firmware.
        serial_puts("[VMM] Long mode – 4-level paging in use\n");
        return;
    }
    if (vmm_paging_on){
        // Firmware's 32-bit paging is active. Use the existing identity mapping.
        serial_puts("[VMM] Firmware paging active (not our own)\n");
        return;
    }

    // ---- 32-bit BIOS path: set up 32-bit paging with 4 MiB PSE ----
    // (only reached if somehow not in long mode and no firmware paging)
    serial_puts("[VMM] Setting up 32-bit paging (PSE)...\n");

    // Clear page directory
    for (int i = 0; i < 1024; i++) page_directory[i] = 0;

    // Identity-map first 32 MiB with 4 MiB pages (covers kernel + 16 MiB heap)
    for (int i = 0; i < 8; i++)
        page_directory[i] = (i * 0x400000u) | PG_PRESENT | PG_RW | PG_PSE;

    // Enable PSE (CR4.PSE = bit 4)
    uint64_t cr4 = read_cr4();
    write_cr4(cr4 | 0x10);

    // Load page directory into CR3
    write_cr3(0x70000);

    // Enable paging (CR0.PG = bit 31)
    uint64_t cr0 = read_cr0();
    write_cr0(cr0 | 0x80000000ull);

    vmm_paging_on  = true;
    vmm_our_paging = true;
    serial_puts("[VMM] Paging enabled (32 MiB identity-mapped, 4 MiB PSE)\n");
}

// Split a 4 MiB PDE into 1024 × 4 KiB pages (transparent: all identity-mapped)
static bool vmm_split_4mb(uint32_t pd_idx){
    uint32_t pde = page_directory[pd_idx];
    if (!(pde & PG_PSE)) return true;          // already a PT
    if (!(pde & PG_PRESENT)) return false;

    // Allocate a 4 KiB page table from PMM (identity-mapped, so accessible)
    uint32_t pt_phys = pmm_alloc_page();
    if (pt_phys == 0) return false;

    uint32_t* pt = (uint32_t*)pt_phys;
    uint32_t base = pd_idx << 22;               // 4 MiB base physical addr
    for (int i = 0; i < 1024; i++)
        pt[i] = (base + i * PAGE_SIZE) | PG_PRESENT | PG_RW;

    page_directory[pd_idx] = pt_phys | PG_PRESENT | PG_RW;
    return true;
}

// Map a 4 KiB virtual page to a physical page
static bool vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags){
    if (!vmm_our_paging) return false;  // can't modify firmware page tables

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    // Ensure we have a 4 KiB page table (split if 4 MiB PSE entry)
    uint32_t pde = page_directory[pd_idx];
    if (pde & PG_PSE){
        if (!vmm_split_4mb(pd_idx)) return false;
        pde = page_directory[pd_idx];
    }
    if (!(pde & PG_PRESENT)){
        uint32_t pt_phys = pmm_alloc_page();
        if (pt_phys == 0) return false;
        memset_((void*)pt_phys, 0, PAGE_SIZE);
        page_directory[pd_idx] = pt_phys | PG_PRESENT | PG_RW;
        pde = page_directory[pd_idx];
    }

    uint32_t* pt = (uint32_t*)(pde & 0xFFFFF000u);
    pt[pt_idx] = (phys & 0xFFFFF000u) | (flags & 0xFFF) | PG_PRESENT;
    invlpg(virt);
    return true;
}

// Translate virtual → physical (returns 0 if not mapped)
static uint32_t vmm_get_phys(uint32_t virt){
    if (!vmm_paging_on) return virt;  // identity (no paging)
    if (vmm_long_mode)  return virt;  // long mode: identity-mapped

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;
    uint32_t pde = page_directory[pd_idx];

    if (!(pde & PG_PRESENT)) return 0;
    if (pde & PG_PSE){
        // 4 MiB page – offset within the 4 MiB block
        return (pde & 0xFFC00000u) + (virt & 0x3FFFFF);
    }
    uint32_t* pt = (uint32_t*)(pde & 0xFFFFF000u);
    uint32_t pte = pt[pt_idx];
    if (!(pte & PG_PRESENT)) return 0;
    return (pte & 0xFFFFF000u) + (virt & 0xFFF);
}

// =====================================================================
//  Kernel Heap (kmalloc / kfree)
// =====================================================================
//  Simple linked-list first-fit allocator on a fixed 1 MiB region.
struct HeapBlock {
    uint32_t magic;       // 0xDEADBEEF = allocated, 0xFEEDFACE = free
    uint32_t size;        // user data size (excludes header, includes padding)
    HeapBlock* next;
    HeapBlock* prev;
};

constexpr uint32_t HEAP_MAGIC_ALLOC = 0xDEADBEEFu;
constexpr uint32_t HEAP_MAGIC_FREE  = 0xFEEDFACEu;
constexpr uint32_t HEAP_ALIGN       = 8;

static HeapBlock* heap_head = nullptr;
static uint32_t   heap_alloc_count   = 0;
static uint32_t   heap_free_count    = 0;
static uint32_t   heap_bytes_alloc   = 0;
static uint32_t   heap_bytes_freed   = 0;

static void heap_init(){
    // The 16 MiB region at HEAP_START is identity-mapped (first 32 MiB)
    heap_head = (HeapBlock*)HEAP_START;
    heap_head->magic = HEAP_MAGIC_FREE;
    heap_head->size  = HEAP_SIZE - sizeof(HeapBlock);
    heap_head->next  = nullptr;
    heap_head->prev  = nullptr;
    heap_alloc_count = 0;
    heap_free_count  = 0;
    heap_bytes_alloc = 0;
    heap_bytes_freed = 0;
    serial_puts("[HEAP] Initialised: 16 MiB at 0x200000\n");
}

extern "C" void* kmalloc(uint32_t size){
    if (size == 0) return nullptr;

    // Align to 8 bytes
    size = (size + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);

    HeapBlock* blk = heap_head;
    while (blk){
        if (blk->magic == HEAP_MAGIC_FREE && blk->size >= size + sizeof(HeapBlock)){
            // Split if there's room for another block
            if (blk->size >= size + sizeof(HeapBlock) + HEAP_ALIGN){
                HeapBlock* new_blk = (HeapBlock*)((uint8_t*)blk + sizeof(HeapBlock) + size);
                new_blk->magic = HEAP_MAGIC_FREE;
                new_blk->size  = blk->size - size - sizeof(HeapBlock);
                new_blk->next  = blk->next;
                new_blk->prev  = blk;
                if (blk->next) blk->next->prev = new_blk;
                blk->next = new_blk;
                blk->size = size;
            }
            blk->magic = HEAP_MAGIC_ALLOC;
            heap_alloc_count++;
            heap_bytes_alloc += size;
            return (uint8_t*)blk + sizeof(HeapBlock);
        }
        blk = blk->next;
    }
    return nullptr;  // OOM
}

extern "C" void kfree(void* ptr){
    if (!ptr) return;
    HeapBlock* blk = (HeapBlock*)((uint8_t*)ptr - sizeof(HeapBlock));

    if (blk->magic != HEAP_MAGIC_ALLOC) return;  // invalid or double-free

    blk->magic = HEAP_MAGIC_FREE;
    heap_free_count++;
    heap_bytes_freed += blk->size;

    // Coalesce with next block if free
    if (blk->next && blk->next->magic == HEAP_MAGIC_FREE){
        HeapBlock* nb = blk->next;
        blk->size += sizeof(HeapBlock) + nb->size;
        blk->next = nb->next;
        if (nb->next) nb->next->prev = blk;
    }
    // Coalesce with previous block if free
    if (blk->prev && blk->prev->magic == HEAP_MAGIC_FREE){
        HeapBlock* pb = blk->prev;
        pb->size += sizeof(HeapBlock) + blk->size;
        pb->next = blk->next;
        if (blk->next) blk->next->prev = pb;
    }
}

// =====================================================================
//  Command history (persistent via save/load)
// =====================================================================
constexpr int CMD_FILE_LBA     = 300;  // moved from 256 to avoid kernel overlap
constexpr int CMD_FILE_SECTORS = 4;
constexpr int HIST_MAX         = 64;
constexpr int HIST_LEN         = 80;

static char g_hist[HIST_MAX][HIST_LEN];
static int  g_hist_count=0;
static uint8_t g_diskbuf[CMD_FILE_SECTORS*512];

static void hist_add(const char* s){
    if(g_hist_count<HIST_MAX){
        int i=0; while(s[i] && i<HIST_LEN-1){ g_hist[g_hist_count][i]=s[i]; i++; }
        g_hist[g_hist_count][i]=0;
        g_hist_count++;
    }
}

// =====================================================================
//  Shell command implementations
// =====================================================================

// --- Memory management commands ---
static void cmd_meminfo(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("=== Memory Information ===\n\n");
    term.set_color(make_color(WHITE, BLACK));

    // PMM stats
    term.write("Physical Memory Manager:\n");
    term.write("  Total RAM:     "); term.write_dec((int)pmm_mem_kb); term.write(" KiB (");
    term.write_dec((int)(pmm_mem_kb / 1024)); term.write(" MiB)\n");
    term.write("  Managed pages: "); term.write_dec((int)pmm_total_pages);
    term.write(" ("); term.write_dec((int)(pmm_total_pages * PAGE_SIZE / 1024)); term.write(" KiB)\n");
    term.write("  Free pages:    "); term.write_dec((int)pmm_free_pages);
    term.write(" ("); term.write_dec((int)(pmm_free_pages * PAGE_SIZE / 1024)); term.write(" KiB)\n");
    term.write("  Used pages:    "); term.write_dec((int)pmm_used_pages);
    term.write(" ("); term.write_dec((int)(pmm_used_pages * PAGE_SIZE / 1024)); term.write(" KiB)\n");
    term.write("  PMM base:      "); term.write_hex(PMM_BASE_ADDR); term.write("\n\n");

    // VMM stats
    term.write("Virtual Memory Manager:\n");
    term.write("  Paging:        ");
    if (vmm_paging_on) { term.set_color(make_color(GREEN, BLACK)); term.write("ENABLED\n"); }
    else               { term.set_color(make_color(BROWN, BLACK)); term.write("DISABLED\n"); }
    term.set_color(make_color(WHITE, BLACK));
    term.write("  Mode:          ");
    if (vmm_long_mode)        term.write("Long mode (64-bit 4-level paging)\n");
    else if (vmm_our_paging)  term.write("32-bit (our PSE 4 MiB paging)\n");
    else                      term.write("32-bit (firmware paging)\n");
    term.write("  Page directory:"); term.write_hex((uintptr_t)page_directory); term.write("\n");
    term.write("  CR0:           "); term.write_hex(read_cr0()); term.write("\n");
    term.write("  CR3:           "); term.write_hex(read_cr3()); term.write("\n");
    term.write("  CR4:           "); term.write_hex(read_cr4()); term.write("\n\n");

    // Heap stats
    term.write("Kernel Heap:\n");
    term.write("  Region:        "); term.write_hex(HEAP_START);
    term.write(" - "); term.write_hex(HEAP_END);
    term.write(" ("); term.write_dec(HEAP_SIZE / 1024); term.write(" KiB)\n");
    term.write("  Allocations:   "); term.write_dec((int)heap_alloc_count); term.write("\n");
    term.write("  Frees:         "); term.write_dec((int)heap_free_count); term.write("\n");
    term.write("  Bytes alloc:   "); term.write_dec((int)heap_bytes_alloc); term.write("\n");
    term.write("  Bytes freed:   "); term.write_dec((int)heap_bytes_freed); term.write("\n");
    int in_use = (int)heap_bytes_alloc - (int)heap_bytes_freed;
    term.write("  In use:        "); term.write_dec(in_use); term.write(" bytes\n\n");

    // Memory map summary
    term.write("Memory Map:\n");
    term.write("  0x000000 - 0x000FFF  Real-mode IVT + BDA\n");
    term.write("  0x007C00 - 0x007DFF  Boot sector\n");
    term.write("  0x010000 - 0x027FFF  Kernel image (~92 KiB)\n");
    term.write("  0x070000 - 0x07FFFF  Page directory + page tables\n");
    term.write("  0x080000 - 0x081FFF  PMM bitmap (8 KiB)\n");
    term.write("  0x090000 - 0x09FFFF  Kernel stack\n");
    term.write("  0x0B8000 - 0x0BFFFF  VGA text buffer\n");
    term.write("  0x100000 - 0x1FFFFF  Free (PMM-managed)\n");
    term.write("  0x200000 - 0x2FFFFF  Kernel heap (1 MiB)\n");
    term.write("  0x300000+            Free (PMM-managed)\n");
}

static void cmd_memtest(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("=== Memory Allocation Test ===\n\n");
    term.set_color(make_color(WHITE, BLACK));

    // Test 1: kmalloc/kfree
    term.write("[1] kmalloc/kfree test:\n");
    void* p1 = kmalloc(64);
    void* p2 = kmalloc(256);
    void* p3 = kmalloc(1024);
    term.write("  kmalloc(64)   = "); term.write_hex((uintptr_t)p1); term.write("\n");
    term.write("  kmalloc(256)  = "); term.write_hex((uintptr_t)p2); term.write("\n");
    term.write("  kmalloc(1024) = "); term.write_hex((uintptr_t)p3); term.write("\n");

    // Write test patterns
    if (p1) { memset_(p1, 0xAA, 64); }
    if (p2) { memset_(p2, 0xBB, 256); }
    if (p3) { memset_(p3, 0xCC, 1024); }

    // Verify
    bool ok = true;
    if (p1){ for(int i=0;i<64;i++)  if(((uint8_t*)p1)[i]!=0xAA) ok=false; }
    if (p2){ for(int i=0;i<256;i++) if(((uint8_t*)p2)[i]!=0xBB) ok=false; }
    if (p3){ for(int i=0;i<1024;i++)if(((uint8_t*)p3)[i]!=0xCC) ok=false; }
    term.write("  Pattern verify: ");
    term.set_color(ok ? make_color(GREEN,BLACK) : make_color(RED,BLACK));
    term.write(ok ? "PASS\n" : "FAIL\n");
    term.set_color(make_color(WHITE, BLACK));

    kfree(p1);
    kfree(p2);
    term.write("  kfree(p1), kfree(p2) done\n");

    // Allocate again to test reuse
    void* p4 = kmalloc(64);
    term.write("  kmalloc(64)   = "); term.write_hex((uintptr_t)p4);
    term.write(" (should reuse freed block)\n");
    kfree(p3);
    kfree(p4);
    term.write("  kfree(p3), kfree(p4) done\n");
    term.write("  Allocs: "); term.write_dec((int)heap_alloc_count);
    term.write("  Frees: "); term.write_dec((int)heap_free_count); term.write("\n\n");

    // Test 2: PMM
    term.write("[2] PMM page allocation test:\n");
    uint32_t pages[5];
    for (int i = 0; i < 5; i++){
        pages[i] = pmm_alloc_page();
        term.write("  pmm_alloc_page() = "); term.write_hex(pages[i]); term.write("\n");
    }
    for (int i = 0; i < 5; i++){
        if (pages[i]) pmm_free_page(pages[i]);
    }
    term.write("  All 5 pages freed\n");
    term.write("  Free pages: "); term.write_dec((int)pmm_free_pages); term.write("\n\n");

    // Test 3: Large allocation
    term.write("[3] Large allocation test (100 KiB):\n");
    void* big = kmalloc(100 * 1024);
    term.write("  kmalloc(102400) = "); term.write_hex((uintptr_t)big); term.write("\n");
    if (big){
        memset_(big, 0x42, 100 * 1024);
        bool big_ok = true;
        for (int i = 0; i < 100 * 1024; i++)
            if (((uint8_t*)big)[i] != 0x42) { big_ok = false; break; }
        term.write("  Verify 100 KiB: ");
        term.set_color(big_ok ? make_color(GREEN,BLACK) : make_color(RED,BLACK));
        term.write(big_ok ? "PASS\n" : "FAIL\n");
        term.set_color(make_color(WHITE, BLACK));
        kfree(big);
        term.write("  Freed\n");
    }
    term.write("\n");
}

static void cmd_pagetest(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("=== Virtual Memory Page Test ===\n\n");
    term.set_color(make_color(WHITE, BLACK));

    if (!vmm_our_paging){
        term.write("Page mapping test requires 32-bit PSE paging.\n");
        term.write("In 64-bit long mode, firmware/switcher paging is used.\n\n");
        return;
    }

    // Show current identity mapping
    term.write("[1] Identity mapping check:\n");
    uint32_t test_addr = 0x100000;  // 1 MiB
    uint32_t phys = vmm_get_phys(test_addr);
    term.write("  vmm_get_phys("); term.write_hex(test_addr);
    term.write(") = "); term.write_hex(phys);
    term.write(phys == test_addr ? " (identity OK)\n" : " (MISMATCH!)\n");

    test_addr = 0x200000;  // 2 MiB (heap)
    phys = vmm_get_phys(test_addr);
    term.write("  vmm_get_phys("); term.write_hex(test_addr);
    term.write(") = "); term.write_hex(phys);
    term.write(phys == test_addr ? " (identity OK)\n" : " (MISMATCH!)\n\n");

    // Map a physical page to a higher virtual address
    term.write("[2] Page mapping test (4 KiB):\n");
    uint32_t vaddr = 0x40000000;  // 1 GiB virtual
    uint32_t paddr = pmm_alloc_page();
    if (paddr == 0){
        term.write("  PMM out of memory!\n\n");
        return;
    }
    term.write("  Physical page: "); term.write_hex(paddr); term.write("\n");
    term.write("  Virtual addr:  "); term.write_hex(vaddr); term.write("\n");

    // Write pattern to physical page (via identity mapping)
    uint8_t* phys_ptr = (uint8_t*)paddr;
    for (int i = 0; i < 256; i++) phys_ptr[i] = (uint8_t)(i & 0xFF);

    // Map virtual to physical
    bool mapped = vmm_map_page(vaddr, paddr, PG_PRESENT | PG_RW);
    term.write("  vmm_map_page:  ");
    term.set_color(mapped ? make_color(GREEN,BLACK) : make_color(RED,BLACK));
    term.write(mapped ? "OK\n" : "FAILED\n");
    term.set_color(make_color(WHITE, BLACK));

    if (mapped){
        // Verify translation
        phys = vmm_get_phys(vaddr);
        term.write("  vmm_get_phys("); term.write_hex(vaddr);
        term.write(") = "); term.write_hex(phys); term.write("\n");

        // Read through virtual address
        uint8_t* virt_ptr = (uint8_t*)vaddr;
        bool vok = true;
        for (int i = 0; i < 256; i++)
            if (virt_ptr[i] != (uint8_t)(i & 0xFF)) { vok = false; break; }
        term.write("  Read via virtual: ");
        term.set_color(vok ? make_color(GREEN,BLACK) : make_color(RED,BLACK));
        term.write(vok ? "PASS\n" : "FAIL\n");
        term.set_color(make_color(WHITE, BLACK));

        // Write via virtual, read via physical
        virt_ptr[0] = 0x99;
        term.write("  Write 0x99 via virtual, read via physical: ");
        term.set_color(phys_ptr[0] == 0x99 ? make_color(GREEN,BLACK) : make_color(RED,BLACK));
        term.write(phys_ptr[0] == 0x99 ? "PASS\n" : "FAIL\n");
        term.set_color(make_color(WHITE, BLACK));
    }

    // Page directory dump (first 4 entries) - only for 32-bit paging
    if (!vmm_long_mode) {
        term.write("\n[3] Page directory (first 4 entries):\n");
        for (int i = 0; i < 4; i++){
            uint32_t pde = page_directory[i];
            term.write("  PDE["); term.write_dec(i); term.write("] = ");
            term.write_hex(pde);
            if (pde & PG_PRESENT){
                if (pde & PG_PSE) term.write("  4MiB page @ ");
                else              term.write("  PT @ ");
                term.write_hex(pde & 0xFFFFF000u);
            } else {
                term.write("  (not present)");
            }
            term.write("\n");
        }
    } else {
        term.write("\n[3] Page tables: 4-level paging (long mode)\n");
        term.write("  CR3 (PML4): "); term.write_hex(read_cr3()); term.write("\n");
    }
    term.write("\n");

    pmm_free_page(paddr);
}

static void cmd_help(){
    term.write("Commands:\n");
    term.write("  help        Show this help\n");
    term.write("  echo <text> Print text\n");
    term.write("  clear, cls  Clear screen & history\n");
    term.write("  about       System info\n");
    term.write("  history, h  Show command history\n");
    term.write("  save        Save history to disk (LBA ");
    term.write_dec(CMD_FILE_LBA); term.write(")\n");
    term.write("  load        Load history from disk\n");
    term.write("\nMKFS (custom file system with dirs):\n");
    term.write("  mkfs        Format MKFS file system\n");
    term.write("  ls, dir     List files in current dir\n");
    term.write("  cat, type   Print file from MKFS\n");
    term.write("  touch       Create empty file\n");
    term.write("  write <f>   Write text to file (empty line to save)\n");
    term.write("  rm, del     Delete file or empty dir\n");
    term.write("  copy <s> <d> Copy file (src -> dst)\n");
    term.write("  mkdir, md   Create directory\n");
    term.write("  cd <d>      Change directory (supports / and \\)\n");
    term.write("  pwd         Show current path\n");
    term.write("\nSFS (compatible read-only file system):\n");
    term.write("  lsfs        List files on SFS\n");
    term.write("  catfs <f>   Print file from SFS\n");
    term.write("\nDisk partitions (Windows-compatible):\n");
    term.write("  part        Show MBR partition table\n");
    term.write("  mount <n>   Mount FAT32 partition (1-4)\n");
    term.write("  lsfat       List files on mounted FAT32\n");
    term.write("  fatinfo     Show mounted FAT32 info\n");
    term.write("\nScript execution:\n");
    term.write("  run <f>     Run .sh script from MKFS\n");
    term.write("  runfs <f>   Run .sh script from SFS\n");
    term.write("\nAI engine:\n");
    term.write("  ai init     Initialize AI engine (Markov + Transformer)\n");
    term.write("  ai info     Show AI engine status\n");
    term.write("  ai mode     Switch mode (markov/transformer)\n");
    term.write("  ai test     Test transformer forward pass\n");
    term.write("  ai cleanup  Free AI engine resources\n");
    term.write("  generate <p> Generate text from prompt\n");
    term.write("  agent init  Initialize multi-agent framework\n");
    term.write("  agent run <g> Run agent pipeline (Planner->Actor->Critic)\n");
    term.write("  agent status Show agent framework status\n");
    term.write("\nNetwork (HTTP server on port 8080):\n");
    term.write("  netstart    Initialize NE2000 NIC and HTTP server\n");
    term.write("  netinfo     Show network status (IP, MAC, connections)\n");
    term.write("  netstat     Alias for netinfo\n");
    term.write("  Web UI:     http://10.0.2.15:8080 (from host: localhost:8080)\n");
    term.write("\nKernel switching:\n");
    term.write("  switch      Switch to 32-bit kernel (loads from LBA 33)\n");
    term.write("\nGUI:\n");
    term.write("  gui         Enter graphical desktop (VBE framebuffer)\n");
    term.write("  ESC         Exit GUI mode and return to text\n");
    term.write("  Mouse       Move cursor, drag title bar, click [X] to close\n");
    term.write("\nMemory management:\n");
    term.write("  meminfo     Show memory/PMM/VMM/heap stats\n");
    term.write("  memtest     Run kmalloc/kfree + PMM tests\n");
    term.write("  pagetest    Test virtual memory page mapping\n");
    term.write("\nPower management:\n");
    term.write("  shutdown    Power off (ACPI)\n");
    term.write("  reboot      Restart system\n");
    term.write("\nKeyboard shortcuts:\n");
    term.write("  Tab         Auto-complete command/filename\n");
    term.write("  Left/Right  Move cursor in input line\n");
    term.write("  Home/End    Jump to start/end of input line\n");
    term.write("  Up/Dn       Recall command history (at prompt)\n");
    term.write("  PgUp/PgDn   Scroll terminal view up/down\n");
    term.write("  Ctrl+C      Abort input / copy selected text\n");
    term.write("  Ctrl+V      Paste clipboard   Ctrl+L: refocus\n");
    term.write("  Ctrl+Up/Dn  Cycle clipboard history\n");
    term.write("\nMouse: wheel=scroll, drag=select, click=refocus\n");
}

static void cmd_about(){
    term.write("MiniOS v4.0  -  C++ freestanding kernel (64-bit)\n");
    term.write("64-bit long mode kernel with 4-level paging.\n");
    term.write("PS/2 keyboard + mouse, ATA disk, VGA 80x25.\n");
    term.write("MKFS+SFS+FAT32, dirs, .sh, Tab completion.\n");
    term.write("PMM (bitmap) + VMM (x86 paging) + kmalloc/kfree + AI engine.\n");
}

// Build full path string for prompt (PowerShell-style "PS /path>")
static void build_prompt_path(char* buf, int maxlen){
    if (g_cwd == FS_ROOT_PARENT || !mkfs.mounted) {
        buf[0] = '/'; buf[1] = 0;
        return;
    }
    // Walk up parent chain
    uint16_t path[16];
    int depth = 0;
    uint16_t cur = g_cwd;
    while (cur != FS_ROOT_PARENT && depth < 16) {
        path[depth++] = cur;
        // Read entry to get parent
        int s = cur / FS_ENTRY_PER_SEC;
        int e = cur % FS_ENTRY_PER_SEC;
        ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        cur = fe->parent;
    }
    int pos = 0;
    buf[pos++] = '/';
    for (int i = depth - 1; i >= 0 && pos < maxlen - 1; i--) {
        int s = path[i] / FS_ENTRY_PER_SEC;
        int e = path[i] % FS_ENTRY_PER_SEC;
        ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        int j = 0;
        while (fe->name[j] && pos < maxlen - 2) {
            buf[pos++] = fe->name[j++];
        }
        if (i > 0 && pos < maxlen - 1)
            buf[pos++] = '/';
    }
    buf[pos] = 0;
}

static void cmd_history(){
    term.write("Command history ("); term.write_dec(g_hist_count); term.write("):\n");
    for(int i=0;i<g_hist_count;i++){
        term.write("  "); term.write_dec(i+1); term.write(". ");
        term.write(g_hist[i]); term.put_char('\n');
    }
}

static void cmd_save(){
    memset_(g_diskbuf,0,sizeof(g_diskbuf));
    g_diskbuf[0]='K'; g_diskbuf[1]='C'; g_diskbuf[2]='M'; g_diskbuf[3]='D';
    g_diskbuf[4]=(uint8_t)(g_hist_count&0xFF);
    g_diskbuf[5]=(uint8_t)((g_hist_count>>8)&0xFF);
    int off=8;
    for(int i=0;i<g_hist_count && off<(int)sizeof(g_diskbuf)-2;i++){
        int L=strlen_(g_hist[i]);
        if(off+1+L >= (int)sizeof(g_diskbuf)) break;
        g_diskbuf[off++]=(uint8_t)L;
        memcpy_(g_diskbuf+off, g_hist[i], L);
        off+=L;
    }
    for(int s=0;s<CMD_FILE_SECTORS;s++)
        ata_write_sector(CMD_FILE_LBA+s, (const uint16_t*)(g_diskbuf+s*512));
    term.write("Saved "); term.write_dec(g_hist_count);
    term.write(" commands to disk (LBA "); term.write_dec(CMD_FILE_LBA); term.write(").\n");
}

static void cmd_load(){
    for(int s=0;s<CMD_FILE_SECTORS;s++)
        ata_read_sector(CMD_FILE_LBA+s, (uint16_t*)(g_diskbuf+s*512));
    if(!(g_diskbuf[0]=='K'&&g_diskbuf[1]=='C'&&g_diskbuf[2]=='M'&&g_diskbuf[3]=='D')){
        term.write("No command file found on disk.\n");
        return;
    }
    int count=g_diskbuf[4] | (g_diskbuf[5]<<8);
    g_hist_count=0;
    int off=8;
    for(int i=0;i<count && off<(int)sizeof(g_diskbuf)-1;i++){
        int L=g_diskbuf[off++];
        if(L>=HIST_LEN) L=HIST_LEN-1;
        if(off+L>(int)sizeof(g_diskbuf)) break;
        memcpy_(g_hist[g_hist_count], g_diskbuf+off, L);
        g_hist[g_hist_count][L]=0;
        g_hist_count++;
        off+=L;
    }
    term.write("Loaded "); term.write_dec(g_hist_count);
    term.write(" commands from disk.\n");
}

// ----- MKFS commands -----
static void cmd_mkfs(){
    mkfs.format();
    g_cwd = FS_ROOT_PARENT;  // reset to root
    term.write("MKFS formatted. Data area: LBA ");
    term.write_dec(MKFS_DATA_LBA); term.write("-");
    term.write_dec(MKFS_DATA_LBA + MKFS_DATA_SECTORS - 1);
    term.write(" ("); term.write_dec(MKFS_DATA_SECTORS * 512 / 1024);
    term.write(" KB)\n");
}

static void cmd_ls(){
    mkfs.ls();
}

static void cmd_cat(const char* name){
    if (!name[0]) { term.write("Usage: cat <filename>\n"); return; }
    int ret = mkfs.read(name, g_iobuf, FS_IOBUF_SIZE);
    if (ret < 0) {
        term.write("File not found: "); term.write(name); term.put_char('\n');
        return;
    }
    g_iobuf[ret] = 0;
    term.write((const char*)g_iobuf);
    if (ret > 0 && g_iobuf[ret-1] != '\n') term.put_char('\n');
}

static void cmd_touch(const char* name){
    if (!name[0]) { term.write("Usage: touch <filename>\n"); return; }
    char buf[FS_NAME_LEN];
    int i=0; while(name[i] && i<FS_NAME_LEN-1){ buf[i]=name[i]; i++; }
    buf[i]=0;
    int ret = mkfs.create(buf, (const uint8_t*)"", 0);
    if (ret >= 0) {
        term.write("Created: "); term.write(buf); term.put_char('\n');
    } else {
        term.write("Failed (code "); term.write_dec(ret); term.write(")\n");
    }
}

static void cmd_rm(const char* name){
    if (!name[0]) { term.write("Usage: rm <filename>\n"); return; }
    int ret = mkfs.remove(name);
    if (ret >= 0) {
        term.write("Removed: "); term.write(name); term.put_char('\n');
    } else if (ret == -3) {
        term.write("Directory not empty: "); term.write(name); term.put_char('\n');
    } else {
        term.write("File not found: "); term.write(name); term.put_char('\n');
    }
}

static void cmd_copy(const char* args){
    // Parse "src dst" from args
    char src[FS_NAME_LEN], dst[FS_NAME_LEN];
    int i=0, j=0;
    while(args[i] && args[i]!=' ' && j<FS_NAME_LEN-1) src[j++]=args[i++];
    src[j]=0;
    while(args[i]==' ') i++;
    j=0;
    while(args[i] && args[i]!=' ' && j<FS_NAME_LEN-1) dst[j++]=args[i++];
    dst[j]=0;
    if(!src[0] || !dst[0]){
        term.write("Usage: copy <src> <dst>\n");
        return;
    }
    int ret = mkfs.copy(src, dst);
    if(ret >= 0){
        term.write("Copied: "); term.write(src);
        term.write(" -> "); term.write(dst);
        term.write(" ("); term.write_dec(ret); term.write(" bytes)\n");
    } else if(ret == -2){
        term.write("Source not found: "); term.write(src); term.put_char('\n');
    } else {
        term.write("Copy failed (code "); term.write_dec(ret); term.write(")\n");
    }
}

static void cmd_write(const char* name){
    if (!name[0]) { term.write("Usage: write <filename>\n"); return; }
    int i=0; while(name[i] && i<FS_NAME_LEN-1){ g_write_name[i]=name[i]; i++; }
    g_write_name[i]=0;
    g_write_len=0;
    g_mode=MODE_WRITE;
    term.write("Writing to: "); term.write(g_write_name);
    term.write("\nEnter text (empty line to save, max 8KB):\n");
}

static void cmd_mkdir(const char* name){
    if (!name[0]) { term.write("Usage: mkdir <dirname>\n"); return; }
    int ret = mkfs.mkdir(name);
    if (ret >= 0) {
        term.write("Created dir: "); term.write(name); term.put_char('\n');
    } else if (ret == -2) {
        term.write("Already exists: "); term.write(name); term.put_char('\n');
    } else {
        term.write("Failed (code "); term.write_dec(ret); term.write(")\n");
    }
}

static void cmd_cd(const char* name){
    char path[FS_NAME_LEN * 2];
    int i=0;
    while(name[i] && i < (int)sizeof(path)-1){ path[i]=name[i]; i++; }
    path[i]=0;
    normalize_path(path);  // convert \ to /

    int ret = mkfs.cd(path);
    if (ret < 0) {
        if (ret == -2) term.write("Directory not found: ");
        else if (ret == -3) term.write("Not a directory: ");
        else term.write("cd failed: ");
        term.write(path); term.put_char('\n');
    }
}

static void cmd_pwd(){
    mkfs.pwd();
}

// ----- SFS commands -----
static void cmd_lsfs(){
    sfs.ls();
}

static void cmd_catfs(const char* name){
    if (!name[0]) { term.write("Usage: catfs <filename>\n"); return; }
    int ret = sfs.read(name, g_iobuf, FS_IOBUF_SIZE);
    if (ret < 0) {
        term.write("File not found: "); term.write(name); term.put_char('\n');
        return;
    }
    g_iobuf[ret] = 0;
    term.write((const char*)g_iobuf);
    if (ret > 0 && g_iobuf[ret-1] != '\n') term.put_char('\n');
}

// ----- Partition commands -----
static void cmd_part(){
    ata_read_sector(0, (uint16_t*)g_fsbuf);
    uint8_t* mbr = g_fsbuf;

    term.write("MBR partition table (LBA 0):\n");

    // Check MBR signature
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        term.write("  No valid MBR signature (0x55AA) found.\n");
        term.write("  This disk uses a raw layout (no partition table).\n");
    } else {
        term.write("  Signature: 0x55AA (valid)\n");
        MbrPartition* parts = (MbrPartition*)(mbr + 446);
        int found = 0;
        for (int i = 0; i < 4; i++) {
            MbrPartition* p = &parts[i];
            if (p->type == 0) continue;
            found++;
            term.write("\n  Partition "); term.write_dec(i + 1);
            term.write(p->boot_flag & 0x80 ? " [BOOT]" : "       ");
            term.write("\n    Type:        "); term.write_hex(p->type);
            term.write(" ("); term.write(part_type_name(p->type)); term.write(")\n");
            term.write("    Start LBA:   "); term.write_dec((int)p->start_lba); term.write("\n");
            term.write("    Sectors:     "); term.write_dec((int)p->total_sectors); term.write("\n");
            term.write("    Size:        ");
            term.write_dec((int)(p->total_sectors / 2)); term.write(" KB (");
            term.write_dec((int)(p->total_sectors / 2048)); term.write(" MB)\n");

            // If FAT type, try to read BPB
            if (is_fat_type(p->type)) {
                ata_read_sector(p->start_lba, (uint16_t*)g_iobuf);
                if (g_iobuf[510] == 0x55 && g_iobuf[511] == 0xAA) {
                    uint16_t bps = *(uint16_t*)(g_iobuf + 11);
                    uint8_t spc = g_iobuf[13];
                    uint16_t root_entries = *(uint16_t*)(g_iobuf + 17);
                    term.write("    FAT BPB:     ");
                    term.write_dec((int)bps); term.write(" bytes/sec, ");
                    term.write_dec((int)spc); term.write(" sec/cluster");
                    if (root_entries == 0) term.write(" (FAT32)");
                    else { term.write(" (FAT16, "); term.write_dec((int)root_entries); term.write(" root entries)"); }
                    term.write("\n");
                    // Volume label
                    term.write("    Volume:      ");
                    for (int j = 0; j < 11; j++) {
                        char c = (char)g_iobuf[71 + j];
                        if (c >= 0x20) term.put_char(c);
                    }
                    term.put_char('\n');
                }
            }
        }
        if (found == 0)
            term.write("\n  No partitions defined (all entries empty).\n");
    }

    // Show MiniOS disk layout
    term.write("\nMiniOS disk layout:\n");
    term.write("  LBA 0:        MBR / Boot sector\n");
    term.write("  LBA 1-32:     Stage2 bootloader\n");
    term.write("  LBA 33+:      C++ kernel (up to 128 KiB)\n");
    term.write("  LBA 300-303:  Command history (save/load)\n");
    term.write("  LBA 512-799:  MKFS file system\n");
    term.write("  LBA 800-1023: SFS file system\n");
}

static void cmd_mount(const char* arg){
    if (!arg[0]) { term.write("Usage: mount <partition_number 1-4>\n"); return; }

    int part_num = 0;
    const char* p = arg;
    while(*p >= '0' && *p <= '9'){ part_num = part_num * 10 + (*p - '0'); p++; }
    if (part_num < 1 || part_num > 4) {
        term.write("Partition number must be 1-4\n");
        return;
    }

    MbrPartition mp;
    memset_(&mp, 0, sizeof(mp));
    uint32_t start = read_mbr_partition(part_num, &mp);
    if (start == 0 || mp.type == 0) {
        term.write("Partition "); term.write_dec(part_num);
        term.write(" does not exist\n");
        return;
    }

    term.write("Mounting partition "); term.write_dec(part_num);
    term.write(" (type "); term.write_hex(mp.type);
    term.write(": "); term.write(part_type_name(mp.type)); term.write(")\n");
    term.write("  Start LBA: "); term.write_dec((int)start); term.write("\n");

    if (!is_fat_type(mp.type)) {
        term.write("  Not a FAT partition (type ");
        term.write_hex(mp.type); term.write(").\n");
        term.write("  NTFS partitions cannot be read (read-only kernel).\n");
        if (fat32.mounted) {
            term.write("  Previous FAT32 mount unmounted.\n");
            fat32.mounted = false;
        }
        return;
    }

    if (fat32.mount(start)) {
        term.write("  FAT32 mounted successfully!\n");
        term.write("  Use 'lsfat' to list files, 'fatinfo' for details.\n");
    } else {
        term.write("  FAT32 mount failed (not FAT32 or invalid BPB).\n");
        term.write("  Note: FAT16 partitions are not supported yet.\n");
    }
}

static void cmd_lsfat(){
    fat32.ls();
}

static void cmd_fatinfo(){
    fat32.info();
}

// ----- Script runner -----
static bool g_in_script = false;
static void run_command(const char* line);  // forward declaration

static void run_script(const char* content, int size){
    char line[HIST_LEN];
    int linepos = 0;
    g_in_script = true;
    for (int i = 0; i < size; i++) {
        char c = content[i];
        if (c == '\n') {
            line[linepos] = 0;
            const char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != 0 && *p != '#') {
                term.set_color(make_color(YELLOW, BLACK));
                term.write("> "); term.write(p); term.put_char('\n');
                term.set_color(make_color(LIGHT_GREY, BLACK));
                run_command(p);
            }
            linepos = 0;
        } else if (c != '\r') {
            if (linepos < HIST_LEN - 1) line[linepos++] = c;
        }
    }
    if (linepos > 0) {
        line[linepos] = 0;
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != 0 && *p != '#') {
            term.set_color(make_color(YELLOW, BLACK));
            term.write("> "); term.write(p); term.put_char('\n');
            term.set_color(make_color(LIGHT_GREY, BLACK));
            run_command(p);
        }
    }
    g_in_script = false;
}

static void cmd_run(const char* name){
    if (!name[0]) { term.write("Usage: run <script.sh>\n"); return; }
    int ret = mkfs.read(name, g_iobuf, FS_IOBUF_SIZE);
    if (ret < 0) {
        term.write("File not found: "); term.write(name); term.put_char('\n');
        return;
    }
    term.set_color(make_color(CYAN, BLACK));
    term.write("--- running: "); term.write(name); term.write(" ---\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
    run_script((const char*)g_iobuf, ret);
    term.set_color(make_color(CYAN, BLACK));
    term.write("--- end of script ---\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
}

static void cmd_runfs(const char* name){
    if (!name[0]) { term.write("Usage: runfs <script.sh>\n"); return; }
    int ret = sfs.read(name, g_iobuf, FS_IOBUF_SIZE);
    if (ret < 0) {
        term.write("File not found: "); term.write(name); term.put_char('\n');
        return;
    }
    term.set_color(make_color(CYAN, BLACK));
    term.write("--- running SFS: "); term.write(name); term.write(" ---\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
    run_script((const char*)g_iobuf, ret);
    term.set_color(make_color(CYAN, BLACK));
    term.write("--- end of script ---\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
}

// =====================================================================
//  Shutdown / Reboot  -  ACPI power-off + keyboard controller reset
// =====================================================================
static void cmd_shutdown(){
    term.set_color(make_color(YELLOW, BLACK));
    term.write("\nShutting down...\n");
    term.render();
    serial_puts("[SHUTDOWN] ACPI power-off requested\n");

    // QEMU / ACPI shutdown methods (try multiple ports)
    outw(0x604, 0x2000);    // QEMU ACPI shutdown (modern)
    outw(0xB004, 0x2000);   // QEMU/Bochs ACPI (legacy)
    outw(0x4004, 0x3400);   // Older ACPI

    // Fallback: halt forever
    for(;;){
        __asm__ __volatile__("hlt");
    }
}

static void cmd_reboot(){
    term.set_color(make_color(YELLOW, BLACK));
    term.write("\nRebooting...\n");
    term.render();
    serial_puts("[REBOOT] Keyboard controller reset\n");

    // Keyboard controller reset (pulse reset line)
    // Small delay to let serial flush
    for(volatile int i=0; i<100000; i++);
    uint8_t val = inb(0x64);
    outb(0x64, val | 0x04);    // set System Reset bit
    outb(0x64, val & ~0x04);   // clear it
    // Alternative: write 0xFE to port 0x64
    outb(0x64, 0xFE);

    // Triple-fault fallback (64-bit IDT descriptor: 2-byte limit + 8-byte base)
    static struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ __volatile__("lidt %0" :: "m"(null_idt));
    __asm__ __volatile__("int $0x03");

    for(;;){ __asm__ __volatile__("hlt"); }
}

// =====================================================================
//  AI Engine commands
// =====================================================================

static void cmd_ai(const char* args){
    if(!*args){
        if(!g_ai_initialized){
            term.write("AI engine not initialized. Use 'ai init' first.\n");
            return;
        }
        char info[512];
        ai_get_info(info, sizeof(info));
        term.write(info);
        return;
    }
    // Parse subcommand
    char sub[16]; int si=0;
    while(*args && *args!=' ' && si<15) sub[si++]=*args++;
    sub[si]=0;
    while(*args==' ') args++;

    if(!strcmp_(sub,"init")){
        term.write("Initializing AI engine...\n");
        serial_puts("[AI] ai_init starting...\n");
        int ret = ai_init("/boot/model.gguf");
        serial_puts("[AI] ai_init returned.\n");
        if(ret==0){
            g_ai_initialized = true;
            term.write("AI engine initialized (Markov mode).\n");
            term.write("  Corpus trained. Model weights generated.\n");
        } else {
            term.write("AI init failed!\n");
        }
    } else if(!strcmp_(sub,"info")){
        if(!g_ai_initialized){ term.write("AI not initialized. Use 'ai init'.\n"); return; }
        char info[512];
        ai_get_info(info, sizeof(info));
        term.write(info);
    } else if(!strcmp_(sub,"mode")){
        if(!g_ai_initialized){ term.write("AI not initialized.\n"); return; }
        if(!strcmp_(args,"transformer") || !strcmp_(args,"1")){
            ai_set_mode(1);
            term.write("Switched to Transformer mode.\n");
        } else if(!strcmp_(args,"markov") || !strcmp_(args,"0")){
            ai_set_mode(0);
            term.write("Switched to Markov mode.\n");
        } else {
            term.write("Usage: ai mode <markov|transformer>\n");
        }
    } else if(!strcmp_(sub,"test")){
        if(!g_ai_initialized){ term.write("AI not initialized.\n"); return; }
        term.write("Running transformer forward pass test...\n");
        int ret = ai_transformer_test();
        if(ret==0) term.write("Transformer forward pass: OK (no NaN/Inf)\n");
        else term.write("Transformer test failed!\n");
    } else if(!strcmp_(sub,"cleanup")){
        if(g_ai_initialized){
            ai_cleanup();
            g_ai_initialized = false;
            term.write("AI engine cleaned up.\n");
        } else {
            term.write("AI not initialized.\n");
        }
    } else {
        term.write("Usage: ai [init|info|mode|test|cleanup]\n");
    }
}

static void cmd_generate(const char* args){
    if(!g_ai_initialized){
        term.write("AI not initialized. Use 'ai init' first.\n");
        return;
    }
    if(!*args){
        term.write("Usage: generate <prompt>\n");
        return;
    }
    term.write("Generating...");
    term.put_char('\n');
    char* result = ai_generate(args, 200);
    if(result){
        term.write(result);
        term.put_char('\n');
        kfree(result);
    } else {
        term.write("Generation failed.\n");
    }
}

static void cmd_agent(const char* args){
    if(!*args){
        char status[256];
        agent_get_status(status, sizeof(status));
        term.write(status);
        return;
    }
    char sub[16]; int si=0;
    while(*args && *args!=' ' && si<15) sub[si++]=*args++;
    sub[si]=0;
    while(*args==' ') args++;

    if(!strcmp_(sub,"init")){
        if(!g_ai_initialized){
            ai_init("/boot/model.gguf");
            g_ai_initialized = true;
        }
        agent_init();
        term.write("Agent framework initialized.\n");
        term.write("  Agents: Planner, Actor, Critic\n");
        term.write("  Use 'agent run <goal>' to execute.\n");
    } else if(!strcmp_(sub,"run")){
        if(!*args){
            term.write("Usage: agent run <goal>\n");
            return;
        }
        term.write("Running agent pipeline...\n\n");
        char output[4096];
        int n = agent_run(args, output, sizeof(output));
        if(n > 0){
            term.write(output);
        } else {
            term.write("Agent run failed. Initialize first with 'agent init'.\n");
        }
    } else if(!strcmp_(sub,"status")){
        char status[256];
        agent_get_status(status, sizeof(status));
        term.write(status);
    } else {
        term.write("Usage: agent [init|run|status]\n");
    }
}

// =====================================================================
//  Network commands
// =====================================================================
static void cmd_netinfo(){
    if(!g_net_initialized){
        term.write("Network not initialized. Use 'netstart' to initialize.\n");
        return;
    }
    char info[512];
    int n = net_status(info, sizeof(info));
    if(n > 0) term.write(info);
    else term.write("Failed to get network status.\n");
}

static void cmd_netstart(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("Initializing network...\n");
    serial_puts("[K8] Initializing network...\n");
    int ret = net_init();
    if(ret == 0){
        g_net_initialized = true;
        term.set_color(make_color(GREEN, BLACK));
        term.write("Network UP! HTTP server on http://10.0.2.15:8080\n");
        term.set_color(make_color(CYAN, BLACK));
        term.write("  (QEMU: use -net nic,model=ne2k_isa -net user,hostfwd=tcp::8080-:8080)\n");
    } else {
        term.set_color(make_color(RED, BLACK));
        term.write("Network init failed! (NE2000 NIC not detected)\n");
        term.set_color(make_color(CYAN, BLACK));
        term.write("  Make sure QEMU has: -net nic,model=ne2k_isa\n");
    }
    term.set_color(make_color(WHITE, BLACK));
}

// =====================================================================
//  Switch to 32-bit kernel
//  Reads kernel.bin from disk (LBA 33) into memory at 0x10000,
//  then calls switch_to_32bit() which transitions the CPU back to
//  32-bit protected mode.
// =====================================================================
static void cmd_switch32(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("\nSwitching to 32-bit kernel...\n");
    serial_puts("[K64] Switching to 32-bit kernel...\n");

    // ---- Load kernel.bin from disk to 0x10000 (64KB) ----
    term.write("Loading kernel.bin from LBA ");
    term.write_dec(KERNEL32_LBA);
    term.write("...\n");

    uint16_t* dst = (uint16_t*)KERNEL32_ADDR;
    for(int i=0; i<KERNEL32_SECTORS; i++){
        ata_read_sector(KERNEL32_LBA + i, dst);
        dst += 256;  // 512 bytes = 256 words
        if(i % 32 == 0){
            term.put_char('\r');
            term.write("  [");
            term.write_dec(i);
            term.write("/");
            term.write_dec(KERNEL32_SECTORS);
            term.write("] sectors loaded");
        }
    }

    term.write("\n  Kernel32 loaded at 0x10000\n");
    term.write("  Transitioning to 32-bit protected mode...\n\n");
    serial_puts("[K64] Kernel32 loaded, calling switch_to_32bit()...\n");

    // ---- Call the mode switcher ----
    // This function never returns — it far-jumps to the 32-bit kernel
    switch_to_32bit();

    // Should never reach here
    term.set_color(make_color(RED, BLACK));
    term.write("ERROR: switch_to_32bit() returned!\n");
    serial_puts("[K64] ERROR: switch_to_32bit returned!\n");
    term.set_color(make_color(WHITE, BLACK));
}

// =====================================================================
//  GUI mode - enter graphical desktop environment
// =====================================================================
static void cmd_gui(){
    if(!g_vbe_active){
        term.set_color(make_color(RED, BLACK));
        term.write("GUI not available (VBE graphics mode not set).\n");
        term.set_color(make_color(CYAN, BLACK));
        term.write("  VBE requires BIOS boot with graphics mode support.\n");
        term.set_color(make_color(WHITE, BLACK));
        return;
    }
    if(!gui_available()){
        term.set_color(make_color(RED, BLACK));
        term.write("GUI initialization failed.\n");
        term.set_color(make_color(WHITE, BLACK));
        return;
    }

    term.set_color(make_color(CYAN, BLACK));
    term.write("Entering Win11 Desktop GUI mode...\n");
    serial_puts("[K64] Entering Win11 GUI mode\n");

    // Enter GUI - renders Win11-style desktop with top bar, icons, and cursor
    gui_enter();
    gui_render();

    term.set_color(make_color(WHITE, BLACK));
}

// =====================================================================
//  Tab completion  -  PowerShell-style command & filename completion
// =====================================================================

// All known command names (including PowerShell-style aliases)
static const char* g_cmd_table[] = {
    "help", "echo", "clear", "cls", "about", "history", "save", "load",
    "mkfs", "ls", "dir", "cat", "type", "touch", "rm", "del",
    "copy", "write", "mkdir", "md", "cd", "pwd",
    "lsfs", "catfs", "part", "mount", "lsfat", "fatinfo",
    "run", "runfs", "ai", "generate", "agent",
    "netinfo", "netstat", "netstart",
    "switch", "shutdown", "reboot", "exit",
    "gui"
};
static const int g_cmd_count = sizeof(g_cmd_table)/sizeof(g_cmd_table[0]);

// Find all commands matching the given prefix. Returns count, fills matches[].
static int match_commands(const char* prefix, const char* matches[], int max){
    int count = 0;
    int prefixlen = strlen_(prefix);
    for(int i=0; i<g_cmd_count && count<max; i++){
        if(strncmp_(g_cmd_table[i], prefix, prefixlen) == 0)
            matches[count++] = g_cmd_table[i];
    }
    return count;
}

// Find all MKFS files in current directory matching the given prefix
static int match_files(const char* prefix, char matches[][FS_NAME_LEN], int max){
    if(!mkfs.mounted) return 0;
    int count = 0;
    int prefixlen = strlen_(prefix);
    for(int s=0; s<MKFS_TABLE_SECT && count<max; s++){
        ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        for(int e=0; e<FS_ENTRY_PER_SEC; e++){
            FileEntry* fe = (FileEntry*)(g_fsbuf + e*FS_ENTRY_SIZE);
            if(fe->name[0] != 0 && fe->parent == g_cwd){
                if(strncmp_(fe->name, prefix, prefixlen) == 0){
                    int j=0;
                    while(fe->name[j] && j<FS_NAME_LEN-1){ matches[count][j]=fe->name[j]; j++; }
                    matches[count][j] = 0;
                    count++;
                }
            }
        }
    }
    return count;
}

// Perform tab completion on the current input buffer.
// Modifies inbuf and inlen in place. Returns true if completed.
static bool do_tab_complete(char* inbuf, int* inlen){
    // Make a null-terminated copy of current input
    inbuf[*inlen] = 0;

    // Find the last word boundary (space or start)
    int word_start = *inlen;
    while(word_start > 0 && inbuf[word_start-1] != ' ')
        word_start--;
    const char* partial = inbuf + word_start;
    int partial_len = *inlen - word_start;

    if(word_start == 0){
        // Completing a command name
        const char* cmd_matches[16];
        int n = match_commands(partial, cmd_matches, 16);
        if(n == 1){
            // Single match: complete it
            int mlen = strlen_(cmd_matches[0]);
            for(int i=partial_len; i<mlen; i++){
                if(*inlen < HIST_LEN-1){
                    inbuf[*inlen] = cmd_matches[0][i];
                    term.put_char(cmd_matches[0][i]);
                    (*inlen)++;
                }
            }
            // Add a space after command
            if(*inlen < HIST_LEN-1){
                inbuf[*inlen] = ' ';
                term.put_char(' ');
                (*inlen)++;
            }
            return true;
        } else if(n > 1){
            // Multiple matches: find common prefix and show options
            int common = partial_len;
            bool done = false;
            while(!done){
                char c = cmd_matches[0][common];
                if(c == 0) break;
                bool all_match = true;
                for(int i=1; i<n; i++){
                    if(cmd_matches[i][common] != c){ all_match = false; break; }
                }
                if(!all_match) break;
                common++;
            }
            // Complete up to common prefix
            for(int i=partial_len; i<common; i++){
                if(*inlen < HIST_LEN-1){
                    inbuf[*inlen] = cmd_matches[0][i];
                    term.put_char(cmd_matches[0][i]);
                    (*inlen)++;
                }
            }
            // Show all matches
            term.put_char('\n');
            term.set_color(make_color(CYAN, BLACK));
            for(int i=0; i<n; i++){
                term.write(cmd_matches[i]);
                term.write("  ");
            }
            term.set_color(make_color(LIGHT_GREY, BLACK));
            term.put_char('\n');
            return true;
        }
    } else {
        // Completing a filename
        char file_matches[16][FS_NAME_LEN];
        int n = match_files(partial, file_matches, 16);
        if(n == 1){
            int mlen = strlen_(file_matches[0]);
            for(int i=partial_len; i<mlen; i++){
                if(*inlen < HIST_LEN-1){
                    inbuf[*inlen] = file_matches[0][i];
                    term.put_char(file_matches[0][i]);
                    (*inlen)++;
                }
            }
            return true;
        } else if(n > 1){
            // Find common prefix
            int common = partial_len;
            bool done = false;
            while(!done){
                char c = file_matches[0][common];
                if(c == 0) break;
                bool all_match = true;
                for(int i=1; i<n; i++){
                    if(file_matches[i][common] != c){ all_match = false; break; }
                }
                if(!all_match) break;
                common++;
            }
            for(int i=partial_len; i<common; i++){
                if(*inlen < HIST_LEN-1){
                    inbuf[*inlen] = file_matches[0][i];
                    term.put_char(file_matches[0][i]);
                    (*inlen)++;
                }
            }
            // Show all matches
            term.put_char('\n');
            term.set_color(make_color(CYAN, BLACK));
            for(int i=0; i<n; i++){
                term.write(file_matches[i]);
                term.write("  ");
            }
            term.set_color(make_color(LIGHT_GREY, BLACK));
            term.put_char('\n');
            return true;
        }
    }
    // No match: beep
    term.put_char('\a');
    return false;
}

// =====================================================================
//  Command dispatcher
// =====================================================================

static void run_command(const char* line){
    while(*line==' ') line++;
    if(*line==0) return;
    if (!g_in_script) hist_add(line);

    // Normalize path separators in the entire line
    char normline[HIST_LEN];
    int li=0;
    while(line[li] && li<HIST_LEN-1){ normline[li]=line[li]; li++; }
    normline[li]=0;
    normalize_path(normline);

    char cmd[32]; int ci=0;
    const char* p=normline;
    while(*p && *p!=' ' && ci<31) cmd[ci++]=*p++;
    cmd[ci]=0;
    const char* args=p; while(*args==' ') args++;

    if(!strcmp_(cmd,"help"))       cmd_help();
    else if(!strcmp_(cmd,"echo"))  { term.write(args); term.put_char('\n'); }
    else if(!strcmp_(cmd,"clear")||!strcmp_(cmd,"cls")) term.clear_screen();
    else if(!strcmp_(cmd,"about")) cmd_about();
    else if(!strcmp_(cmd,"history")||!strcmp_(cmd,"h")) cmd_history();
    else if(!strcmp_(cmd,"save"))  cmd_save();
    else if(!strcmp_(cmd,"load"))  cmd_load();
    // MKFS commands (with PowerShell-style aliases)
    else if(!strcmp_(cmd,"mkfs"))  cmd_mkfs();
    else if(!strcmp_(cmd,"ls")||!strcmp_(cmd,"dir")) cmd_ls();
    else if(!strcmp_(cmd,"cat")||!strcmp_(cmd,"type")) cmd_cat(args);
    else if(!strcmp_(cmd,"touch")||!strcmp_(cmd,"ni")) cmd_touch(args);
    else if(!strcmp_(cmd,"rm")||!strcmp_(cmd,"del")||!strcmp_(cmd,"erase")) cmd_rm(args);
    else if(!strcmp_(cmd,"copy")||!strcmp_(cmd,"cp"))  cmd_copy(args);
    else if(!strcmp_(cmd,"write")) cmd_write(args);
    else if(!strcmp_(cmd,"mkdir")||!strcmp_(cmd,"md")) cmd_mkdir(args);
    else if(!strcmp_(cmd,"cd")||!strcmp_(cmd,"sl"))    cmd_cd(args);
    else if(!strcmp_(cmd,"pwd")||!strcmp_(cmd,"gl"))   cmd_pwd();
    // SFS commands
    else if(!strcmp_(cmd,"lsfs"))  cmd_lsfs();
    else if(!strcmp_(cmd,"catfs")) cmd_catfs(args);
    // Partition commands
    else if(!strcmp_(cmd,"part"))  cmd_part();
    else if(!strcmp_(cmd,"mount")) cmd_mount(args);
    else if(!strcmp_(cmd,"lsfat")) cmd_lsfat();
    else if(!strcmp_(cmd,"fatinfo"))cmd_fatinfo();
    // Script execution
    else if(!strcmp_(cmd,"run"))   cmd_run(args);
    else if(!strcmp_(cmd,"runfs")) cmd_runfs(args);
    // AI engine commands
    else if(!strcmp_(cmd,"ai"))         cmd_ai(args);
    else if(!strcmp_(cmd,"generate")||!strcmp_(cmd,"gen")) cmd_generate(args);
    else if(!strcmp_(cmd,"agent"))      cmd_agent(args);
    // Network commands
    else if(!strcmp_(cmd,"netinfo")||!strcmp_(cmd,"netstat")) cmd_netinfo();
    else if(!strcmp_(cmd,"netstart")||!strcmp_(cmd,"net"))   cmd_netstart();
    // Kernel switching
    else if(!strcmp_(cmd,"switch")||!strcmp_(cmd,"switch32")) cmd_switch32();
    // GUI
    else if(!strcmp_(cmd,"gui"))  cmd_gui();
    // Memory management
    else if(!strcmp_(cmd,"meminfo"))  cmd_meminfo();
    else if(!strcmp_(cmd,"memtest"))  cmd_memtest();
    else if(!strcmp_(cmd,"pagetest")) cmd_pagetest();
    // Power management
    else if(!strcmp_(cmd,"shutdown")||!strcmp_(cmd,"exit")||!strcmp_(cmd,"poweroff")) cmd_shutdown();
    else if(!strcmp_(cmd,"reboot")||!strcmp_(cmd,"restart")) cmd_reboot();
    else { term.write("Unknown command: "); term.write(cmd);
           term.write("  (Type 'help' for available commands)\n"); }
}

// =====================================================================
//  Terminal::render (defined after the class, uses its members)
// =====================================================================
void Terminal::render(){
    if(m_at_bottom) m_view=bottom_view();
    int cur_row = m_at_bottom ? (m_count - m_view) : -1;
    for(int r=0;r<VGA_HEIGHT;r++){
        if(m_at_bottom && r==cur_row){
            for(int x=0;x<VGA_WIDTH;x++){
                char ch=(x<m_cur_len)?m_cur[x]:' ';
                VGA_MEMORY[r*VGA_WIDTH+x]=make_entry((unsigned char)ch,m_color);
            }
        } else {
            int li=m_view+r;
            if(li<m_count){
                Line& L=line_at(li);
                for(int x=0;x<VGA_WIDTH;x++){
                    char ch=(x<L.len)?L.data[x]:' ';
                    VGA_MEMORY[r*VGA_WIDTH+x]=make_entry((unsigned char)ch,m_color);
                }
            } else {
                for(int x=0;x<VGA_WIDTH;x++)
                    VGA_MEMORY[r*VGA_WIDTH+x]=make_entry(' ',m_color);
            }
        }
    }

    // ----- Selection highlight (invert colors for selected cells) -----
    if(m_selecting || m_has_selection){
        int sx=m_sel_sx, sy=m_sel_sy, ex=m_sel_ex, ey=m_sel_ey;
        if(sy>ey || (sy==ey && sx>ex)){
            int t=sx; sx=ex; ex=t;
            int t2=sy; sy=ey; ey=t2;
        }
        for(int r=sy; r<=ey && r<VGA_HEIGHT; r++){
            int cs = (r==sy) ? sx : 0;
            int ce = (r==ey) ? ex : VGA_WIDTH-1;
            for(int x=cs; x<=ce && x<VGA_WIDTH; x++){
                uint16_t cell = VGA_MEMORY[r*VGA_WIDTH+x];
                uint8_t  attr = (cell>>8)&0xFF;
                uint8_t  inv  = ((attr&0x0F)<<4) | ((attr>>4)&0x0F);
                VGA_MEMORY[r*VGA_WIDTH+x] = (cell&0x00FF) | ((uint16_t)inv<<8);
            }
        }
    }

    // ----- Mouse cursor (invert the cell under the mouse) -----
    if(m_mouse_visible){
        int idx = m_mouse_y * VGA_WIDTH + m_mouse_x;
        if(idx >= 0 && idx < VGA_WIDTH * VGA_HEIGHT){
            uint16_t cell = VGA_MEMORY[idx];
            uint8_t  attr = (cell>>8)&0xFF;
            uint8_t  inv  = ((attr&0x0F)<<4) | ((attr>>4)&0x0F);
            VGA_MEMORY[idx] = (cell&0x00FF) | ((uint16_t)inv<<8);
        }
    }

    if(m_at_bottom){ show_cursor(); set_cursor_pos(cur_row, m_cur_pos); }
    else            hide_cursor();
}

// =====================================================================
//  GUI Callbacks - registered by kernel for data access (64-bit version)
// =====================================================================
// Simple int-to-string for serial debug
static void int_to_str_k64(int val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    char tmp[16]; int i = 0;
    bool neg = val < 0;
    if (neg) val = -val;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

static void uint_to_str_k64(uint32_t val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    char tmp[16]; int i = 0;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

// ---- Memory info callbacks ----
static uint32_t gui_cb_total_mem(void)    { return pmm_mem_kb; }
static uint32_t gui_cb_free_pages(void)   { return pmm_free_pages; }
static uint32_t gui_cb_used_pages(void)   { return pmm_used_pages; }
static uint32_t gui_cb_total_pages(void)  { return pmm_total_pages; }

// ---- Heap info callbacks ----
static uint32_t gui_cb_heap_alloc_bytes(void) { return heap_bytes_alloc - heap_bytes_freed; }
static uint32_t gui_cb_heap_free_bytes(void)  {
    uint32_t used = heap_bytes_alloc - heap_bytes_freed;
    return (HEAP_SIZE - sizeof(HeapBlock)) > used ? (HEAP_SIZE - sizeof(HeapBlock)) - used : 0;
}
static uint32_t gui_cb_heap_alloc_count(void) { return heap_alloc_count - heap_free_count; }
static uint32_t gui_cb_heap_free_count(void)  { return heap_free_count; }

// ---- Memory optimization callback ----
static void gui_cb_optimize_memory(void) {
    serial_puts("[K64-MEM] Memory optimization requested\n");
    // Coalesce free heap blocks
    HeapBlock* blk = heap_head;
    int coalesced = 0;
    while (blk) {
        if (blk->magic == HEAP_MAGIC_FREE && blk->next &&
            blk->next->magic == HEAP_MAGIC_FREE) {
            HeapBlock* nb = blk->next;
            blk->size += sizeof(HeapBlock) + nb->size;
            blk->next = nb->next;
            if (nb->next) nb->next->prev = blk;
            coalesced++;
            continue; // don't advance, try again
        }
        blk = blk->next;
    }
    serial_puts("[K64-MEM] Heap coalesced ");
    char buf[16]; int_to_str_k64(coalesced, buf); serial_puts(buf);
    serial_puts(" blocks\n");
}

// ---- File listing callback ----
// fs_type: 0=MKFS, 1=SFS, 2=FAT32
static int gui_cb_list_files(int fs_type, char* outbuf, int bufsize) {
    int count = 0;
    int pos = 0;
    outbuf[0] = 0;

    if (fs_type == 0) {
        // MKFS
        if (!mkfs.mounted) return 0;
        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0 && fe->parent == g_cwd) {
                    const char* prefix = (fe->type == FS_TYPE_DIR) ? "[D] " : "    ";
                    int plen = strlen_(prefix);
                    int nlen = strlen_(fe->name);
                    if (pos + plen + nlen + 2 < bufsize) {
                        memcpy_(outbuf + pos, prefix, plen); pos += plen;
                        memcpy_(outbuf + pos, fe->name, nlen); pos += nlen;
                        outbuf[pos++] = '\n';
                        count++;
                    }
                }
            }
        }
    } else if (fs_type == 1) {
        // SFS
        if (!sfs.mounted) return 0;
        for (int s = 0; s < SFS_DIR_SECT; s++) {
            ata_read_sector(SFS_DIR_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0) {
                    int nlen = strlen_(fe->name);
                    if (pos + nlen + 12 < bufsize) {
                        memcpy_(outbuf + pos, fe->name, nlen); pos += nlen;
                        outbuf[pos++] = ' ';
                        outbuf[pos++] = '(';
                        char sz[12]; uint_to_str_k64(fe->size, sz);
                        int sl = strlen_(sz);
                        memcpy_(outbuf + pos, sz, sl); pos += sl;
                        outbuf[pos++] = 'B';
                        outbuf[pos++] = ')';
                        outbuf[pos++] = '\n';
                        count++;
                    }
                }
            }
        }
    } else if (fs_type == 2) {
        // FAT32
        if (!fat32.mounted) return 0;
        uint32_t cluster = fat32.root_cluster;
        for (int cl = 0; cl < 32; cl++) {
            if (cluster < 2 || cluster >= 0x0FFFFFF8) break;
            uint32_t lba = fat32.data_start + (cluster - 2) * fat32.sectors_per_cluster;
            for (int s = 0; s < fat32.sectors_per_cluster && s * 512 < FS_IOBUF_SIZE; s++)
                ata_read_sector(lba + s, (uint16_t*)(g_iobuf + s * 512));
            for (int i = 0; i < fat32.sectors_per_cluster * 512; i += 32) {
                uint8_t* de = g_iobuf + i;
                if (de[0] == 0x00) break;
                if (de[0] == 0xE5) continue;
                if (de[11] & 0x0F) continue; // skip LFN entries
                char name[13];
                int ni = 0;
                for (int j = 0; j < 8 && de[j] != ' '; j++)
                    if (ni < 12) name[ni++] = de[j];
                if (de[8] != ' ') {
                    if (ni < 12) name[ni++] = '.';
                    for (int j = 8; j < 11 && de[j] != ' '; j++)
                        if (ni < 12) name[ni++] = de[j];
                }
                name[ni] = 0;
                int nlen = ni;
                if (pos + nlen + 2 < bufsize) {
                    memcpy_(outbuf + pos, name, nlen); pos += nlen;
                    outbuf[pos++] = '\n';
                    count++;
                }
            }
            // Follow FAT chain
            uint32_t fat_off = cluster * 4;
            uint32_t fat_sec = fat32.part_start + fat32.reserved_sectors + fat_off / 512;
            ata_read_sector(fat_sec, (uint16_t*)g_fsbuf);
            cluster = *(uint32_t*)(g_fsbuf + (fat_off % 512)) & 0x0FFFFFFF;
        }
    }
    outbuf[pos] = 0;
    return count;
}

// ---- Time callback (from CMOS RTC) ----
static void gui_cb_get_time(int* h, int* m, int* s) {
    outb(0x70, 0x00); *s = inb(0x71);
    outb(0x70, 0x02); *m = inb(0x71);
    outb(0x70, 0x04); *h = inb(0x71);
    // Convert BCD to binary
    *s = (*s & 0x0F) + ((*s >> 4) & 0x0F) * 10;
    *m = (*m & 0x0F) + ((*m >> 4) & 0x0F) * 10;
    *h = (*h & 0x0F) + ((*h >> 4) & 0x0F) * 10;
}

// ---- OS name callback ----
static const char* gui_cb_os_name(void) {
    return "MiniOS v2.0 (Win11 Desktop)";
}

// ---- 64-bit detection ----
// Returns whether we are CURRENTLY running in 64-bit long mode.
// This 64-bit kernel runs in long mode, so return true.
static bool gui_cb_is_64bit(void) {
    return true; // 64-bit long mode active
}

// CPU supports 64-bit (always true in 64-bit kernel)
static int gui_cb_cpu_64bit_capable(void) {
    return 1;
}

// ---- Register all callbacks ----
static void register_gui_callbacks(void) {
    GuiCallbacks cb;
    cb.get_total_mem_kb    = gui_cb_total_mem;
    cb.get_free_pages      = gui_cb_free_pages;
    cb.get_used_pages      = gui_cb_used_pages;
    cb.get_total_pages     = gui_cb_total_pages;
    cb.get_heap_alloc_bytes= gui_cb_heap_alloc_bytes;
    cb.get_heap_free_bytes = gui_cb_heap_free_bytes;
    cb.get_heap_alloc_count= gui_cb_heap_alloc_count;
    cb.get_heap_free_count = gui_cb_heap_free_count;
    cb.optimize_memory     = gui_cb_optimize_memory;
    cb.list_files          = gui_cb_list_files;
    cb.get_time            = gui_cb_get_time;
    cb.get_os_name         = gui_cb_os_name;
    cb.is_64bit            = gui_cb_is_64bit;
    cb.get_cpu_64bit_capable = gui_cb_cpu_64bit_capable;
    gui_set_callbacks(&cb);
}

// =====================================================================
//  kmain64  -  64-bit kernel entry point (called from entry64.asm)
// =====================================================================
extern "C" void kmain64(){
    serial_puts("[K64-1] kmain64 entered\n");

    // ---- Always set VGA text mode (kernel starts in text mode) ----
    // VBE info is collected by stage2 but mode is NOT set.
    // GUI mode is activated on demand via "gui" command using Bochs VBE ports.
    vga_set_text_mode();
    serial_puts("[K64-2] VGA text mode set\n");

    // ---- Check if VBE info is available (for GUI) ----
    volatile uint8_t* vbe_flag = (volatile uint8_t*)0x500D;
    if(*vbe_flag == 1){
        g_vbe_active = true;
        serial_puts("[K64-VBE] VBE info available (GUI ready)\n");
    }

    memset_(g_hist,0,sizeof(g_hist));
    memset_(g_diskbuf,0,sizeof(g_diskbuf));
    memset_(g_iobuf,0,sizeof(g_iobuf));
    memset_(g_writebuf,0,sizeof(g_writebuf));

    term.init();
    serial_puts("[K64-3] terminal init done\n");
    kbd  = Keyboard();
    mouse.init();
    serial_puts("[K64-4] mouse init done\n");

    mkfs.init();
    sfs.init();
    fat32.init();
    serial_puts("[K64-6] filesystem init done\n");

    // ---- Memory management ----
    pmm_init();
    vmm_init();
    heap_init();
    serial_puts("[K64-7] memory management init done\n");

    // ---- GUI initialization (if VBE info is available) ----
    // Initialize GUI but do NOT auto-enter. Boot to command line.
    // Users can type 'gui' to enter the desktop.
    if(g_vbe_active){
        register_gui_callbacks();
        gui_init();
        if(gui_available()){
            serial_puts("[K64-VBE] GUI initialized successfully (available via 'gui' command)\n");
        } else {
            serial_puts("[K64-VBE] GUI init failed - command line only\n");
            g_vbe_active = false;
        }
    }

    // ---- Network initialization ----
    serial_puts("[K64-8] Initializing network...\n");
    {
        int net_ret = net_init();
        if(net_ret == 0){
            g_net_initialized = true;
            serial_puts("[K64-8] Network initialized successfully\n");
        } else {
            serial_puts("[K64-8] Network init failed (no NIC?)\n");
        }
    }

    term.set_color(make_color(GREEN,BLACK));
    term.write("Hello world from C++ 64-bit kernel!\n");
    serial_puts("[K64-5] Hello world written\n");
    term.set_color(make_color(WHITE,BLACK));
    term.write("64-bit long mode kernel active. Type 'switch' to return to 32-bit.\n\n");

    // File system status
    if (mkfs.mounted) {
        term.set_color(make_color(CYAN,BLACK));
        term.write("MKFS: mounted ("); term.write_dec(mkfs.sb.file_count);
        term.write(" files)\n");
    } else {
        term.set_color(make_color(BROWN,BLACK));
        term.write("MKFS: not formatted (use 'mkfs' to format)\n");
    }
    if (sfs.mounted) {
        term.set_color(make_color(CYAN,BLACK));
        term.write("SFS:  mounted ("); term.write_dec(sfs.sb.file_count);
        term.write(" files)\n");
    } else {
        term.set_color(make_color(BROWN,BLACK));
        term.write("SFS:  not found\n");
    }
    term.set_color(make_color(BROWN,BLACK));
    term.write("FAT32: not mounted (use 'part' + 'mount')\n");

    // Memory management status
    term.set_color(make_color(CYAN,BLACK));
    term.write("MEM:  "); term.write_dec((int)(pmm_mem_kb/1024)); term.write(" MiB RAM, ");
    term.write_dec((int)pmm_free_pages); term.write(" free pages, paging ");
    term.write(vmm_paging_on ? "ON" : "OFF");
    if (vmm_our_paging)       term.write(" (32-bit PSE)\n");
    else if (vmm_long_mode)   term.write(" (64-bit long mode)\n");
    else                      term.write(" (firmware)\n");

    // Network status
    if(g_net_initialized){
        term.set_color(make_color(GREEN,BLACK));
        term.write("NET:  UP  HTTP server on http://10.0.2.15:8080\n");
    } else {
        term.set_color(make_color(BROWN,BLACK));
        term.write("NET:  not detected (use 'netstart' to retry)\n");
    }

    // GUI status
    if(g_vbe_active){
        term.set_color(make_color(GREEN,BLACK));
        term.write("GUI:  VBE "); term.write_dec(gui_get_width());
        term.write("x"); term.write_dec(gui_get_height());
        term.write(" (type 'gui' to enter Win11 desktop)\n");
    } else {
        term.set_color(make_color(BROWN,BLACK));
        term.write("GUI:  not available (text mode)\n");
    }

    term.set_color(make_color(CYAN,BLACK));
    term.write("\nShell ready. Type 'help' for commands.\n");
    term.write("Tab=autocomplete, Arrows=cursor/history, PgUp/PgDn=scroll, Home/End, Ctrl+C/V/L.\n\n");

    char inbuf[HIST_LEN];
    int  hist_recall = -1;      // command history recall index (-1 = not recalling)

    for(;;){
        // ---- GUI event loop ----
        // If GUI mode is active (entered via 'gui' command), run a separate
        // event loop that routes mouse/keyboard to the window manager.
        // ESC exits GUI mode and returns to text terminal.
        if(gui_is_active()){
            bool gui_prev_left = false;
            int gui_tick_counter = 0;
            while(gui_is_active()){
                if(g_net_initialized) net_poll();

                // Update clock every ~50 iterations
                if(++gui_tick_counter > 50){
                    gui_tick();
                    gui_tick_counter = 0;
                }

                uint8_t st = inb(0x64);
                if(st & 0x01){
                    uint8_t data = inb(0x60);
                    if(st & 0x20){
                        // ----- Mouse event -----
                        MouseEvent me = mouse.process(data);
                        if(me.valid){
                            // Mouse movement (PS/2 Y is inverted, GUI Y is down-positive)
                            if(me.dx != 0 || me.dy != 0){
                                gui_mouse_move(me.dx, -me.dy);
                            }
                            // Left button: down/up for dragging and clicking
                            if(me.left && !gui_prev_left){
                                gui_mouse_down();
                            } else if(!me.left && gui_prev_left){
                                gui_mouse_up();
                            }
                            gui_prev_left = me.left;
                        }
                    } else {
                        // ----- Keyboard event -----
                        KbdEvent e = kbd.process(data);
                        if(e.type == K_CHAR){
                            if(e.ch == 0x1B){
                                // ESC: exit GUI mode
                                gui_exit();
                            } else {
                                // Pass character to GUI window manager
                                gui_handle_key(e.ch);
                            }
                        }
                    }
                }
            }
            // After GUI exits, re-render terminal to framebuffer
            term.render();
            continue;  // Go back to prompt
        }

        if (g_mode == MODE_WRITE) {
            term.set_color(make_color(MAGENTA,BLACK));
            term.write(">> ");
        } else {
            // PowerShell-style prompt: "PS /path> "
            term.set_color(make_color(GREEN,BLACK));
            term.write("PS ");
            char pathbuf[FS_NAME_LEN * 4];
            build_prompt_path(pathbuf, sizeof(pathbuf));
            term.set_color(make_color(CYAN, BLACK));
            term.write(pathbuf);
            term.set_color(make_color(GREEN, BLACK));
            term.write("> ");
        }
        term.set_color(make_color(LIGHT_GREY,BLACK));
        // Mark where user input begins (after prompt text)
        term.begin_input();
        term.render();

        int inlen=0;
        hist_recall = -1;
        for(;;){
            // Poll network for incoming packets (HTTP server, ARP, etc.)
            if(g_net_initialized) net_poll();
            
            uint8_t st=inb(0x64);
            if(st&0x01){
                uint8_t data=inb(0x60);
                if(st&0x20){
                    // ----- Mouse event -----
                    MouseEvent me = mouse.process(data);
                    if(me.valid){
                        // Wheel scrolling
                        if(me.dz > 0)      term.scroll_view(-3);
                        else if(me.dz < 0) term.scroll_view(3);

                        // Mouse movement
                        if(me.dx != 0 || me.dy != 0){
                            term.update_mouse(me.dx, me.dy);
                        }

                        // Left button: drag selection
                        static bool prev_left = false;
                        if(me.left && !prev_left){
                            term.mouse_left_down();
                        } else if(me.left && prev_left){
                            term.mouse_left_drag();
                        } else if(!me.left && prev_left){
                            term.mouse_left_up();
                        }
                        prev_left = me.left;

                        // Right click: refocus to input
                        if(me.right){
                            term.mouse_click();
                        }
                    }
                } else {
                    // ----- Keyboard event -----
                    KbdEvent e=kbd.process(data);

                    if(e.type==K_TAB){
                        // Tab completion
                        term.snap_bottom();
                        // Sync inbuf from terminal before completion
                        term.get_line(inbuf, &inlen);
                        do_tab_complete(inbuf, &inlen);
                        // Update terminal display with result
                        term.set_line(inbuf, inlen);
                    } else if(e.type==K_CTRL_C){
                        // Ctrl+C: if text selected, copy to clipboard; otherwise abort input
                        if(term.has_selection()){
                            // Selection already auto-copied on mouse-up; just clear it
                            term.clear_selection();
                        } else {
                            // Abort current input line
                            term.snap_bottom();
                            term.put_char('^'); term.put_char('C');
                            term.put_char('\n');
                            inbuf[0] = 0;
                            inlen = 0;
                            break;  // exit inner loop, re-prompt
                        }
                    } else if(e.type==K_CTRL_V){
                        // Ctrl+V: paste clipboard
                        term.snap_bottom();
                        for(int i=0; i<g_clipboard_len; i++){
                            char c = g_clipboard[i];
                            if(c == '\n') continue;  // skip newlines
                            term.put_char(c);
                        }
                        term.get_line(inbuf, &inlen);
                        term.render();
                    } else if(e.type==K_CTRL_L){
                        // Ctrl+L: refocus to input (snap to bottom + clear selection)
                        term.clear_selection();
                        term.snap_bottom();
                    } else if(e.type==K_CTRL_UP){
                        // Ctrl+Up: previous clipboard history
                        clipboard_hist_prev();
                        term.snap_bottom();
                        term.set_color(make_color(CYAN, BLACK));
                        term.write("\n[Clipboard]: ");
                        term.write(g_clipboard);
                        term.put_char('\n');
                        term.set_color(make_color(LIGHT_GREY, BLACK));
                        term.render();
                    } else if(e.type==K_CTRL_DOWN){
                        // Ctrl+Down: next clipboard history
                        clipboard_hist_next();
                        term.snap_bottom();
                        term.set_color(make_color(CYAN, BLACK));
                        term.write("\n[Clipboard]: ");
                        term.write(g_clipboard);
                        term.put_char('\n');
                        term.set_color(make_color(LIGHT_GREY, BLACK));
                        term.render();
                    } else if(e.type==K_CHAR){
                        term.snap_bottom();
                        if(e.ch=='\n'){
                            // Sync inbuf before committing the line
                            term.get_line(inbuf, &inlen);
                            term.put_char('\n');
                            term.render();
                            break;
                        }
                        // Backspace and character insertion handled by term.put_char
                        // (which now supports cursor-position editing)
                        term.put_char(e.ch);
                        // Sync inbuf from terminal
                        term.get_line(inbuf, &inlen);
                        // Any character input resets history recall
                        hist_recall = -1;
                        term.render();
                    } else if(e.type==K_UP){
                        // PowerShell-style: Up recalls previous command from history
                        if(term.is_at_bottom() && g_mode == MODE_NORMAL && g_hist_count > 0){
                            if(hist_recall < 0){
                                // Start recalling from most recent
                                hist_recall = g_hist_count - 1;
                            } else if(hist_recall > 0){
                                hist_recall--;
                            }
                            if(hist_recall >= 0 && hist_recall < g_hist_count){
                                int hlen = strlen_(g_hist[hist_recall]);
                                term.set_line(g_hist[hist_recall], hlen);
                                term.get_line(inbuf, &inlen);
                            }
                        } else {
                            // Scrolled back: scroll view up
                            term.scroll_view(-1);
                        }
                    } else if(e.type==K_DOWN){
                        // PowerShell-style: Down goes to next command in history
                        if(term.is_at_bottom() && g_mode == MODE_NORMAL && hist_recall >= 0){
                            if(hist_recall < g_hist_count - 1){
                                hist_recall++;
                                int hlen = strlen_(g_hist[hist_recall]);
                                term.set_line(g_hist[hist_recall], hlen);
                                term.get_line(inbuf, &inlen);
                            } else {
                                // At the latest: clear the line
                                hist_recall = -1;
                                term.set_line("", 0);
                                term.get_line(inbuf, &inlen);
                            }
                        } else {
                            // Scrolled back: scroll view down
                            term.scroll_view(1);
                        }
                    } else if(e.type==K_LEFT){
                        // Left arrow: move cursor left
                        term.snap_bottom();
                        term.cursor_left();
                    } else if(e.type==K_RIGHT){
                        // Right arrow: move cursor right
                        term.snap_bottom();
                        term.cursor_right();
                    } else if(e.type==K_PAGEUP){
                        // Page Up: scroll view up by 10 lines
                        term.scroll_view(-10);
                    } else if(e.type==K_PAGEDN){
                        // Page Down: scroll view down by 10 lines
                        term.scroll_view(10);
                    } else if(e.type==K_HOME){
                        // Home: move cursor to start of input
                        term.snap_bottom();
                        term.cursor_home();
                    } else if(e.type==K_END){
                        // End: move cursor to end of input
                        term.snap_bottom();
                        term.cursor_end();
                    }
                }
            }
        }
        inbuf[inlen]=0;

        if (g_mode == MODE_WRITE) {
            if (inlen == 0) {
                // Empty line = save and exit write mode
                int ret = mkfs.create(g_write_name, g_writebuf, g_write_len);
                if (ret >= 0) {
                    term.write("Saved: "); term.write(g_write_name);
                    term.write(" ("); term.write_dec(g_write_len); term.write(" bytes)\n");
                } else {
                    term.write("Save failed (code "); term.write_dec(ret); term.write(")\n");
                }
                g_mode = MODE_NORMAL;
                g_write_len = 0;
            } else {
                if (g_write_len + inlen + 1 < FS_WRITEBUF_SIZE) {
                    memcpy_(g_writebuf + g_write_len, inbuf, inlen);
                    g_write_len += inlen;
                    g_writebuf[g_write_len++] = '\n';
                } else {
                    term.write("Buffer full! Auto-saving...\n");
                    mkfs.create(g_write_name, g_writebuf, g_write_len);
                    term.write("Saved: "); term.write(g_write_name);
                    term.write(" ("); term.write_dec(g_write_len); term.write(" bytes)\n");
                    g_mode = MODE_NORMAL;
                    g_write_len = 0;
                }
            }
        } else {
            run_command(inbuf);
        }
        term.render();
    }
}
