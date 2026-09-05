// =====================================================================
//  kernel.cpp  -  Freestanding C++ kernel with shell, mouse & scrollback
// ---------------------------------------------------------------------
//  Runs in 32-bit protected mode, no standard library.
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
#include "win32.h"        // Win32 subsystem: registry, PE32 loader, GUI bridge
#include "linux_compat.h" // Linux binary-compat shim (Wine-on-NexOS Milestone 0)
#include "mm.h"           // Foundation 0: USER_BASE/USER_END ring-3 region bounds
#include "gdt.h"          // Foundation 0: ring-3 GDT/TSS
#include "syscall.h"      // Foundation 0: unified int 0x80 syscall ABI
#include "proc.h"         // Foundation 0: process table
#include "vfs.h"          // Foundation 0: sandboxed virtual filesystem
#include "perm.h"         // Security 3.3: Y/N permission prompt engine
#include "clr.h"          // MiniCLR: CIL interpreter for Roslyn-compiled C# apps
#include "ai_model.h"     // open-source model recognition + registry
#include "ai_env.h"       // VM vs bare-metal detection
#include "skill.h"        // P4: AI skill registry + intent dispatch
#include "addrman.h"       // Address Management Registry (single source of truth)
#include "ai_plugin.h"      // NexOS plugin catalogue / manager
#include "distnet.h"        // Minimal distributed compute network (distnet.cpp)

// =====================================================================
//  Port I/O helpers
// =====================================================================
static inline uint8_t  inb(uint16_t p){ uint8_t v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p){ uint16_t v; __asm__ __volatile__("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p){ uint32_t v; __asm__ __volatile__("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outb(uint16_t p,uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void outw(uint16_t p,uint16_t v){ __asm__ __volatile__("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void outl(uint16_t p,uint32_t v){ __asm__ __volatile__("outl %0,%1"::"a"(v),"Nd"(p)); }

// =====================================================================
//  Serial debug output (port 0x3F8)  -  for UEFI boot tracing
// =====================================================================
static void serial_putc(char c){ outb(0x3F8, (uint8_t)c); }
static void serial_puts(const char* s){ while(*s) outb(0x3F8, (uint8_t)*s++); }
// Print a non-negative integer in decimal to the serial port.
static void serial_puts_dec(int v){
    if (v == 0){ serial_puts("0"); return; }
    if (v < 0){ serial_puts("-"); v = -v; }
    char t[12]; int n = 0;
    while (v > 0 && n < 11){ t[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = n - 1; i >= 0; i--) outb(0x3F8, (uint8_t)t[i]);
}
static void serial_hex(uint32_t v){
    const char* H = "0123456789ABCDEF";
    char buf[9];
    for (int i = 0; i < 8; i++) buf[i] = H[(v >> (28 - i*4)) & 0xF];
    buf[8] = 0;
    serial_puts(buf);
}
// Non-blocking serial read: returns char if data ready, else -1.
static int serial_try_getc(void){
    if (inb(0x3FD) & 0x01) return (int)(unsigned char)inb(0x3F8);
    return -1;
}

static void serial_init(void){
    outb(0x3F8 + 1, 0x00);   // IER: disable interrupts
    outb(0x3F8 + 3, 0x80);   // LCR: DLAB=1
    outb(0x3F8 + 0, 0x01);   // DLL: divisor low (115200 baud)
    outb(0x3F8 + 1, 0x00);   // DLM: divisor high
    outb(0x3F8 + 3, 0x03);   // LCR: 8N1, DLAB=0
    outb(0x3F8 + 2, 0xC7);   // FCR: enable FIFO, clear, 14-byte trig
    outb(0x3F8 + 4, 0x0B);   // MCR: DTR/RTS/OUT2
}
// Remote console buffer: commands arriving over COM1 (frontend ops terminal /
// bridge) are accumulated here and dispatched to run_command() on newline.
static char g_serial_inbuf[256];
static int  g_serial_inlen = 0;

// =====================================================================
//  Hardware Detection Module
//  Auto-detects CPU, memory, display, input, disk, and network
//  Adapts to any x86 PC hardware (like Windows 1.0 hardware abstraction)
// =====================================================================

struct HwInfo {
    // CPU
    char     cpu_vendor[13];     // 12-char vendor string + null
    char     cpu_brand[49];      // 48-char brand string + null
    uint32_t cpu_family;
    uint32_t cpu_model;
    uint32_t cpu_stepping;
    bool     has_fpu;
    bool     has_sse;
    bool     has_sse2;
    bool     has_sse3;
    bool     has_long_mode;      // x86-64 capable
    uint32_t cpu_features_edx;
    uint32_t cpu_features_ecx;
    bool     cpu_detected;

    // Memory
    uint32_t mem_total_kb;
    uint32_t mem_e820_entries;
    bool     mem_e820_available;

    // Display
    uint32_t vbe_width;
    uint32_t vbe_height;
    uint32_t vbe_bpp;
    uint32_t vbe_pitch;
    uint32_t vbe_fb_phys;
    bool     vbe_available;
    bool     vbe_mode_set;       // whether we set the mode ourselves

    // Input
    bool     keyboard_present;
    bool     mouse_present;
    uint8_t  mouse_type;         // 0=none, 1=PS/2 standard, 2=Intellimouse

    // Disk
    bool     disk_present;
    uint32_t disk_sectors;       // total LBA sectors
    uint32_t disk_size_mb;
    char     disk_model[41];

    // Network
    bool     nic_present;
    uint16_t nic_io_base;
    uint8_t  nic_type;           // 0=none, 1=NE2000, 2=PCI detected

    // PCI
    uint32_t pci_devices_found;
};

static HwInfo g_hw = {0};

// ---- CPUID inline assembly ----
static inline void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d){
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf)
    );
}

static inline void cpuidex(uint32_t leaf, uint32_t subleaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d){
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(subleaf)
    );
}

static inline uint64_t read_msr(uint32_t msr){
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

// ---- Detect CPU features via CPUID ----
static void detect_cpu(){
    g_hw.cpu_detected = false;

    // Check if CPUID is available (EFLAGS bit 21 can be toggled)
    uint32_t eflags;
    __asm__ __volatile__(
        "pushfl\n\t"
        "popl %0\n\t"
        : "=r"(eflags)
    );
    uint32_t old = eflags;
    eflags ^= 0x200000;  // toggle ID bit
    __asm__ __volatile__(
        "push %0\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %0\n\t"
        : "=r"(eflags)
        : "0"(eflags)
    );
    // Restore original EFLAGS
    __asm__ __volatile__("push %0\n\tpopfl" :: "r"(old));

    if ((eflags & 0x200000) == (old & 0x200000)){
        serial_puts("[HW] CPUID not supported\n");
        return;
    }

    // CPUID leaf 0: vendor string
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    *(uint32_t*)(g_hw.cpu_vendor)     = b;
    *(uint32_t*)(g_hw.cpu_vendor + 4) = d;
    *(uint32_t*)(g_hw.cpu_vendor + 8) = c;
    g_hw.cpu_vendor[12] = 0;

    // CPUID leaf 1: family, model, stepping, features
    cpuid(1, &a, &b, &c, &d);
    g_hw.cpu_stepping  = a & 0xF;
    g_hw.cpu_model     = (a >> 4) & 0xF;
    g_hw.cpu_family    = (a >> 8) & 0xF;
    // Extended family/model for modern CPUs
    uint32_t ext_family = (a >> 20) & 0xFF;
    uint32_t ext_model  = (a >> 16) & 0xF;
    if (g_hw.cpu_family == 0xF){
        g_hw.cpu_family += ext_family;
        g_hw.cpu_model  += (ext_model << 4);
    }

    g_hw.cpu_features_edx = d;
    g_hw.cpu_features_ecx = c;
    g_hw.has_fpu  = (d & (1 << 0))  != 0;
    g_hw.has_sse  = (d & (1 << 25)) != 0;
    g_hw.has_sse2 = (d & (1 << 26)) != 0;
    g_hw.has_sse3 = (c & (1 << 0))  != 0;

    // CPUID leaf 0x80000000: max extended leaf
    uint32_t max_ext;
    cpuid(0x80000000, &max_ext, &b, &c, &d);

    // Check long mode (x86-64) support
    if (max_ext >= 0x80000001){
        cpuid(0x80000001, &a, &b, &c, &d);
        g_hw.has_long_mode = (d & (1 << 29)) != 0;
    }

    // CPU brand string (leaf 0x80000002-0x80000004)
    if (max_ext >= 0x80000004){
        uint32_t* brand = (uint32_t*)g_hw.cpu_brand;
        cpuid(0x80000002, &a, &b, &c, &d);
        brand[0]=a; brand[1]=b; brand[2]=c; brand[3]=d;
        cpuid(0x80000003, &a, &b, &c, &d);
        brand[4]=a; brand[5]=b; brand[6]=c; brand[7]=d;
        cpuid(0x80000004, &a, &b, &c, &d);
        brand[8]=a; brand[9]=b; brand[10]=c; brand[11]=d;
        g_hw.cpu_brand[48] = 0;
    } else {
        g_hw.cpu_brand[0] = 0;
    }

    g_hw.cpu_detected = true;

    serial_puts("[HW] CPU: ");
    serial_puts(g_hw.cpu_vendor);
    serial_puts(" Family=");
    {
        char buf[12];
        int i = 0;
        uint32_t v = g_hw.cpu_family;
        if (v == 0) { buf[i++] = '0'; }
        else { char t[10]; int j=0; while(v){t[j++]='0'+v%10;v/=10;} while(j)buf[i++]=t[--j]; }
        buf[i] = 0;
        serial_puts(buf);
    }
    serial_puts(" Model=");
    {
        char buf[12];
        int i = 0;
        uint32_t v = g_hw.cpu_model;
        if (v == 0) { buf[i++] = '0'; }
        else { char t[10]; int j=0; while(v){t[j++]='0'+v%10;v/=10;} while(j)buf[i++]=t[--j]; }
        buf[i] = 0;
        serial_puts(buf);
    }
    serial_puts(g_hw.has_long_mode ? " [x86-64]" : " [32-bit]");
    serial_puts(g_hw.has_sse2 ? " [SSE2]" : "");
    serial_puts("\n");
}

// ---- E820 memory map detection (BIOS path) ----
// E820 entry structure (matches INT 15h E820 format)
struct __attribute__((packed)) E820Entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;      // 1=usable, 2=reserved, 3=ACPI, 4=NVS, 5=unusable
};

static E820Entry g_e820_entries[32];

static void detect_memory_e820(){
    g_hw.mem_e820_entries = 0;
    g_hw.mem_e820_available = false;

    // INT 15h E820 is only available in real mode.
    // In protected mode (UEFI or after stage2), we can't call BIOS INT 15h.
    // For BIOS path: stage2 should have stored E820 at a known address.
    // For UEFI path: we use the memory map from GetMemoryMap (already consumed).

    // Check if stage2 left E820 data at 0x6000 (convention)
    volatile uint32_t* e820_count = (volatile uint32_t*)0x6000;
    volatile E820Entry* e820_entries = (volatile E820Entry*)0x6004;

    if (*e820_count > 0 && *e820_count <= 32){
        g_hw.mem_e820_entries = *e820_count;
        for (uint32_t i = 0; i < *e820_count && i < 32; i++){
            g_e820_entries[i].base   = e820_entries[i].base;
            g_e820_entries[i].length = e820_entries[i].length;
            g_e820_entries[i].type   = e820_entries[i].type;
        }
        g_hw.mem_e820_available = true;

        // Calculate total usable memory from E820
        uint64_t total = 0;
        for (uint32_t i = 0; i < g_hw.mem_e820_entries; i++){
            if (g_e820_entries[i].type == 1){  // usable
                total += g_e820_entries[i].length;
            }
        }
        g_hw.mem_total_kb = (uint32_t)(total / 1024);

        serial_puts("[HW] E820: ");
        {
            char buf[12]; int i=0; uint32_t v=g_hw.mem_e820_entries;
            if(v==0){buf[i++]='0';} else{char t[10];int j=0;while(v){t[j++]='0'+v%10;v/=10;}while(j)buf[i++]=t[--j];}
            buf[i]=0; serial_puts(buf);
        }
        serial_puts(" entries, ");
        {
            char buf[12]; int i=0; uint32_t v=g_hw.mem_total_kb/1024;
            if(v==0){buf[i++]='0';} else{char t[10];int j=0;while(v){t[j++]='0'+v%10;v/=10;}while(j)buf[i++]=t[--j];}
            buf[i]=0; serial_puts(buf);
        }
        serial_puts(" MiB usable\n");
    } else {
        serial_puts("[HW] E820 not available, using CMOS fallback\n");
    }
}

// ---- Detect PS/2 controller and mouse ----
static void detect_input_devices(){
    // Check if PS/2 controller exists (port 0x64 status byte)
    uint8_t status = inb(0x64);

    // REAL HARDWARE GUARD:
    // Modern laptops booted in pure UEFI mode (no CSM) frequently have no
    // legacy i8042 controller at all - the keyboard is an I2C-HID / USB HID
    // device owned by the firmware. Reading an unimplemented ISA port then
    // returns 0xFF (floating bus), which makes *every* status-bit test below
    // evaluate true and turns the polling loops into infinite loops.
    // Emulators (QEMU/VirtualBox) always emulate i8042, so this never shows
    // up in a VM - only on real machines, where the kernel simply hangs.
    if (status == 0xFF){
        g_hw.keyboard_present = false;
        g_hw.mouse_present    = false;
        g_hw.mouse_type       = 0;
        serial_puts("[HW] PS/2 controller: absent (port 0x64 reads 0xFF)\n");
        return;
    }

    // Test if controller responds - write command and check response
    outb(0x64, 0xAA);  // Controller self-test
    bool controller_ok = false;
    for (int i = 0; i < 1000; i++){
        if (inb(0x64) & 0x01){
            uint8_t result = inb(0x60);
            if (result == 0x55){  // 0x55 = self-test passed
                controller_ok = true;
            }
            break;
        }
    }

    g_hw.keyboard_present = controller_ok;
    serial_puts(controller_ok ? "[HW] PS/2 controller: OK\n" : "[HW] PS/2 controller: not detected\n");

    if (!controller_ok){
        g_hw.mouse_present = false;
        g_hw.mouse_type = 0;
        return;
    }

    // Enable auxiliary device (mouse)
    outb(0x64, 0xA8);

    // Test if mouse port works
    outb(0x64, 0xD4);  // Write to mouse
    // Wait for input buffer empty - MUST be bounded, a wedged/absent EC keeps
    // bit 1 set forever and would hang the kernel on real hardware.
    for (int g = 0; (inb(0x64) & 0x02) && g < 100000; g++) {}
    outb(0x60, 0xFF);  // Reset mouse
    bool mouse_ok = false;
    for (int i = 0; i < 5000; i++){
        if (inb(0x64) & 0x01){
            uint8_t r = inb(0x60);
            if (r == 0xFA || r == 0xAA){  // ACK or self-test passed
                mouse_ok = true;
                // Drain any remaining reset response
                for (int j = 0; j < 1000; j++){
                    if (inb(0x64) & 0x01) inb(0x60);
                    else break;
                }
                break;
            }
        }
    }

    g_hw.mouse_present = mouse_ok;
    g_hw.mouse_type = mouse_ok ? 2 : 0;  // Assume Intellimouse

    serial_puts(mouse_ok ? "[HW] PS/2 mouse: detected\n" : "[HW] PS/2 mouse: not detected\n");
}

// ---- PCI configuration space access ----
static uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset){
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val){
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

// ---- Scan PCI bus for devices ----
static void pci_scan(){
    g_hw.pci_devices_found = 0;

    // Check if PCI configuration mechanism #1 is available
    // Write to 0xCF8 and read back
    uint32_t test_val = 0x80000000;
    outl(0xCF8, test_val);
    uint32_t readback = inl(0xCF8);
    if (readback != test_val){
        serial_puts("[HW] PCI: configuration mechanism #1 not available\n");
        return;
    }

    serial_puts("[HW] PCI bus scan:\n");
    for (uint16_t bus = 0; bus < 256; bus++){
        for (uint8_t dev = 0; dev < 32; dev++){
            for (uint8_t func = 0; func < 8; func++){
                uint32_t id = pci_read_config(bus, dev, func, 0);
                uint16_t vendor = id & 0xFFFF;
                uint16_t device = (id >> 16) & 0xFFFF;

                if (vendor == 0xFFFF || vendor == 0x0000)
                    continue;

                g_hw.pci_devices_found++;

                uint32_t class_reg = pci_read_config(bus, dev, func, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class  = (class_reg >> 16) & 0xFF;

                // Log device
                serial_puts("[HW]   ");
                {
                    char buf[8];
                    const char* hex = "0123456789ABCDEF";
                    buf[0] = hex[(bus >> 4) & 0xF]; buf[1] = hex[bus & 0xF];
                    buf[2] = ':'; buf[3] = 0;
                    serial_puts(buf);
                    buf[0] = hex[(dev >> 4) & 0xF]; buf[1] = hex[dev & 0xF];
                    buf[2] = '.'; buf[3] = hex[func]; buf[4] = 0;
                    serial_puts(buf);
                }
                serial_puts(" vendor=");
                {
                    char buf[8]; const char* hex="0123456789ABCDEF";
                    buf[0]=hex[(vendor>>12)&0xF];buf[1]=hex[(vendor>>8)&0xF];
                    buf[2]=hex[(vendor>>4)&0xF];buf[3]=hex[vendor&0xF];buf[4]=0;
                    serial_puts(buf);
                }
                serial_puts(" dev=");
                {
                    char buf[8]; const char* hex="0123456789ABCDEF";
                    buf[0]=hex[(device>>12)&0xF];buf[1]=hex[(device>>8)&0xF];
                    buf[2]=hex[(device>>4)&0xF];buf[3]=hex[device&0xF];buf[4]=0;
                    serial_puts(buf);
                }
                serial_puts(" class=");
                {
                    char buf[8]; const char* hex="0123456789ABCDEF";
                    buf[0]=hex[(base_class>>4)&0xF];buf[1]=hex[base_class&0xF];
                    buf[2]=hex[(sub_class>>4)&0xF];buf[3]=hex[sub_class&0xF];buf[4]=0;
                    serial_puts(buf);
                }
                serial_puts("\n");

                // Check for network controllers (class 0x02xx)
                if (base_class == 0x02 && !g_hw.nic_present){
                    g_hw.nic_type = 2;
                    // Read BAR0 for I/O base
                    uint32_t bar0 = pci_read_config(bus, dev, func, 0x10);
                    if (bar0 & 1){
                        g_hw.nic_io_base = bar0 & 0xFFFC;
                        g_hw.nic_present = true;
                        serial_puts("[HW]   -> Network controller at I/O 0x");
                        {
                            char buf[8]; const char* hex="0123456789ABCDEF";
                            buf[0]=hex[(g_hw.nic_io_base>>12)&0xF];buf[1]=hex[(g_hw.nic_io_base>>8)&0xF];
                            buf[2]=hex[(g_hw.nic_io_base>>4)&0xF];buf[3]=hex[g_hw.nic_io_base&0xF];buf[4]=0;
                            serial_puts(buf);
                        }
                        serial_puts("\n");
                    }
                    // Enable I/O space and bus master
                    uint32_t cmd = pci_read_config(bus, dev, func, 0x04);
                    pci_write_config(bus, dev, func, 0x04, cmd | 0x07);
                }

                // Check for display controllers (class 0x03xx)
                if (base_class == 0x03){
                    serial_puts("[HW]   -> Display controller detected\n");
                }

                // Check for storage controllers (class 0x01xx)
                if (base_class == 0x01){
                    serial_puts("[HW]   -> Mass storage controller\n");
                }

                // Multi-function device check
                if (func == 0){
                    uint32_t hdr = pci_read_config(bus, dev, func, 0x0C);
                    if (!((hdr >> 16) & 0x80))
                        break;  // Not multi-function, skip other functions
                }
            }
        }
        // Early exit if we've scanned enough
        if (g_hw.pci_devices_found > 0 && bus > 2)
            break;
    }

    {
        serial_puts("[HW] PCI: ");
        char buf[12]; int i=0; uint32_t v=g_hw.pci_devices_found;
        if(v==0){buf[i++]='0';} else{char t[10];int j=0;while(v){t[j++]='0'+v%10;v/=10;}while(j)buf[i++]=t[--j];}
        buf[i]=0; serial_puts(buf);
        serial_puts(" devices found\n");
    }
}

// ---- ATA IDENTIFY command for disk detection ----
static void detect_disk(){
    g_hw.disk_present = false;
    g_hw.disk_sectors = 0;
    g_hw.disk_size_mb = 0;
    g_hw.disk_model[0] = 0;

    // Try primary master first, then primary slave, then secondary
    uint16_t io_ports[] = { 0x1F0, 0x1F0, 0x170, 0x170 };
    uint8_t  drives[]   = { 0xE0, 0xF0, 0xE0, 0xF0 };

    for (int i = 0; i < 4; i++){
        uint16_t base = io_ports[i];
        uint8_t drv = drives[i];

        // Select drive
        outb(base + 6, drv);
        for (volatile int j = 0; j < 1000; j++);

        // Check if controller exists
        outb(base + 2, 0x55);
        if (inb(base + 2) != 0x55){
            continue;  // No controller at this base
        }
        outb(base + 2, 0xAA);
        if (inb(base + 2) != 0xAA){
            continue;
        }

        // Send IDENTIFY command
        outb(base + 6, drv);
        outb(base + 1, 0);
        outb(base + 2, 0);
        outb(base + 3, 0);
        outb(base + 4, 0);
        outb(base + 5, 0);
        outb(base + 7, 0xEC);  // IDENTIFY DEVICE

        // Wait for DRQ
        bool ok = false;
        for (int j = 0; j < 100000; j++){
            uint8_t status = inb(base + 7);
            if (status & 0x01) break;  // ERR - not a disk or ATAPI
            if (status & 0x08){ ok = true; break; }  // DRQ
        }

        if (!ok) continue;

        // Read 256 words of IDENTIFY data
        uint16_t id_buf[256];
        for (int j = 0; j < 256; j++){
            id_buf[j] = inw(base + 0);
        }

        // Check if this is a valid device
        if (id_buf[0] == 0xFFFF || id_buf[0] == 0x0000) continue;

        g_hw.disk_present = true;

        // Extract model name (words 27-46, byte-swapped)
        for (int j = 0; j < 40; j += 2){
            g_hw.disk_model[j]   = (id_buf[27 + j/2] >> 8) & 0xFF;
            g_hw.disk_model[j+1] = id_buf[27 + j/2] & 0xFF;
        }
        g_hw.disk_model[40] = 0;

        // Total sectors (LBA28: words 60-61, LBA48: words 100-103)
        uint32_t lba28_sectors = id_buf[60] | ((uint32_t)id_buf[61] << 16);
        uint64_t lba48_sectors = 0;
        if (id_buf[83] & (1 << 10)){  // LBA48 supported
            lba48_sectors = (uint64_t)id_buf[100] | ((uint64_t)id_buf[101] << 16)
                          | ((uint64_t)id_buf[102] << 32) | ((uint64_t)id_buf[103] << 48);
        }

        if (lba48_sectors > 0){
            g_hw.disk_sectors = (uint32_t)(lba48_sectors > 0xFFFFFFFF ? 0xFFFFFFFF : lba48_sectors);
        } else {
            g_hw.disk_sectors = lba28_sectors;
        }

        g_hw.disk_size_mb = g_hw.disk_sectors / (1024 * 1024 / 512);

        serial_puts("[HW] Disk: ");
        serial_puts(g_hw.disk_model);
        serial_puts(" (");
        {
            char buf[12]; int i=0; uint32_t v=g_hw.disk_size_mb;
            if(v==0){buf[i++]='0';} else{char t[10];int j=0;while(v){t[j++]='0'+v%10;v/=10;}while(j)buf[i++]=t[--j];}
            buf[i]=0; serial_puts(buf);
        }
        serial_puts(" MB)\n");

        break;  // Use first found disk
    }

    if (!g_hw.disk_present){
        serial_puts("[HW] Disk: none detected\n");
    }
}

// ---- Master hardware detection function ----
static void detect_hardware(){
    serial_puts("[HW] ===== Hardware Detection =====\n");

    detect_cpu();
    detect_memory_e820();
    detect_input_devices();
    pci_scan();
    detect_disk();

    // Read VBE info from 0x5000 (set by stage2 or UEFI bootloader)
    // Layout: 0x5000=fb_phys(4) 0x5004=width(2) 0x5006=height(2)
    //         0x5008=bpp(1) 0x5009=pitch(2) 0x500B=mode(2)
    //         0x500D=vbe_ok(1) 0x500E=mode_set(1) 0x500F=pixel_format(1)
    //         0x5010=fb_phys64(8) 0x5018=shadow_buffer(4)
    volatile uint8_t* vbe_raw = (volatile uint8_t*)0x5000;
    if (vbe_raw[0x0D] == 1){  // vbe_ok
        g_hw.vbe_fb_phys   = *(volatile uint32_t*)(vbe_raw + 0x00);
        g_hw.vbe_width     = *(volatile uint16_t*)(vbe_raw + 0x04);
        g_hw.vbe_height    = *(volatile uint16_t*)(vbe_raw + 0x06);
        g_hw.vbe_bpp       = *(volatile uint8_t*)(vbe_raw + 0x08);
        g_hw.vbe_pitch     = *(volatile uint16_t*)(vbe_raw + 0x09);
        g_hw.vbe_available = true;
        // Read vbe_mode_set flag (0x500E): 1 = mode set by BIOS INT 10h or UEFI GOP
        g_hw.vbe_mode_set = (vbe_raw[0x0E] == 1);

        // Read new UEFI GOP fields
        uint8_t px_fmt = vbe_raw[0x0F];  // pixel_format
        uint64_t fb_phys64 = *(volatile uint64_t*)(vbe_raw + 0x10);
        uint32_t shadow_buf = *(volatile uint32_t*)(vbe_raw + 0x18);

        serial_puts("[HW] VBE: available ");
        {
            char buf[12]; int i=0; uint32_t v=g_hw.vbe_width;
            if(v==0){buf[i++]='0';} else{char t[10];int j=0;while(v){t[j++]='0'+v%10;v/=10;}while(j)buf[i++]=t[--j];}
            buf[i]=0; serial_puts(buf);
        }
        serial_puts("x");
        {
            char buf[12]; int i=0; uint32_t v=g_hw.vbe_height;
            if(v==0){buf[i++]='0';} else{char t[10];int j=0;while(v){t[j++]='0'+v%10;v/=10;}while(j)buf[i++]=t[--j];}
            buf[i]=0; serial_puts(buf);
        }
        serial_puts(g_hw.vbe_mode_set ? " [mode_set]" : " [mode_not_set]");
        // Print pixel format and framebuffer address for debugging
        {
            serial_puts(" fmt=");
            const char* fmt_names[] = {"BGRX32","RGBX32","RGB24","RGB565","BltOnly"};
            if (px_fmt < 5) serial_puts(fmt_names[px_fmt]);
            else { serial_puts("?"); }

            // Warn if framebuffer is above 4GB without shadow buffer
            if (fb_phys64 > 0xFFFFFFFFULL && shadow_buf == 0) {
                serial_puts(" [WARN:FB>4GB no shadow]");
            }
        }
        serial_puts("\n");
    } else {
        g_hw.vbe_available = false;
        g_hw.vbe_mode_set = false;
        serial_puts("[HW] VBE: not available\n");
    }

    serial_puts("[HW] ===== Detection Complete =====\n");
}

// =====================================================================
//  Tiny libc (freestanding)
// =====================================================================
static int   strlen_(const char* s){ int n=0; while(s[n]) n++; return n; }
static int   strcmp_(const char* a,const char* b){ while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
static int   strncmp_(const char* a,const char* b,int n){ while(n>0&&*a&&*a==*b){a++;b++;n--;} return n==0?0:(unsigned char)*a-(unsigned char)*b; }
static bool  startswith_(const char* s,const char* prefix){ while(*prefix){ if(*s!=*prefix) return false; s++; prefix++; } return true; }
static void* memset_(void* d,int v,int n){ unsigned char* p=(unsigned char*)d; while(n--) *p++=(unsigned char)v; return d; }
static void* memcpy_(void* d,const void* s,int n){ unsigned char* dp=(unsigned char*)d; const unsigned char* sp=(const unsigned char*)s; while(n--) *dp++=*sp++; return d; }

// Freestanding memcpy/memset/memmove for compiler-generated builtin calls (-O2)
extern "C" void* memcpy(void* d, const void* s, unsigned int n){
    unsigned char* dp=(unsigned char*)d; const unsigned char* sp=(const unsigned char*)s;
    while(n--) *dp++=*sp++; return d;
}
extern "C" void* memset(void* d, int v, unsigned int n){
    unsigned char* p=(unsigned char*)d; while(n--) *p++=(unsigned char)v; return d;
}
extern "C" void* memmove(void* d, const void* s, unsigned int n){
    unsigned char* dp=(unsigned char*)d; const unsigned char* sp=(const unsigned char*)s;
    if(dp<sp){ while(n--) *dp++=*sp++; }
    else { dp+=n; sp+=n; while(n--) *--dp=*--sp; }
    return d;
}
static void  int_to_str(int val, char* buf){ if(val==0){buf[0]='0';buf[1]=0;return;} char tmp[16]; int i=0; bool neg=val<0; if(neg)val=-val; while(val>0){tmp[i++]='0'+(val%10);val/=10;} int j=0; if(neg)buf[j++]='-'; while(i>0)buf[j++]=tmp[--i]; buf[j]=0; }
static void  uint_to_str(uint32_t val, char* buf){ if(val==0){buf[0]='0';buf[1]=0;return;} char tmp[16]; int i=0; while(val>0){tmp[i++]='0'+(val%10);val/=10;} int j=0; while(i>0)buf[j++]=tmp[--i]; buf[j]=0; }

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
    void  agent_plan(const char* goal);
    int   agent_run(const char* goal, char* output, int outsize);
    int   agent_get_status(char* buf, int bufsize);
    void  agent_abort(void);
    void  agent_set_confirm(int on);
}
static bool g_ai_initialized = false;

// =====================================================================
//  Network stack interface (implemented in net.cpp)
// =====================================================================
extern "C" {
    int  net_init(void);
    void net_poll(void);
    const char* net_ip_str(void);
    int  net_status(char* buf, int bufsize);
    // Browser HTTP client API
    int  browser_navigate(const char* url);
    int  browser_status(void);
    int  browser_get_page(char* buf, int bufsize);
    void browser_reset(void);
    const char* browser_content_type(void);
    int  browser_status_code(void);
    // Synchronous HTTP GET for the managed Browser control.
    int  net_http_get(const char* url, char* out, int outsize);
    // Ask a question through the host-side LLM bridge (10.0.2.2:18080).
    int  net_ask_host(const char* question, char* out, int outsize);
    // ICMP ping client: returns 1 if any attempt got a reply, 0 on timeout.
    int  net_ping(const char* host, int attempts);
    // WiFi manager (control plane) + time-server client (net.cpp)
    int  net_wifi_scan(char* out, int n);
    int  net_wifi_connect(const char* arg, char* out, int n);
    int  net_wifi_disconnect(char* out, int n);
    int  net_wifi_status(char* out, int n);
    int  net_time(char* out, int n);
}
static bool g_net_initialized = false;

// =====================================================================
//  ATA PIO disk driver (LBA28) with primary/secondary channel support
//  dev 0 = primary (0x1F0)  - boot disk (or ISO/SFS carrier)
//  dev 1 = secondary (0x170) - dedicated user data disk (VHD)
// =====================================================================
static bool ata_wait_bsy_base(uint16_t base){
    for(int i=0;i<100000;i++){ if(!(inb(base+7)&0x80)) return true; }
    return false;  // timeout (no disk)
}
static bool ata_wait_drq_base(uint16_t base){
    for(int i=0;i<100000;i++){ if(inb(base+7)&0x08) return true; }
    return false;  // timeout (no disk)
}
static bool ata_wait_bsy(){ return ata_wait_bsy_base(0x1F0); }
static bool ata_wait_drq(){ return ata_wait_drq_base(0x1F0); }

static void ata_read_sector_dev(int dev, uint32_t lba, uint16_t* buf){
    uint16_t base = dev ? 0x170 : 0x1F0;
    if(!ata_wait_bsy_base(base)) return;
    outb(base+6, 0xE0 | ((lba>>24)&0x0F));
    outb(base+1, 0x00);
    outb(base+2, 1);
    outb(base+3, lba & 0xFF);
    outb(base+4, (lba>>8) & 0xFF);
    outb(base+5, (lba>>16) & 0xFF);
    outb(base+7, 0x20);                 // READ SECTORS
    if(!ata_wait_bsy_base(base)) return;
    if(!ata_wait_drq_base(base)) return;
    for(int i=0;i<256;i++) buf[i]=inw(base);
}
static void ata_write_sector_dev(int dev, uint32_t lba, const uint16_t* buf){
    uint16_t base = dev ? 0x170 : 0x1F0;
    if(!ata_wait_bsy_base(base)) return;
    outb(base+6, 0xE0 | ((lba>>24)&0x0F));
    outb(base+1, 0x00);
    outb(base+2, 1);
    outb(base+3, lba & 0xFF);
    outb(base+4, (lba>>8) & 0xFF);
    outb(base+5, (lba>>16) & 0xFF);
    outb(base+7, 0x30);                 // WRITE SECTORS
    if(!ata_wait_drq_base(base)) return;
    for(int i=0;i<256;i++) outw(base, buf[i]);
    outb(base+7, 0xE7);                 // CACHE FLUSH
    ata_wait_bsy_base(base);
}

static void ata_read_sector(uint32_t lba, uint16_t* buf){ ata_read_sector_dev(0, lba, buf); }
static void ata_write_sector(uint32_t lba, const uint16_t* buf){ ata_write_sector_dev(0, lba, buf); }

// ---- Read/write with explicit ATA base port + drive select ----
static void ata_read_sector_disk(uint16_t base, uint8_t drv, uint32_t lba, uint16_t* buf){
    if(!ata_wait_bsy_base(base)) return;
    outb(base+6, drv | ((lba>>24)&0x0F));
    outb(base+1, 0x00);
    outb(base+2, 1);
    outb(base+3, lba & 0xFF);
    outb(base+4, (lba>>8) & 0xFF);
    outb(base+5, (lba>>16) & 0xFF);
    outb(base+7, 0x20);                 // READ SECTORS
    if(!ata_wait_bsy_base(base)) return;
    if(!ata_wait_drq_base(base)) return;
    for(int i=0;i<256;i++) buf[i]=inw(base);
}
static void ata_write_sector_disk(uint16_t base, uint8_t drv, uint32_t lba, const uint16_t* buf){
    if(!ata_wait_bsy_base(base)) return;
    outb(base+6, drv | ((lba>>24)&0x0F));
    outb(base+1, 0x00);
    outb(base+2, 1);
    outb(base+3, lba & 0xFF);
    outb(base+4, (lba>>8) & 0xFF);
    outb(base+5, (lba>>16) & 0xFF);
    outb(base+7, 0x30);                 // WRITE SECTORS
    if(!ata_wait_drq_base(base)) return;
    for(int i=0;i<256;i++) outw(base, buf[i]);
    outb(base+7, 0xE7);                 // CACHE FLUSH
    ata_wait_bsy_base(base);
}

// ---- Find the first ATA HARD disk (excludes ATAPI CD-ROMs) ----
// Returns true + fills base/drv if a hard disk exists on any IDE slot.
static bool ata_find_hdd(uint16_t* out_base, uint8_t* out_drv){
    uint16_t bases[] = {0x1F0, 0x170};
    uint8_t  drvs[]  = {0xE0, 0xF0};
    for(int b = 0; b < 2; b++){
        for(int d = 0; d < 2; d++){
            uint16_t base = bases[b];
            uint8_t drv = drvs[d];
            // Controller presence check
            outb(base+2, 0x55);
            if(inb(base+2) != 0x55) continue;
            outb(base+2, 0xAA);
            if(inb(base+2) != 0xAA) continue;
            // IDENTIFY DEVICE
            outb(base+6, drv);
            outb(base+1, 0);
            outb(base+2, 0);
            outb(base+3, 0);
            outb(base+4, 0);
            outb(base+5, 0);
            outb(base+7, 0xEC);
            for(int j = 0; j < 100000; j++){
                uint8_t st = inb(base+7);
                if(st & 0x01) break;              // ERR / ATAPI (CD-ROM) - not a hard disk
                if(st & 0x08){                     // DRQ ready
                    // Read the FULL IDENTIFY block to clear DRQ, then decide.
                    uint16_t id[256];
                    for(int k = 0; k < 256; k++) id[k] = inw(base);
                    if(id[0] == 0xFFFF || id[0] == 0x0000) break;
                    if(id[0] & 0x8000) break;      // word0 bit15: ATAPI device (CD-ROM)
                    *out_base = base;
                    *out_drv  = drv;
                    return true;
                }
            }
        }
    }
    return false;
}

// ---- File-system device abstraction (used by MKFS) ----
// The data disk: the first ATA hard disk found. When none exists
// (e.g. old single-disk setups), fall back to primary master.
static uint16_t g_fs_base = 0x1F0;
static uint8_t  g_fs_drv  = 0xE0;
static bool     g_fs_is_data_disk = false;   // true when a dedicated data disk is used

static void fs_read_sector(uint32_t lba, uint16_t* buf){ ata_read_sector_disk(g_fs_base, g_fs_drv, lba, buf); }
static void fs_write_sector(uint32_t lba, const uint16_t* buf){ ata_write_sector_disk(g_fs_base, g_fs_drv, lba, buf); }

static void fs_detect_data_disk(){
    uint16_t base; uint8_t drv;
    if(ata_find_hdd(&base, &drv)){
        g_fs_base = base;
        g_fs_drv  = drv;
        g_fs_is_data_disk = true;
        serial_puts("[MKFS] Data disk: ATA ");
        serial_puts(base == 0x1F0 ? "primary" : "secondary");
        serial_puts(drv == 0xE0 ? " master\n" : " slave\n");
    } else {
        g_fs_base = 0x1F0;
        g_fs_drv  = 0xE0;
        g_fs_is_data_disk = false;
        serial_puts("[MKFS] No ATA hard disk - using boot disk (primary master)\n");
    }
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

char g_clipboard[CLIP_LEN];
int  g_clipboard_len = 0;
static char g_clip_hist[CLIP_HIST_MAX][CLIP_LEN];
static int  g_clip_hist_count = 0;
static int  g_clip_hist_idx = 0;

void clipboard_set(const char* text, int len){
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
extern "C" int gui_is_active(void);   // forward decl (defined in gui.cpp); used by Keyboard::process to resolve GUI vs text mode
namespace {

// ----- VGA constants -----
constexpr int VGA_WIDTH  = 80;
constexpr int VGA_HEIGHT = 25;
constexpr int SCROLLBACK_LINES = 200;

// Shadow VGA text buffer used when the firmware is in a VBE graphics mode.
// QEMU's Bochs-VBE only exposes the legacy VGA aperture at 0xA0000-0xAFFFF,
// so direct reads from 0xB8000 return 0xFF (white-on-white).  The terminal
// writes into this array instead, and fbcon reads from VGA_MEMORY which points
// here while VBE is active.
static uint16_t      VGA_SHADOW[VGA_WIDTH * VGA_HEIGHT];
static bool          g_vga_shadow_active = false;

}

// The terminal normally writes to the legacy VGA text buffer at 0xB8000.
// In VBE/fb_console mode it points to VGA_SHADOW.  This symbol has external
// linkage because gui.cpp's fb_console_render() reads it.
volatile uint16_t* VGA_MEMORY = reinterpret_cast<volatile uint16_t*>(0xB8000);

namespace {

enum VgaColor : uint8_t {
    BLACK=0, BLUE=1, GREEN=2, CYAN=3, RED=4, MAGENTA=5,
    BROWN=6, LIGHT_GREY=7, WHITE=15, YELLOW=14,
};
inline uint8_t  make_color(VgaColor fg, VgaColor bg){ return (uint8_t)(fg | (bg<<4)); }
inline uint16_t make_entry(unsigned char c, uint8_t color){ return (uint16_t)c | ((uint16_t)color<<8); }

struct Line { int len; char data[VGA_WIDTH]; };

// ---- GUI terminal command output capture ----
// When g_capturing is true, terminal output is also captured into g_exec_output
// so the GUI terminal can display command results.
static char g_exec_output[2048];
static int  g_exec_output_len;
static bool g_capturing;

// ---- terminal -> COM1 mirror ----
// Terminal::put_char writes only to VGA text memory, so headless runs
// (-vga none / -display none) had no way to observe shell output; only
// serial_puts() diagnostics reached COM1.  Mirroring every terminal
// character to 0x3F8 makes the whole shell scriptable from a test harness.
// Toggle at runtime with the `serialecho on|off` command.
static bool g_term_serial = true;

// ---- SSH output sink ----
// When a remote SSH command is executing, g_ssh_out_fn (if non-null) receives
// every terminal character so the output can be forwarded over the encrypted
// channel instead of (or in addition to) the local VGA/serial console.  Set by
// kernel_exec_line() around the run_command() call and cleared afterwards.
typedef void (*ssh_out_fn_t)(const char* data, int len);
static ssh_out_fn_t g_ssh_out_fn = 0;
static void ssh_out_dummy(const char*, int){}

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
    void set_color(uint8_t c){
        (void)c;  // 单色终端: 强制浅灰白字黑底, 忽略彩色高亮
        m_color=make_color(LIGHT_GREY,BLACK);
    }

    void clear_screen(){
        m_head=0; m_count=0; m_cur_len=0; m_cur_pos=0; m_prompt_len=0; m_at_bottom=true; m_view=0;
        for(int i=0;i<VGA_WIDTH*VGA_HEIGHT;i++) VGA_MEMORY[i]=make_entry(' ',m_color);
        render();
    }

    void put_char(char c){
        // Capture output for GUI terminal when in capture mode
        if(g_capturing && g_exec_output_len < (int)sizeof(g_exec_output)-1)
            g_exec_output[g_exec_output_len++] = c;
        // Forward to an active SSH channel if one is consuming shell output
        if(g_ssh_out_fn) g_ssh_out_fn(&c, 1);
        if(g_term_serial) serial_putc(c);
        if(c=='\n'){ commit_line(); return; }
        if(c=='\b'){
            // Backspace must never eat the prompt ("PS user@NexOS /path> "):
            // m_prompt_len marks where user input begins.  The old check
            // (m_cur_pos > 0) let an extra backspace start deleting the
            // prompt text.
            if(m_cur_pos > m_prompt_len){
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

    void write_hex(uint32_t v){
        put_char('0'); put_char('x');
        char b[8]; int i=0;
        if(v==0){ put_char('0'); return; }
        while(v){ uint8_t d=v&0xF; b[i++]=(d<10)?('0'+d):('A'+d-10); v>>=4; }
        while(i) put_char(b[--i]);
    }
    // Zero-padded full 64-bit hex (for >4GB physical addresses).
    void write_hex64(uint64_t v){
        put_char('0'); put_char('x');
        for (int i = 15; i >= 0; i--) {
            uint8_t d = (uint8_t)((v >> (i * 4)) & 0xF);
            put_char(d < 10 ? (char)('0' + d) : (char)('A' + d - 10));
        }
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
    void begin_input(){ m_prompt_len = m_cur_len; m_cur_pos = m_cur_len; reset_undo(); }
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
        push_undo();
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

    // ----- Undo (Ctrl+Z) -----
    // Snapshot the current input line before each edit so Ctrl+Z can revert
    // the last keystroke / paste / completion / history recall.
    void push_undo(){
        if(m_undo_sp < UNDO_MAX){
            int n = m_cur_len;
            if(n > VGA_WIDTH) n = VGA_WIDTH;
            for(int i=0;i<n;i++) m_undo_buf[m_undo_sp][i] = m_cur[i];
            m_undo_len[m_undo_sp] = n;
            m_undo_pos[m_undo_sp] = m_cur_pos;
            m_undo_sp++;
        }
    }
    bool pop_undo(){
        if(m_undo_sp > 0){
            m_undo_sp--;
            int n = m_undo_len[m_undo_sp];
            for(int i=0;i<n;i++) m_cur[i] = m_undo_buf[m_undo_sp][i];
            m_cur_len = n;
            m_cur_pos = m_undo_pos[m_undo_sp];
            if(m_cur_pos > m_cur_len) m_cur_pos = m_cur_len;
            render();
            return true;
        }
        return false;
    }
    void reset_undo(){ m_undo_sp = 0; }

    // ----- Select all (Ctrl+A) -----
    // Highlight the entire current input line (single screen row).  The
    // following Ctrl+C then copies it, or Backspace/Delete removes it.
    void select_all(){
        if(m_cur_len > m_prompt_len){
            int input_row = (m_count - m_view);
            m_sel_sy = m_sel_ey = input_row;
            m_sel_sx = m_prompt_len;
            m_sel_ex  = m_cur_len - 1;
            if(m_sel_ex > VGA_WIDTH-1) m_sel_ex = VGA_WIDTH-1;
            m_has_selection = true;
            m_selecting = false;
            render();
        }
    }

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

    // Undo stack (Ctrl+Z) - stored as raw input-line snapshots
    static const int UNDO_MAX = 64;
    char    m_undo_buf[UNDO_MAX][VGA_WIDTH];
    int     m_undo_len[UNDO_MAX];
    int     m_undo_pos[UNDO_MAX];
    int     m_undo_sp = 0;

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
    K_CTRL_Z, K_CTRL_A, K_CTRL_S,
    K_PAGEUP, K_PAGEDN, K_HOME, K_END, K_SHIFT,
    K_DESK_L, K_DESK_R, K_DESK_TGL
};
struct KbdEvent { KbdType type; char ch; };

enum ScanCode : uint8_t {
    SC_BACKSPACE=0x0E, SC_TAB=0x0F, SC_ENTER=0x1C, SC_LSHIFT=0x2A,
    SC_RSHIFT=0x36, SC_CAPSLOCK=0x3A, SC_SPACE=0x39,
    SC_LCTRL=0x1D, SC_LGUI=0x5B, SC_RGUI=0x5C,
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
            if(key==SC_LGUI||key==SC_RGUI) m_gui=false;
            m_ext=false;
            return e;
        }
        if(m_ext){
            m_ext=false;
            if(key==SC_LGUI||key==SC_RGUI){ m_gui=true; return e; }
            if(key==0x48){
                if(m_ctrl && (m_gui || gui_is_active())) e.type=K_DESK_TGL;
                else if(m_ctrl)                          e.type=K_CTRL_UP;
                else                                     e.type=K_UP;
            }
            else if(key==0x50){
                if(m_ctrl) e.type=K_CTRL_DOWN;
                else       e.type=K_DOWN;
            }
            // Virtual-desktop switch: Ctrl+arrows (no Win key needed while
            // the GUI desktop is active).  Win is still accepted so the old
            // Ctrl+Win+arrows combo keeps working; text mode (no GUI, no Win)
            // falls through to the plain arrow events so its Ctrl+arrow
            // shortcuts (cursor / clipboard history) are untouched.
            else if(key==0x4B) e.type = (m_ctrl && (m_gui || gui_is_active())) ? K_DESK_L : K_LEFT;
            else if(key==0x4D) e.type = (m_ctrl && (m_gui || gui_is_active())) ? K_DESK_R : K_RIGHT;
            else if(key==0x49) e.type=K_PAGEUP;
            else if(key==0x51) e.type=K_PAGEDN;
            else if(key==0x47) e.type=K_HOME;
            else if(key==0x4F) e.type=K_END;
            return e;
        }
        if(key==SC_LSHIFT||key==SC_RSHIFT){
            // Emit a single SHIFT event on the rising edge (not while held)
            // so that pressing Shift toggles the IME language mode once.
            if(!m_shift){ m_shift=true; e.type=K_SHIFT; }
            return e;
        }
        if(key==SC_LCTRL){ m_ctrl=true; return e; }
        if(key==SC_CAPSLOCK){ m_caps=!m_caps; return e; }

        // Ctrl+key combinations
        if(m_ctrl){
            if(key==0x2E){ e.type=K_CTRL_C; return e; }   // Ctrl+C
            if(key==0x2F){ e.type=K_CTRL_V; return e; }   // Ctrl+V
            if(key==0x26){ e.type=K_CTRL_L; return e; }   // Ctrl+L
            if(key==0x2C){ e.type=K_CTRL_Z; return e; }   // Ctrl+Z (undo)
            if(key==0x1E){ e.type=K_CTRL_A; return e; }   // Ctrl+A (select all)
            if(key==0x1F){ e.type=K_CTRL_S; return e; }   // Ctrl+S (save)
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
    // Clear any stuck modifier / extended-key state.  Called before
    // each discrete input read so a stale 0xE0 (extended-key prefix)
    // left in the PS/2 buffer at boot cannot swallow the first real
    // keystroke (symptom: first typed character silently lost).
    void reset(){ m_shift=m_caps=m_ext=m_ctrl=m_gui=false; }
    // Discard bytes already queued in the PS/2 output buffer (keyboard
    // BAT / ACK / identify responses from power-on) so they are not
    // misinterpreted as the start of a key.  Always paired with reset().
    void drain(){
        for(int i=0;i<64;i++){
            uint8_t s = inb(0x64);
            if(s==0xFF) break;
            if(!(s & 0x01)) break;
            (void)inb(0x60);
        }
        reset();
    }
private:
    bool m_shift=false, m_caps=false, m_ext=false, m_ctrl=false, m_gui=false;
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
        // REAL HARDWARE GUARD: skip entirely when no i8042 controller exists.
        // detect_input_devices() already probed it. Without this check the
        // drain loop below spins forever on machines where inb(0x64) == 0xFF
        // (modern laptops in pure UEFI mode), hanging the boot at this point.
        if (!g_hw.keyboard_present){
            serial_puts("[MOUSE] no i8042 controller - mouse init skipped\n");
            return;
        }
        outb(0x64,0xA8);          // enable auxiliary (mouse) port
        drain();                  // flush any stale output
        cmd(0xFF); drain();       // reset mouse (resp: 0xFA,0xAA,0x00)
        // Probe the device id.  A standard PS/2 mouse reports 0x00 and uses
        // 3-byte relative packets.  Intellimouse-wheel devices report 0x03
        // (or 0x04) after the 200,100,80 sample-rate magic, and then need
        // 4-byte packets.  Mismatching the packet length is exactly what made
        // a plain 3-byte touchpad desync on real hardware (the next packet's
        // start byte got swallowed as a phantom wheel value).
        uint8_t id = get_id();
        m_pkt_len = 3;
        if (id == 0x00){
            cmd(0xF3); param(200);
            cmd(0xF3); param(100);
            cmd(0xF3); param(80);
            if (get_id() >= 0x03) m_pkt_len = 4;   // wheel -> 4-byte
        }
        cmd(0xF4);                // enable streaming
        serial_puts(m_pkt_len == 4
            ? "[MOUSE] Intellimouse (4-byte packets, wheel)\n"
            : "[MOUSE] standard PS/2 mouse (3-byte packets)\n");
        outb(0x64,0xAE);          // re-enable keyboard controller
    }
    MouseEvent process(uint8_t b){
        MouseEvent ev={0,0,0,false,false,false,false};
        // The first byte of every PS/2 mouse packet has bit 3 set; bytes 1..n
        // are raw signed deltas that may LEGITIMATELY have bit 3 set (e.g.
        // 0xFE/0xFF for negative movement), so we only use bit 3 to validate
        // the START byte.  A stray byte at idle is dropped; once a packet is
        // in progress we accept the next (len-1) bytes unconditionally and
        // resync on the next genuine start byte.  (Using bit 3 to resync
        // mid-packet would wrongly treat 0xFE/0xFF deltas as new packets.)
        if (m_i == 0 && !(b & 0x08)) return ev;
        m_pkt[m_i++] = b;
        if (m_i >= m_pkt_len){
            m_i = 0;
            ev.valid  = true;
            ev.left   = m_pkt[0] & 0x01;
            ev.right  = m_pkt[0] & 0x02;
            ev.middle = m_pkt[0] & 0x04;
            ev.dx     = (int8_t)m_pkt[1];
            ev.dy     = (int8_t)m_pkt[2];
            ev.dz     = (m_pkt_len >= 4) ? (int8_t)m_pkt[3] : 0;
        }
        return ev;
    }
private:
    // Bounded waits - an absent/wedged i8042 leaves a status bit set forever.
    void wait_write(){ for(int g=0; (inb(0x64)&0x02) && g<100000; g++){} }
    void wait_read(){  for(int g=0; (inb(0x64)&0x01) && g<100000; g++){} }
    void ack(){ for(int i=0;i<1000;i++){ if(inb(0x64)&0x01){ if(inb(0x60)==0xFA) return; } } }
    void drain(){ for(int g=0; (inb(0x64)&0x01) && g<100000; g++) inb(0x60); }
    void cmd(uint8_t c){ outb(0x64,0xD4); wait_write(); outb(0x60,c); ack(); }
    void param(uint8_t p){ outb(0x64,0xD4); wait_write(); outb(0x60,p); ack(); }
    uint8_t get_id(){ cmd(0xF2); wait_read(); return inb(0x60); }
    uint8_t m_pkt[4];
    int     m_i       = 0;
    int     m_pkt_len = 3;
};

} // namespace

// Global instances
static Terminal  term;
static Keyboard  kbd;
static Mouse     mouse;

// ---- GUI functions (implemented in gui.cpp) ----
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
    int      (*write_file)(int fs_type, const char* name, const uint8_t* buf, int size);
    void     (*get_time)(int* h, int* m, int* s);
    // File-mutation (context-menu actions: new folder / delete / rename).
    int      (*mkdir)(int fs, const char* name);
    int      (*remove)(int fs, const char* name);
    int      (*rename)(int fs, const char* old_name, const char* new_name);
    const char* (*get_os_name)(void);
    bool     (*is_64bit)(void);
    // Browser callbacks
    int      (*browser_navigate)(const char* url);
    int      (*browser_status)(void);
    int      (*browser_get_page)(char* buf, int bufsize);
    void     (*browser_reset)(void);
    // Terminal command execution
    void     (*exec_command)(const char* cmd, char* output, int outsize);
    void     (*shutdown)(void);   // power off
    void     (*reboot)(void);     // restart system
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
    int      (*get_cpu_64bit_capable)(void);  // CPU supports 64-bit (capability)
    // Synchronous HTTP GET for the managed Browser control.
    const char* (*http_get)(const char* url);
    // Session persistence (must match gui.cpp's GuiCallbacks).
    int (*session_save)(const char* name, const void* data, int size);
    int (*session_load)(const char* name, void* buf, int bufsize);
    int (*session_clear)(const char* name);
    // Sign-in bridge for the managed lock screen (must match gui.cpp).
    // The password hashes never leave the kernel: login_check() is the
    // only entry point, and it commits the session itself on success.
    int         (*login_check)(const char* user, const char* pass);
    int         (*login_uid)(void);
    int         (*user_count)(void);
    const char* (*user_name)(int idx);
};

extern "C" {
    void gui_set_callbacks(const GuiCallbacks* cb);
    int  gui_init(void);
    void gui_probe_vbe(void);
    void gui_set_startup_app(int id);
    int  gui_app_browser_id(void);
    int  gui_app_id_by_name(const char* n);
    int  gui_available(void);
    void gui_enter(void);
    void gui_mouse_move(int dx, int dy);
    void gui_mouse_down(void);
    void gui_mouse_up(void);
    void gui_mouse_down_right(void);
    int  gui_handle_key(char ch);
    void gui_handle_ctrl(int code);   // 1=Ctrl+C 2=Ctrl+V 3=Ctrl+Z 4=Ctrl+A
    void gui_toggle_ime(void);
    void gui_create_window(int x, int y, int w, int h, const char* title);
    void gui_draw_text(int x, int y, const char* text);
    void gui_fill_rect(int x, int y, int w, int h, uint32_t color);
    int  gui_get_width(void);
    int  gui_get_height(void);
    void gui_render_text_mode(void);
    void gui_exit(void);
    void gui_render(void);
    void gui_tick(void);
    void gui_animate_frame(void);
    // Open a Windows executable file (exe/bat/ps1/com) inside the GUI.
    // Initializes the GUI on demand (kernel boots to command line only).
    void gui_open_file(const char* filename, const char* args);
    // Launch a native Win32 (PE32) application in the GUI (gui.cpp).
    // Returns the number of desktop windows created for the app.
    int  gui_launch_win32(const char* filename);
    // Windows executable loader (winloader.cpp)
    void winloader_init(int (*reader)(const char*, uint8_t*, int), void (*writer)(const char*));
    int  winloader_run(const char* filename, const char* args);
    // Framebuffer console (for real hardware without BGA)
    void fb_console_init(void);
    int  fb_console_available(void);
    void fb_console_render(void);
    void fb_console_clear(void);
    int  gui_bga_available(void);
    int  gui_vbe_mode_set_by_bios(void);
}

// Global VBE flag (set from 0x500D in kernel main)
static bool g_vbe_active = false;
// Auto-launch the Win11 desktop GUI at the end of boot (default ON).
// Set to 0 (e.g. via the `nogui` command, or by editing this line) to
// stay in the text shell after boot.  Initialized at runtime because the
// freestanding .data section is not reliably loaded for new globals on
// this linker script (observed: static = 1 became 0 at runtime).
static int g_auto_gui;
// Framebuffer console mode (when VBE mode set by BIOS, no BGA ports)
static bool g_fb_console_mode = false;

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

// The MKFS data area must NOT overlap the kernel image on the boot disk:
// boot.bin(0) + stage2.bin(1-2) + kernel32.bin(3..~804) occupy LBA 0-804
// and kernel64.bin starts at LBA 2048 (see Makefile).  The old LBA 512
// placement sat inside kernel32.bin, so `mkfs` on a single-disk image
// corrupted the on-disk kernel and the machine could not boot after a
// reboot.  LBA 900..1171 is a safe gap (after kernel32, before kernel64).
#define MKFS_SUPER_LBA    900
#define MKFS_TABLE_LBA    901
#define MKFS_TABLE_SECT   16
#define MKFS_DATA_LBA     917
#define MKFS_DATA_SECTORS 271
// User-data disk (secondary ATA / VHD): data area size in sectors.
// data.vhd must be at least (512 + 15200) sectors = ~7.7 MB.
#define DATA_DISK_SECTORS 15200

#define SFS_SUPER_LBA     800
#define SFS_DIR_LBA       801
#define SFS_DIR_SECT      16
#define SFS_DATA_LBA      817
// The BIOS os.img build places the same SFS image further out on the disk so
// it does not collide with the 64-bit kernel payload.  Probe both locations.
#define SFS_ALT_LBA       3568   // must match Makefile SFS_LBA (kernel64 payload ends at 2048+1492=3540, SFS at 3568)
#define SFS_LINUX_LBA     3836   // independent Linux user-space partition (after main SFS vol; matches Makefile LINUX_SFS_LBA)

// CD/ISO-boot RAM-SFS handoff.  boot_cd.asm streams the (texture-free) SFS
// image off the CD into high RAM and leaves a flag at 0x0900 so the kernel
// can mount it without an ATA disk (none exists on a CD/ISO boot).
#define SFS_RAM_MAGIC     0xC0DE5A5F
#define SFS_RAM_TARGET    0x01400000   // 20 MiB: free window (heap ends 19 MiB, identity-mapped to 32 MiB)
#define SFS_RAM_RESERVE   0x00400000   // 4 MiB reserved in PMM for the RAM-SFS image

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

// =====================================================================
//  User / Permission / sudo system
// =====================================================================
#define MAX_USERS        8
#define USER_NAME_LEN    16
#define USER_GROUP_LEN   16
#define MAX_PERMS        256

// ---- Permission bits (standard 9-bit rwxrwxrwx; stored in PermEntry.mode) ----
#define P_OWNER_R  0x100
#define P_OWNER_W  0x080
#define P_OWNER_X  0x040
#define P_GRP_R    0x020
#define P_GRP_W    0x010
#define P_GRP_X    0x008
#define P_OTH_R    0x004
#define P_OTH_W    0x002
#define P_OTH_X    0x001
// 0644: owner rw + group r + other r
#define DEFAULT_FILE_MODE  (P_OWNER_R|P_OWNER_W|P_GRP_R|P_OTH_R)
// 0755: owner rwx + group rx + other rx
#define DEFAULT_DIR_MODE   (P_OWNER_R|P_OWNER_W|P_OWNER_X|P_GRP_R|P_GRP_X|P_OTH_R|P_OTH_X)
// low 8 bits kept in FileEntry.reserved as redundant copy
#define DEFAULT_FILE_MODE8 (uint8_t)(DEFAULT_FILE_MODE & 0xFF)
#define DEFAULT_DIR_MODE8  (uint8_t)(DEFAULT_DIR_MODE & 0xFF)

// ---- File permission table (persisted to "permdb" in MKFS root) ----
struct PermEntry {
    char     name[FS_NAME_LEN];
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;               // 9-bit rwxrwxrwx
};
static PermEntry g_perms[MAX_PERMS];
static int       g_perm_count = 0;

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

// =====================================================================
//  MKFS v2 optimizations (scene#15 small-file performance work)
//    1. Tail Packing   - small files (<4KB) merged into one data block
//    2. Dir hash index - FNV-1a hash table, O(1) lookup (DirEntry)
//    3. Batch write    - write_batch() syscall: 1 call, 1 entry, N files
//    4. Prealloc + seq - contiguous preallocation, sequential write
// =====================================================================
#define FS_TYPE_PACK   2      // tail-packed small-file container (1 entry, many files)
#define FS_TYPE_BATCH  3      // batch-write container (1 entry, many files)
#define MKFS_V2        2
#define TAIL_PACK_LIMIT   4096 // files smaller than this are tail-packed
#define BATCH_AUTO_FILES  100 // cp --batch auto-enable threshold (file count)
#define BATCH_AUTO_AVG   16384 // cp --batch auto-enable threshold (avg bytes/file)

// FNV-1a 32-bit hash (used by the directory hash index)
static uint32_t fnv1a(const char* s, int len){
    uint32_t h = 2166136261u;
    for(int i=0;i<len;i++){ h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

// The user-specified directory hash-entry structure
struct DirEntry {
    uint32_t hash;       // FNV-1a(name)
    uint16_t file_id;    // file-table index, or 0xFFFF when embedded in a container
    uint16_t name_len;
    char     name[FS_NAME_LEN];
};

// In-memory FNV-1a directory index over the 256-entry file table (O(1) lookup)
#define DIR_HASH_BUCKETS 64
static int16_t g_dir_head[DIR_HASH_BUCKETS];
static int16_t g_dir_next[FS_ENTRY_PER_SEC * MKFS_TABLE_SECT];

// Container (shared by PACK and BATCH): one table entry manages many files.
// Layout: [ContainerHdr][slot[CONT_SLOTS]][link[CONT_MAXREC]][PackFileRec[CONT_MAXREC]][data...]
#define CONT_MAGIC_PACK  0x5041434Bu // 'PACK'
#define CONT_MAGIC_BATCH 0x42415448u // 'BATH'
#define CONT_META_SECTORS 16
#define CONT_SLOTS 64
#define CONT_MAXREC 200
struct ContainerHdr {
    uint32_t magic;      // CONT_MAGIC_PACK / CONT_MAGIC_BATCH
    uint16_t count;      // number of files in this container
    uint16_t slots;      // hash table slot count
    uint32_t data_off;   // byte offset (from block start) where file data begins
    uint32_t used;       // bytes used in the data region
    uint32_t reserved;
};
struct PackFileRec {
    DirEntry de;         // hash, file_id(0xFFFF), name_len, name
    uint32_t off;        // byte offset into the data region
    uint32_t len;        // byte length
};
// Resolution of a file name: either a normal table entry, or a packed container.
struct FoundLoc { int table_idx; int pack_idx; int rec_idx; };

// Scratch buffers (separate from g_fsbuf so data writes never clobber index/container state)
static uint8_t g_scratch[512];
static uint8_t g_cont_meta[CONT_META_SECTORS * 512];
// Batch workspace (cp --batch): up to CONT_MAXREC files, 64 KB of payload
struct BatchFile { char name[FS_NAME_LEN]; const uint8_t* data; int size; };
static BatchFile g_batch_files[CONT_MAXREC];
static uint8_t  g_batch_data[64*1024];

// Read file-table entry `idx` into g_scratch and return a pointer to it
static FileEntry* entry_at(int idx){
    fs_read_sector(MKFS_TABLE_LBA + idx/FS_ENTRY_PER_SEC, (uint16_t*)g_scratch);
    return (FileEntry*)(g_scratch + (idx % FS_ENTRY_PER_SEC) * FS_ENTRY_SIZE);
}
static void dir_index_init(){
    for(int i=0;i<DIR_HASH_BUCKETS;i++) g_dir_head[i] = -1;
    for(int i=0;i<FS_ENTRY_PER_SEC*MKFS_TABLE_SECT;i++) g_dir_next[i] = -1;
}
static void dir_index_add(int idx){
    FileEntry* fe = entry_at(idx);
    if(fe->name[0]==0) return;
    uint32_t h = fnv1a(fe->name, strlen_(fe->name));
    int b = h % DIR_HASH_BUCKETS;
    g_dir_next[idx] = g_dir_head[b];
    g_dir_head[b] = (int16_t)idx;
}
static void dir_index_rebuild(){
    dir_index_init();
    for(int i=0;i<FS_ENTRY_PER_SEC*MKFS_TABLE_SECT;i++) dir_index_add(i);
}

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
        // Prefer a dedicated user-data ATA hard disk. Fall back to the
        // boot disk (primary master) for compatibility with single-disk setups.
        fs_detect_data_disk();
        fs_read_sector(MKFS_SUPER_LBA, (uint16_t*)g_fsbuf);
        memcpy_(&sb, g_fsbuf, sizeof(sb));
        mounted = (sb.magic[0]=='M' && sb.magic[1]=='K' &&
                   sb.magic[2]=='F' && sb.magic[3]=='S');
        if(mounted){
            serial_puts("[MKFS] Mounted, data area ");
            serial_puts(g_fs_is_data_disk ? "(user data disk)\n" : "(boot disk)\n");
        } else {
            serial_puts("[MKFS] Not formatted - type 'mkfs' to format the data disk\n");
        }
    }

    void format(){
        sb.magic[0]='M'; sb.magic[1]='K'; sb.magic[2]='F'; sb.magic[3]='S';
        sb.version = MKFS_V2;
        sb.file_count = 0;
        sb.data_start = MKFS_DATA_LBA;
        sb.free_lba = MKFS_DATA_LBA;
        // Dedicated data disk gets a large data area;
        // the boot disk keeps the classic small area (LBA 529..799).
        sb.total_sectors = g_fs_is_data_disk ? DATA_DISK_SECTORS : (uint32_t)MKFS_DATA_SECTORS;
        memset_(g_fsbuf, 0, 512);
        memcpy_(g_fsbuf, &sb, sizeof(sb));
        fs_write_sector(MKFS_SUPER_LBA, (const uint16_t*)g_fsbuf);

        memset_(g_fsbuf, 0, 512);
        for (int s = 0; s < MKFS_TABLE_SECT; s++)
            fs_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);

        mounted = true;
        dir_index_rebuild();
        serial_puts("[MKFS] formatted (v2, tail-pack + batch enabled)\n");
    }

    // List files/dirs in current directory (g_cwd)
    void ls(){
        if (!mounted) { term.write("MKFS not formatted. Use 'mkfs' first.\n"); return; }
        int count = 0;
        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
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
            fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
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
        // IMPORTANT: take a private copy - the sector read below reuses
        // g_fsbuf, which would otherwise clobber the directory entry after
        // the very first iteration (start_lba would turn into file data).
        FileEntry fent;
        memcpy_(&fent, g_fsbuf, sizeof(FileEntry));
        int size = (int)fent.size;
        if (size > bufsize) size = bufsize;
        int sectors = (size + 511) / 512;
        uint8_t* dst = (uint8_t*)buf;
        int remaining = size;
        for (int i = 0; i < sectors && remaining > 0; i++) {
            fs_read_sector(fent.start_lba + (uint32_t)i, (uint16_t*)g_fsbuf);
            int to_copy = (remaining > 512) ? 512 : remaining;
            memcpy_(dst, g_fsbuf, to_copy);
            dst += to_copy;
            remaining -= to_copy;
        }
        return size;
    }

    int create(const char* name, const void* data, int size){
        if (!mounted) return -1;
        int existing = find(name);
        if (existing >= 0) remove_idx(existing);

        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] == 0) {
                    int sectors = (size + 511) / 512;
                    if (sectors == 0) sectors = 1;
                    uint32_t lba = sb.free_lba;
                    if (lba + sectors > MKFS_DATA_LBA + (uint32_t)sb.total_sectors)
                        return -5;  // disk full

                    const uint8_t* src = (const uint8_t*)data;
                    for (int i = 0; i < sectors; i++) {
                        memset_(g_fsbuf, 0, 512);
                        int off = i * 512;
                        int to_copy = (size - off > 512) ? 512 : (size - off);
                        if (to_copy > 0) memcpy_(g_fsbuf, src + off, to_copy);
                        fs_write_sector(lba + i, (const uint16_t*)g_fsbuf);
                    }

                    // Re-read table sector and write entry
                    fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
                    fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                    memset_((void*)fe, 0, FS_ENTRY_SIZE);
                    int i=0; while(name[i] && i<FS_NAME_LEN-1){ fe->name[i]=name[i]; i++; }
                    fe->name[i] = 0;
                    fe->size = (uint32_t)size;
                    fe->start_lba = lba;
                    fe->type = FS_TYPE_FILE;
                    fe->parent = g_cwd;
                    fe->reserved = DEFAULT_FILE_MODE8;
                    fs_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);

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
            fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
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
                    fe->reserved = DEFAULT_DIR_MODE8;
                    fs_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);

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
                fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
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
        fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        memcpy_(g_fsbuf, g_fsbuf + e * FS_ENTRY_SIZE, FS_ENTRY_SIZE);
    }

    void remove_idx(int idx){
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        memset_((void*)fe, 0, FS_ENTRY_SIZE);
        fs_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);
        if (sb.file_count > 0) sb.file_count--;
        flush_sb();
    }

    void flush_sb(){
        memset_(g_fsbuf, 0, 512);
        memcpy_(g_fsbuf, &sb, sizeof(sb));
        fs_write_sector(MKFS_SUPER_LBA, (const uint16_t*)g_fsbuf);
    }
};

// =====================================================================
//  Sfs - Simple File System (compatible, read-only)
// =====================================================================
class Sfs {
public:
    bool       mounted;
    uint32_t   base;      // LBA where the superblock was actually found
    int32_t    delta;     // base - SFS_SUPER_LBA (image may be relocated on disk)
    Superblock sb;

    // CD/ISO-boot RAM-SFS support: when the bootloader has already streamed
    // the SFS image into high RAM (no ATA disk is present on a CD boot), the
    // kernel mounts it from there instead of probing the disk.
    bool       ram_mode;       // true => image is in RAM at ram_base
    uint32_t   ram_base;       // physical address of the SFS image in RAM
    uint32_t   ram_size;       // size of the SFS image in bytes

    void set_ram(uint32_t base, uint32_t size){
        ram_mode = true; ram_base = base; ram_size = size;
    }

    // Unified sector read: from RAM when ram_mode, else from the ATA disk.
    // lba is the canonical SFS LBA (superblock=800, dir=801+, data=817+);
    // in RAM the image is laid out from byte 0, so offset = (lba-800)*512.
    void rd(uint32_t lba, uint16_t* buf){
        if (ram_mode) {
            const uint8_t* src = (const uint8_t*)ram_base
                                + (int32_t)((int32_t)lba - (int32_t)SFS_SUPER_LBA) * 512;
            memcpy_(buf, src, 512);
        } else {
            fs_read_sector(lba, buf);
        }
    }

    // The image is always generated with the canonical 800/801/817 layout, but
    // the build places it at different disk offsets (UEFI: LBA 800,
    // BIOS os.img: LBA 3368).  Probe the known spots and derive a delta that is
    // applied to every directory / data LBA, so one kernel handles both.
    void init(){
        mounted = false; base = SFS_SUPER_LBA; delta = 0;
        if (ram_mode) {
            const uint8_t* sbp = (const uint8_t*)ram_base;
            if (sbp[0]=='S' && sbp[1]=='F' && sbp[2]=='S' && sbp[3]==0) {
                memcpy_(&sb, sbp, sizeof(sb));
                mounted = true;
                serial_puts("[SFS] RAM-backed image mounted at ");
                serial_hex(ram_base);
                serial_puts("\n");
            } else {
                serial_puts("[SFS] RAM flag set but no 'SFS' magic at base\n");
            }
            return;
        }
        // Makefile places the SFS at LBA 3508 (2048 + KERNEL64_SECTORS=1460).
        static const uint32_t cand[] = { SFS_SUPER_LBA, SFS_ALT_LBA, 3536 };
        for (unsigned i = 0; i < sizeof(cand)/sizeof(cand[0]); i++) {
            rd(cand[i], (uint16_t*)g_fsbuf);
            if (g_fsbuf[0]=='S' && g_fsbuf[1]=='F' && g_fsbuf[2]=='S' && g_fsbuf[3]==0) {
                memcpy_(&sb, g_fsbuf, sizeof(sb));
                base    = cand[i];
                delta   = (int32_t)cand[i] - (int32_t)SFS_SUPER_LBA;
                mounted = true;
                return;
            }
        }
    }

    inline uint32_t dir_lba(int s)  const { return (uint32_t)((int32_t)SFS_DIR_LBA + delta) + (uint32_t)s; }
    inline uint32_t data_lba(uint32_t l) const { return (uint32_t)((int32_t)l + delta); }

    // Mount an SFS image placed at an arbitrary disk LBA (used for the
    // independent Linux user-space partition).  delta is derived from the
    // canonical SFS_SUPER_LBA layout, exactly like the probing init() does.
    bool mount_at(uint32_t lba){
        fs_read_sector(lba, (uint16_t*)g_fsbuf);
        if (!(g_fsbuf[0]=='S' && g_fsbuf[1]=='F' && g_fsbuf[2]=='S' && g_fsbuf[3]==0)) {
            serial_puts("[SFS] mount_at(");
            serial_puts_dec(lba);
            serial_puts(") magic Fail bytes=");
            for (int i=0;i<4;i++){ static const char* h="0123456789ABCDEF"; serial_putc(h[(g_fsbuf[i]>>4)&0xF]); serial_putc(h[g_fsbuf[i]&0xF]); }
            serial_puts("\n");
            return false;
        }
        memcpy_(&sb, g_fsbuf, sizeof(sb));
        base  = lba;
        delta = (int32_t)lba - (int32_t)SFS_SUPER_LBA;
        mounted = true;
        return true;
    }

    void ls(){
        if (!mounted) { term.write("SFS not found on disk.\n"); return; }
        int count = 0;
        for (int s = 0; s < SFS_DIR_SECT; s++) {
            rd(dir_lba(s), (uint16_t*)g_fsbuf);
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
        if (!mounted) return -1;
        for (int s = 0; s < SFS_DIR_SECT; s++) {
            rd(dir_lba(s), (uint16_t*)g_fsbuf);
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
        rd(dir_lba(s), (uint16_t*)g_fsbuf);
        // IMPORTANT: private copy - the loop below reloads g_fsbuf with file
        // data, so a pointer into g_fsbuf would decay into garbage after the
        // first sector and every following LBA would be random.
        FileEntry fent;
        memcpy_(&fent, g_fsbuf + e * FS_ENTRY_SIZE, sizeof(FileEntry));
        int size = (int)fent.size;
        if (size > bufsize) size = bufsize;
        int sectors = (size + 511) / 512;
        uint8_t* dst = (uint8_t*)buf;
        int remaining = size;
        for (int i = 0; i < sectors && remaining > 0; i++) {
            rd(data_lba(fent.start_lba + (uint32_t)i), (uint16_t*)g_fsbuf);
            int to_copy = (remaining > 512) ? 512 : remaining;
            memcpy_(dst, g_fsbuf, to_copy);
            dst += to_copy;
            remaining -= to_copy;
        }
        return size;
    }

    // Look up a file and return its size without reading the payload.
    int size_of(const char* name){
        int idx = find(name);
        if (idx < 0) return -1;
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        rd(dir_lba(s), (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        return (int)fe->size;
    }

    // ---- write support (SFS becomes RW) ----
    // Unified sector write: to the ATA disk, or into the RAM-backed image
    // (CD/ISO boot).  lba is the canonical SFS LBA; delta is applied by the
    // callers exactly like rd().
    void wr(uint32_t lba, const uint16_t* buf){
        if (ram_mode) {
            uint8_t* dst = (uint8_t*)ram_base
                         + (int32_t)((int32_t)lba - (int32_t)SFS_SUPER_LBA) * 512;
            memcpy_(dst, buf, 512);
        } else {
            ata_write_sector(lba, buf);
        }
    }

    void flush_sb(){
        memset_(g_fsbuf, 0, 512);
        memcpy_(g_fsbuf, &sb, sizeof(Superblock));   // struct is packed 20 bytes
        wr(base, (const uint16_t*)g_fsbuf);
    }

    // Find a free directory slot, or -1 if the directory is full.
    int find_free(){
        for (int s = 0; s < SFS_DIR_SECT; s++) {
            rd(dir_lba(s), (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] == 0) return s * FS_ENTRY_PER_SEC + e;
            }
        }
        return -1;
    }

    // Create (or overwrite-size) a file.  Data sectors are carved out of the
    // superblock's free area; the superblock is flushed afterwards.
    int create(const char* name, const void* data, int size){
        if (!mounted) return -1;
        if (size < 0 || size > 4*1024*1024) return -2;
        int idx = find(name);
        if (idx >= 0) {
            // overwrite: drop the old entry first, then re-create
            remove(name);
            idx = find(name);
        }
        idx = find_free();
        if (idx < 0) return -3;                        // directory full
        int sectors = (size + 511) / 512;
        if (sectors == 0) sectors = 1;
        uint32_t start = sb.free_lba;
        // Bounds checks in ON-DISK coordinates: delta relocates the volume
        // (BIOS os.img puts SFS at 3368 => delta=2568, UEFI keeps 800 =>
        // delta=0).  The main SFS volume must not cross into the independent
        // Linux user-space volume at SFS_LINUX_LBA; the Linux volume itself
        // may only extend to the end of the disk.
        uint32_t vol_end = (base == SFS_LINUX_LBA) ? g_hw.disk_sectors : SFS_LINUX_LBA;
        uint32_t on_disk_end = (start + (uint32_t)sectors) + (uint32_t)delta;
        if (on_disk_end > vol_end) return -4;            // would hit Linux vol / disk end
        if (ram_mode && (start + (uint32_t)sectors) * 512 > ram_size) return -5;

        // write payload sectors (last one zero-padded)
        const uint8_t* src = (const uint8_t*)data;
        int remaining = size;
        for (int i = 0; i < sectors; i++) {
            memset_(g_fsbuf, 0, 512);
            if (remaining > 0) {
                int n = (remaining > 512) ? 512 : remaining;
                memcpy_(g_fsbuf, src, n);
                src += n; remaining -= n;
            }
            wr(data_lba(start + (uint32_t)i), (const uint16_t*)g_fsbuf);
        }

        // write directory entry
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        rd(dir_lba(s), (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        memset_(fe, 0, FS_ENTRY_SIZE);
        int nl = strlen_(name);
        if (nl > FS_NAME_LEN - 1) nl = FS_NAME_LEN - 1;
        for (int i = 0; i < nl; i++) fe->name[i] = name[i];
        fe->size      = (uint32_t)size;
        fe->start_lba = start;
        fe->type      = FS_TYPE_FILE;
        fe->parent    = FS_ROOT_PARENT;
        fe->reserved  = 0;
        wr(dir_lba(s), (const uint16_t*)g_fsbuf);

        // update + flush superblock
        sb.free_lba   = start + (uint32_t)sectors;
        sb.file_count++;
        flush_sb();
        return 0;
    }

    int remove(const char* name){
        if (!mounted) return -1;
        int idx = find(name);
        if (idx < 0) return -2;
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        rd(dir_lba(s), (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        memset_(fe, 0, FS_ENTRY_SIZE);
        wr(dir_lba(s), (const uint16_t*)g_fsbuf);
        if (sb.file_count > 0) sb.file_count--;
        flush_sb();
        return 0;
    }

    int rename(const char* oldname, const char* newname){
        if (!mounted) return -1;
        int idx = find(oldname);
        if (idx < 0) return -2;
        if (find(newname) >= 0) return -3;
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        rd(dir_lba(s), (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        memset_(fe->name, 0, FS_NAME_LEN);
        int nl = strlen_(newname);
        if (nl > FS_NAME_LEN - 1) nl = FS_NAME_LEN - 1;
        for (int i = 0; i < nl; i++) fe->name[i] = newname[i];
        wr(dir_lba(s), (const uint16_t*)g_fsbuf);
        return 0;
    }

    int mkdir(const char* name){
        if (!mounted) return -1;
        if (find(name) >= 0) return -3;
        int idx = find_free();
        if (idx < 0) return -4;
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        rd(dir_lba(s), (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        memset_(fe, 0, FS_ENTRY_SIZE);
        int nl = strlen_(name);
        if (nl > FS_NAME_LEN - 1) nl = FS_NAME_LEN - 1;
        for (int i = 0; i < nl; i++) fe->name[i] = name[i];
        fe->size      = 0;
        fe->start_lba = 0;
        fe->type      = FS_TYPE_DIR;
        fe->parent    = FS_ROOT_PARENT;
        fe->reserved  = 0;
        wr(dir_lba(s), (const uint16_t*)g_fsbuf);
        sb.file_count++;
        flush_sb();
        return 0;
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
static Sfs    linux_fs;   // independent Linux user-space partition (SFS_LINUX_LBA)
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
constexpr uint32_t PMM_MAX_PAGES   = 65536;        // 256 MB / 4 KiB
// The relocated .bss lives at 0x120000 and now ends at 0x0087AE74 (verified
// via i686-elf-readelf on kernel_textboot.elf: __bss_end = 0x0087AE74).  A
// previous edit bumped HEAP_START to 0x500000, which sits INSIDE .bss and made
// the 64-bit staging kmalloc overlap .bss -> "switch" aborted.  Start the heap
// just past __bss_end and keep HEAP_END below the RAM-SFS reserve at 0x1400000
// (and well clear of .lmboot @ 0x1800000).  The 64-bit staging buffer (720 KiB
// kmalloc) therefore lands in 0x880000..0xF20000, clear of both.
constexpr uint32_t HEAP_START      = 0x900000;     // 9 MiB (must stay > __bss_end; CLR globals enlarged .bss)
constexpr uint32_t HEAP_SIZE       = 0xA00000;     // 10 MiB (HEAP_END = 0x1300000 < RAM-SFS @ 0x1400000)
constexpr uint32_t HEAP_END        = HEAP_START + HEAP_SIZE;

// Page-table / PDE flags
constexpr uint32_t PG_PRESENT  = 0x001;
constexpr uint32_t PG_RW       = 0x002;
constexpr uint32_t PG_USER     = 0x004;
constexpr uint32_t PG_PSE      = 0x080;   // 4 MiB page (PS bit in PDE)

// --- CRx / MSR helpers ---
static inline uint32_t read_cr0(){ uint32_t v; __asm__ __volatile__("mov %%cr0,%0":"=r"(v)); return v; }
static inline uint32_t read_cr3(){ uint32_t v; __asm__ __volatile__("mov %%cr3,%0":"=r"(v)); return v; }
static inline uint32_t read_cr4(){ uint32_t v; __asm__ __volatile__("mov %%cr4,%0":"=r"(v)); return v; }
static inline void write_cr0(uint32_t v){ __asm__ __volatile__("mov %0,%%cr0"::"r"(v)); }
static inline void write_cr3(uint32_t v){ __asm__ __volatile__("mov %0,%%cr3"::"r"(v)); }
static inline void write_cr4(uint32_t v){ __asm__ __volatile__("mov %0,%%cr4"::"r"(v)); }
static inline void invlpg(uint32_t addr){ __asm__ __volatile__("invlpg (%0)"::"r"(addr)); }

// =====================================================================
//  Physical Memory Manager (PMM)
// =====================================================================
// The bitmap used to live at a hard-coded physical 0x80000.  That was safe
// only while the kernel image stayed below 448 KiB.  It no longer does: the
// flat binary is linked at 0x10000 and .rodata now ends past 0x83000, so the
// bitmap sat *inside* the kernel's own .rodata.  pmm_init() then marked the
// kernel/heap pages used, writing runs of 0xFFFFFFFF over live string
// literals -- string constants silently turned into 0xFF garbage (the P4
// skill registry's `keywords` pointer was one victim).
//
// Letting the linker place it in .bss makes overlap impossible by
// construction, whatever the image grows to.
alignas(PAGE_SIZE) static uint32_t pmm_bitmap_store[PMM_MAX_PAGES / 32];
static uint32_t* pmm_bitmap   = pmm_bitmap_store;
static uint32_t  pmm_total_pages = 0;
static uint32_t  pmm_free_pages  = 0;
static uint32_t  pmm_used_pages  = 0;
static uint32_t  pmm_mem_kb      = 0;

// Detect total memory: prefer E820 map (from BIOS), fall back to CMOS RTC
static uint32_t detect_memory_kb(){
    // Use E820 data if available (more accurate than CMOS)
    if (g_hw.mem_e820_available && g_hw.mem_total_kb > 0) {
        return g_hw.mem_total_kb;
    }
    // Fallback: CMOS RTC ports 0x70/0x71
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

// Provided by linker.ld -- bounds of the relocated .bss section.
extern "C" char __bss_start[];
extern "C" char __bss_end[];
// Provided by linker.ld -- bounds of the boot-critical .lmboot scratch region
// (32-bit boot stack + long-mode page tables + 64-bit boot stack).  It sits at
// 0x1800000 (24 MiB), i.e. ABOVE the kernel heap and the RAM-SFS window, not
// just above .bss: CR3 keeps pointing at those tables for the whole life of
// the 64-bit kernel, so the region has to clear BOTH kernels' allocators.
// PMM must treat it as used -- handing out the pages holding the live
// PML4/PDPT would corrupt CR3 under the CPU.
extern "C" char __lmboot_start[];
extern "C" char __lmboot_end[];

// Do the half-open intervals [a0,a1) and [b0,b1) intersect?
//
// The guards below used to be single-sided comparisons ("x < __lmboot_end"),
// which silently assumed .lmboot sat between .bss and the heap.  Once it moved
// above the heap those tests became permanently true and started rejecting
// perfectly good addresses -- the staging-buffer guard refused every kmalloc
// result and blocked the 64-bit transition outright, so the machine quietly
// stayed on the 32-bit GUI.  Always compare ranges, never edges.
static inline bool ranges_overlap(uint32_t a0, uint32_t a1,
                                  uint32_t b0, uint32_t b1){
    return a0 < b1 && b0 < a1;
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

    // Mark the kernel .bss (relocated above 1 MiB by linker.ld) as used,
    // otherwise pmm_alloc_page() would hand out pages holding live globals.
    // The range deliberately extends through .lmboot (boot stacks + the
    // long-mode page tables) -- those pages stay live all the way into the
    // 64-bit kernel, so they must never be allocatable.
    {
        uint32_t bs = (uint32_t)(uintptr_t)__bss_start & ~(PAGE_SIZE - 1);
        uint32_t be = ((uint32_t)(uintptr_t)__lmboot_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (uint32_t a = bs; a < be; a += PAGE_SIZE) {
            if (a < PMM_BASE_ADDR) continue;
            uint32_t page_idx = (a - PMM_BASE_ADDR) / PAGE_SIZE;
            if (page_idx >= pmm_total_pages) break;
            if (pmm_bitmap[page_idx / 32] & (1u << (page_idx % 32))) continue;
            pmm_bitmap[page_idx / 32] |= (1u << (page_idx % 32));
            pmm_free_pages--;
            pmm_used_pages++;
        }
        serial_puts("[PMM] .bss+lmboot reserved "); serial_hex(bs);
        serial_puts(" .. "); serial_hex(be); serial_puts("\n");
        serial_puts("[PMM] lmboot "); serial_hex((uint32_t)(uintptr_t)__lmboot_start);
        serial_puts(" .. "); serial_hex((uint32_t)(uintptr_t)__lmboot_end);
        serial_puts(" (k32 stack/PML4/k64 stack)\n");
    }
    // Reserve the CD-boot RAM-SFS region (SFS_RAM_TARGET, SFS_RAM_RESERVE) so
    // pmm_alloc_page() never hands those pages to a caller and clobbers the
    // read-only SFS image that boot_cd.asm streamed off the CD.
    {
        uint32_t rs = SFS_RAM_TARGET & ~(PAGE_SIZE - 1);
        uint32_t re = rs + SFS_RAM_RESERVE;
        for (uint32_t a = rs; a < re; a += PAGE_SIZE) {
            if (a < PMM_BASE_ADDR) continue;
            uint32_t page_idx = (a - PMM_BASE_ADDR) / PAGE_SIZE;
            if (page_idx >= pmm_total_pages) break;
            if (pmm_bitmap[page_idx / 32] & (1u << (page_idx % 32))) continue;
            pmm_bitmap[page_idx / 32] |= (1u << (page_idx % 32));
            pmm_free_pages--;
            pmm_used_pages++;
        }
        serial_puts("[PMM] RAM-SFS region reserved "); serial_hex(rs);
        serial_puts(" .. "); serial_hex(re); serial_puts("\n");
    }
    serial_puts("[PMM] Initialised\n");
}

extern "C" uint32_t pmm_alloc_page(){
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

extern "C" void pmm_free_page(uint32_t phys){
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
//  Page directory: a linker-reserved, 4 KiB-aligned array in .bss.
//  It MUST NOT be a hard-coded absolute address -- the kernel .bss now
//  extends past 0xA0000, so a fixed 0x70000 landed *inside* g_wm and got
//  silently overwritten by the window manager, wiping PDE[0] and
//  triple-faulting the CPU as soon as a stale TLB entry expired.
//  Uses 4 MiB PSE pages for identity-mapping the first 32 MiB, with the
//  ability to split individual 4 MiB regions into 4 KiB pages.
alignas(4096) static uint32_t g_page_directory_store[1024];
static uint32_t* const page_directory = g_page_directory_store;

static bool vmm_paging_on  = false;
static bool vmm_long_mode  = false;
static bool vmm_our_paging = false;  // true only when WE set up paging (BIOS path)

static bool vmm_check_long_mode(){
    // EFER MSR (0xC0000080).  Bit 8 = LME (Long Mode Enable),
    // bit 10 = LMA (Long Mode Active).  rdmsr returns MSR bits 0..31 in EAX,
    // bits 32..63 in EDX.  We check LMA (active long mode) in EAX.
    uint32_t eax, edx;
    __asm__ __volatile__("rdmsr":"=a"(eax),"=d"(edx):"c"(0xC0000080u));
    return (eax & (1u << 10)) != 0;
}

// ---- High-framebuffer 4-level mapping (for >4GB GOP framebuffers) ----
// When the UEFI GOP framebuffer lives above 4GB, the 32-bit kernel cannot
// reach it through the firmware's identity mapping (VA == PA > 4GB).  We
// build our own 4-level page tables that keep the low 4GB identity-mapped
// and add a <4GB virtual window at FB_WIN_VA pointing at the real high
// framebuffer, so 32-bit code can draw to it directly.  The CPU stays in
// long mode (we run 32-bit compat code), so CR4.PAE/EFER.LME stay set and
// CR3 is a PML4 -- no fragile LME teardown required.
#define FB_WIN_VA  0xF0000000u
// A PAE / long-mode page table page holds 512 x 8-byte entries (NOT 1024 like
// a classic 32-bit PTE page).  Getting this wrong silently mapped only the
// first 2 MiB of the window: 800x600x4 = 1.92 MiB just fit, but any real
// native resolution (1920x1080x4 = 8.3 MiB) ran off the end of the mapping
// and #PF'd on the first pixel past 2 MiB -> black screen on real hardware.
#define FB_WIN_PT_ENTRIES 512u
#define FB_WIN_PTS        32u                      // 32 x 2 MiB = 64 MiB window
alignas(4096) static uint64_t g_pml4[512];
alignas(4096) static uint64_t g_pdpt[512];
alignas(4096) static uint64_t g_pd[4][512];
alignas(4096) static uint64_t g_win_pt[FB_WIN_PTS][FB_WIN_PT_ENTRIES];
static bool g_fb_high_mapped = false;

static void vmm_map_high_fb(void);

static void vmm_init(){
    vmm_long_mode = vmm_check_long_mode();
    vmm_paging_on = (read_cr0() & 0x80000000u) != 0;

    if (vmm_long_mode){
        // UEFI: firmware 4-level identity paging active; we run 32-bit compat.
        // If the GOP framebuffer is above 4GB, the kernel can't address it via
        // the identity map (VA == PA).  Build our own 4-level tables that keep
        // the low 4GB identity-mapped and add a <4GB window for the high fb.
        volatile uint8_t* vb = (volatile uint8_t*)0x5000;
        if (vb[0x0D] == 1) {                                   // vbe_ok
            uint64_t fb = *(volatile uint64_t*)(vb + 0x10);     // framebuffer_phys64
            uint32_t sb = *(volatile uint32_t*)(vb + 0x18);     // shadow_buffer
            if (fb > 0xFFFFFFFFULL && sb == 0) {
                vmm_map_high_fb();
                return;
            }
        }
        serial_puts("[VMM] Long mode – firmware paging in use\n");
        return;
    }
    if (vmm_paging_on){
        // UEFI compat mode: firmware's 32-bit paging is active.
        // We can't safely replace it, so use the existing identity mapping.
        serial_puts("[VMM] Firmware paging active (not our own)\n");
        return;
    }

    // ---- BIOS path: set up 32-bit paging with 4 MiB PSE ----
    serial_puts("[VMM] Setting up 32-bit paging (PSE)...\n");

    // Clear page directory
    for (int i = 0; i < 1024; i++) page_directory[i] = 0;

    // Identity-map first 32 MiB with 4 MiB pages (covers kernel + 16 MiB heap)
    for (int i = 0; i < 8; i++)
        page_directory[i] = (i * 0x400000u) | PG_PRESENT | PG_RW | PG_PSE;

    // Identity-map VBE linear framebuffer area (for GUI support)
    // Read VBE info from 0x5000 (set by stage2 bootloader)
    {
        volatile uint8_t* vbe_info = (volatile uint8_t*)0x5000;
        if (vbe_info[0x0D] == 1) {  // vbe_ok
            uint32_t lfb_phys = *(volatile uint32_t*)(0x5000);
            uint16_t lfb_w    = *(volatile uint16_t*)(0x5004);
            uint16_t lfb_h    = *(volatile uint16_t*)(0x5006);
            uint16_t lfb_pitch= *(volatile uint16_t*)(0x5009);
            uint32_t lfb_size = (uint32_t)lfb_pitch * lfb_h;
            // Map enough 4MB pages to cover the LFB (identity-mapped)
            uint32_t pd_start = lfb_phys / 0x400000u;
            uint32_t pd_count = (lfb_size + 0x3FFFFFu) / 0x400000u;
            if (pd_count < 1) pd_count = 1;
            if (pd_count > 64) pd_count = 64;  // cap at 256MB
            for (uint32_t i = 0; i < pd_count; i++) {
                uint32_t idx = pd_start + i;
                if (idx < 1024) {
                    page_directory[idx] = (idx * 0x400000u) | PG_PRESENT | PG_RW | PG_PSE;
                }
            }
            serial_puts("[VMM] VBE LFB mapped in page directory\n");
        }
        // Also identity-map the standard Bochs VBE LFB (0xE0000000) -- the
        // active framebuffer on QEMU/Bochs even when the BIOS VBE callback
        // reports the legacy 0xFD000000 address.
        {
            uint32_t pd_start = 0xE0000000u / 0x400000u;
            for (uint32_t i = 0; i < 2; i++) {
                uint32_t idx = pd_start + i;
                if (idx < 1024)
                    page_directory[idx] = (idx * 0x400000u) | PG_PRESENT | PG_RW | PG_PSE;
            }
        }
    }

    // Foundation 0: ring-3 user region (PG_USER). Identity-map 64-256 MiB
    // as 4 MiB PSE pages. This memory is real RAM (QEMU -m 512M) and is
    // separate from the supervisor-only lower 32 MiB, so a ring-3 process
    // can execute here but cannot reach kernel/heap without a #PF.
    // 128-256 MiB covers real Linux static ELF link addresses (0x08048000).
    for (int i = 0x10; i < 0x40; i++)
        page_directory[i] = (i * 0x400000u) | PG_PRESENT | PG_RW | PG_USER | PG_PSE;
    serial_puts("[VMM] ring-3 user region 0x04000000-0x08000000 (PG_USER)\n");

    // Enable PSE (CR4.PSE = bit 4)
    uint32_t cr4 = read_cr4();
    write_cr4(cr4 | 0x10);

    // Load page directory into CR3 (identity-mapped, so VA == PA)
    write_cr3((uint32_t)(uintptr_t)page_directory);

    // Enable paging (CR0.PG = bit 31)
    uint32_t cr0 = read_cr0();
    write_cr0(cr0 | 0x80000000u);

    vmm_paging_on  = true;
    vmm_our_paging = true;
    serial_puts("[VMM] Paging enabled (32 MiB identity-mapped, 4 MiB PSE)\n");
}

// Map a >4GB GOP framebuffer into a <4GB virtual window (FB_WIN_VA) using our
// own 4-level (long-mode) page tables, replacing the firmware tables.  The CPU
// remains in long mode, so CR3 is a PML4 and CR4.PAE/EFER.LME stay enabled.
static void vmm_map_high_fb(void){
    volatile uint8_t* vb = (volatile uint8_t*)0x5000;
    uint64_t fb    = *(volatile uint64_t*)(vb + 0x10);   // framebuffer_phys64
    uint32_t pitch = *(volatile uint16_t*)(vb + 0x09);
    uint16_t h     = *(volatile uint16_t*)(vb + 0x06);
    uint16_t w     = *(volatile uint16_t*)(vb + 0x04);
    uint32_t total = (uint32_t)pitch * (uint32_t)h;
    if (total == 0) total = (uint32_t)w * (uint32_t)h * 4u;          // bpp fallback
    uint32_t pages = (total + 4095u) >> 12;
    const uint32_t max_pages = FB_WIN_PTS * FB_WIN_PT_ENTRIES;      // 64 MiB cap
    if (pages > max_pages) pages = max_pages;
    uint32_t pt_count = (pages + FB_WIN_PT_ENTRIES - 1u) / FB_WIN_PT_ENTRIES;
    if (pt_count > FB_WIN_PTS) pt_count = FB_WIN_PTS;

    // Zero the page-table structures.
    for (int i = 0; i < 512; i++) g_pml4[i] = 0;
    for (int i = 0; i < 512; i++) g_pdpt[i] = 0;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 512; j++) g_pd[i][j] = 0;
    for (uint32_t p = 0; p < pt_count; p++)
        for (uint32_t j = 0; j < FB_WIN_PT_ENTRIES; j++) g_win_pt[p][j] = 0;

    // Identity-map the low 4GB with 2MiB pages (covers kernel/heap/MMIO).
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 512; j++)
            g_pd[i][j] = ((uint64_t)((uint32_t)i * 0x40000000u + (uint32_t)j * 0x200000u)) | 0x83;

    // PDPT[i] -> g_pd[i]  (each PD covers 1 GiB).
    for (int i = 0; i < 4; i++)
        g_pdpt[i] = ((uint64_t)(uintptr_t)&g_pd[i][0]) | 0x7;

    // Override the window PDE(s): VA FB_WIN_VA lives in PDPT[3] (3..4 GiB).
    uint32_t win_pde = (FB_WIN_VA & 0x3FFFFFFFu) >> 21;            // index within PD
    if (win_pde + pt_count > 512u) pt_count = 512u - win_pde;      // stay in the PD
    for (uint32_t p = 0; p < pt_count; p++)
        g_pd[3][win_pde + p] = ((uint64_t)(uintptr_t)&g_win_pt[p][0]) | 0x7;
    if (pages > pt_count * FB_WIN_PT_ENTRIES) pages = pt_count * FB_WIN_PT_ENTRIES;

    // Fill the window page tables: VA FB_WIN_VA + k*4K -> PA fb + k*4K.
    // 512 entries per table -- see FB_WIN_PT_ENTRIES note above.
    for (uint32_t k = 0; k < pages; k++) {
        uint64_t pa = fb + (uint64_t)k * 4096u;
        g_win_pt[k / FB_WIN_PT_ENTRIES][k % FB_WIN_PT_ENTRIES] = pa | 0x3;
    }

    // PML4[0] -> PDPT (covers 0..512 GiB: 0..4GB + window).
    g_pml4[0] = ((uint64_t)(uintptr_t)&g_pdpt[0]) | 0x7;

    // Load our PML4 (paging already on, long mode, PAE already enabled).
    write_cr3((uint32_t)(uintptr_t)g_pml4);

    // Tell the GUI to draw into the virtual window, not the >4GB address.
    *(volatile uint32_t*)(vb)      = FB_WIN_VA;                    // framebuffer_phys
    *(volatile uint32_t*)(vb + 0x18) = 1;                         // shadow_buffer = 1 (mapped)

    // Registry: in the 32-bit compat stage the framebuffer is reached through
    // the 0xF0000000 window, so FB_VIRT must point there (the 64-bit kernel
    // later overrides this to the real high address via Gfx::init).
    addr_set_phys(ADDR_FB_PHYS, fb);
    addr_set_virt(ADDR_FB_VIRT, (uint64_t)FB_WIN_VA);

    g_fb_high_mapped = true;
    serial_puts("[VMM] high FB 0x");
    serial_hex((uint32_t)(fb >> 32));
    serial_hex((uint32_t)(fb & 0xFFFFFFFFULL));
    serial_puts(" mapped to VA 0x");
    serial_hex(FB_WIN_VA);
    serial_puts(" span=0x");
    serial_hex(pages << 12);
    serial_puts(" pts=0x");
    serial_hex(pt_count);
    serial_puts("\n");
}
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
extern "C" int vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags){
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
extern "C" uint32_t vmm_get_phys(uint32_t virt){
    if (!vmm_paging_on) return virt;  // identity (no paging)

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
    // The 14 MiB region at HEAP_START is identity-mapped (first 32 MiB).
    // Guards: the heap must not overlap the relocated .bss (live globals) nor
    // the .lmboot scratch region (boot stacks + live long-mode page tables).
    // linker.ld already ASSERTs the layout at build time; these are runtime
    // belt-and-braces checks that survive a bad HEAP_START edit.
    if (ranges_overlap(HEAP_START, HEAP_END,
                       (uint32_t)(uintptr_t)__bss_start,
                       (uint32_t)(uintptr_t)__bss_end)) {
        serial_puts("[HEAP] FATAL: heap overlaps .bss ");
        serial_hex((uint32_t)(uintptr_t)__bss_start);
        serial_puts("..");
        serial_hex((uint32_t)(uintptr_t)__bss_end);
        serial_puts(" -- raise HEAP_START in kernel.cpp\n");
    }
    if (ranges_overlap(HEAP_START, HEAP_END,
                       (uint32_t)(uintptr_t)__lmboot_start,
                       (uint32_t)(uintptr_t)__lmboot_end)) {
        serial_puts("[HEAP] FATAL: heap overlaps .lmboot ");
        serial_hex((uint32_t)(uintptr_t)__lmboot_start);
        serial_puts("..");
        serial_hex((uint32_t)(uintptr_t)__lmboot_end);
        serial_puts(" -- move .lmboot in linker.ld or shrink the heap\n");
    }
    heap_head = (HeapBlock*)HEAP_START;
    heap_head->magic = HEAP_MAGIC_FREE;
    heap_head->size  = HEAP_SIZE - sizeof(HeapBlock);
    heap_head->next  = nullptr;
    heap_head->prev  = nullptr;
    heap_alloc_count = 0;
    heap_free_count  = 0;
    heap_bytes_alloc = 0;
    heap_bytes_freed = 0;
    serial_puts("[HEAP] Initialised: 14 MiB at 0x500000\n");
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
    if (vmm_long_mode)        term.write("Long mode (UEFI 4-level paging)\n");
    else if (vmm_our_paging)  term.write("32-bit (our PSE 4 MiB paging)\n");
    else                      term.write("32-bit (firmware paging)\n");
    term.write("  Page directory:"); term.write_hex((uint32_t)page_directory); term.write("\n");
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

    // Address Management Registry (single source of truth for key addresses)
    term.write("\nAddress Registry:\n");
    for (int i = 0; i < ADDR_COUNT; i++) {
        if (!addr_is_set((addr_key_t)i)) continue;
        term.write("  ");
        term.write(addr_name((addr_key_t)i));
        term.write("  phys=0x");
        term.write_hex64(addr_phys((addr_key_t)i));
        term.write("  virt=0x");
        term.write_hex64(addr_virt((addr_key_t)i));
        term.write("\n");
    }
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
    term.write("  kmalloc(64)   = "); term.write_hex((uint32_t)p1); term.write("\n");
    term.write("  kmalloc(256)  = "); term.write_hex((uint32_t)p2); term.write("\n");
    term.write("  kmalloc(1024) = "); term.write_hex((uint32_t)p3); term.write("\n");

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
    term.write("  kmalloc(64)   = "); term.write_hex((uint32_t)p4);
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
    term.write("  kmalloc(102400) = "); term.write_hex((uint32_t)big); term.write("\n");
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
        term.write("Page mapping test requires our own paging (BIOS path).\n");
        term.write("In UEFI mode, firmware paging is used and cannot be modified.\n\n");
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

    // Page directory dump (first 4 entries)
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
    term.write("  disk        Show disk + SFS info\n");
    term.write("  disk rw     Raw ATA sector write/read-back test\n");
    term.write("  disk sfs    SFS file create/read/rename/remove round-trip\n");
    term.write("\nScript execution:\n");
    term.write("  run <f>     Run .sh script from MKFS\n");
    term.write("  runfs <f>   Run .sh script from SFS\n");
    term.write("\nWin32 subsystem (PE32 loader + simulated registry):\n");
    term.write("  winapp <f.exe> [args]  Load a Win32 app and open its GUI window\n");
    term.write("  winapp /i <f.exe>      Inspect PE headers/sections/imports only\n");
    term.write("  reg query|list|tree <key>   Read the simulated registry\n");
    term.write("  reg set <key> <val> <type> <data>   Write a registry value\n");
    term.write("  reg delete <key> [value]    Delete a key or value\n");
    term.write("  winver      Simulated Windows version (from the hive)\n");
    term.write("  winenv [n]  Simulated process environment variables\n");
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
    term.write("  agent skills List registered AI skills\n");
    term.write("  webapi help Agent bridge into the browser (password gated)\n");
    term.write("\nNetwork (HTTP server on port 8080):\n");
    term.write("  netstart    Initialize NE2000 NIC and HTTP server\n");
    term.write("  netinfo     Show network status (IP, MAC, connections)\n");
    term.write("  netstat     Alias for netinfo\n");
    term.write("  Web UI:     http://10.0.2.15:8080 (from host: localhost:8080)\n");
    term.write("\nKernel switching:\n");
    term.write("  switch      Switch to 64-bit kernel (loads from LBA 400)\n");
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
    term.write("  Ctrl+Z      Undo last edit    Ctrl+A: select all\n");
    term.write("  Ctrl+Up/Dn  Cycle clipboard history\n");
    term.write("\nMouse: wheel=scroll, drag=select, click=refocus\n");
}

static void cmd_about(){
    term.write("NexOS v4.0  -  C++ freestanding kernel\n");
    term.write("Two-stage boot, 32-bit protected mode.\n");
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
        fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        cur = fe->parent;
    }
    int pos = 0;
    buf[pos++] = '/';
    for (int i = depth - 1; i >= 0 && pos < maxlen - 1; i--) {
        int s = path[i] / FS_ENTRY_PER_SEC;
        int e = path[i] % FS_ENTRY_PER_SEC;
        fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
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

// ---- User/permission forward declarations (defined below in the security module) ----
static int  cur_uid();
static int  cur_gid();
static bool perm_check(const char* name, char need, bool quiet);
static void perm_set(const char* name, uint32_t uid, uint32_t gid, uint16_t mode);
static bool perm_get(const char* name, uint32_t* uid, uint32_t* gid, uint16_t* mode);

static void cmd_ls(){
    mkfs.ls();
}

static void cmd_cat(const char* name){
    if (!name[0]) { term.write("Usage: cat <filename>\n"); return; }
    if(!perm_check(name, 'r', false)) return;
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
    // If file exists, require write permission
    if(mkfs.find(buf) >= 0){
        if(!perm_check(buf, 'w', false)) return;
    }
    int ret = mkfs.create(buf, (const uint8_t*)"", 0);
    if (ret >= 0) {
        perm_set(buf, (uint32_t)cur_uid(), (uint32_t)cur_gid(), DEFAULT_FILE_MODE);
        term.write("Created: "); term.write(buf); term.put_char('\n');
    } else {
        term.write("Failed (code "); term.write_dec(ret); term.write(")\n");
    }
}

static void cmd_rm(const char* name){
    if (!name[0]) { term.write("Usage: rm <filename>\n"); return; }
    if(mkfs.find(name) < 0){ term.write("File not found: "); term.write(name); term.put_char('\n'); return; }
    if(!perm_check(name, 'w', false)) return;
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
        perm_set(dst, (uint32_t)cur_uid(), (uint32_t)cur_gid(), DEFAULT_FILE_MODE);
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
    // If file exists, require write permission
    if(mkfs.find(name) >= 0){
        if(!perm_check(name, 'w', false)) return;
    }
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
        perm_set(name, (uint32_t)cur_uid(), (uint32_t)cur_gid(), DEFAULT_DIR_MODE);
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
// ----- process management (ps / kill) -----
extern "C" int  proc_list(char* buf, int bufsz);
extern "C" int  proc_kill(uint32_t pid);
static void cmd_ps(){
    char buf[512];
    proc_list(buf, (int)sizeof(buf));
    term.write(buf);
    if (buf[0]) term.write("\n");
}
static void cmd_kill(const char* arg){
    if (!*arg){ term.write("usage: kill <pid>\n"); return; }
    uint32_t pid = 0;
    for (const char* c = arg; *c >= '0' && *c <= '9'; c++) pid = pid * 10 + (uint32_t)(*c - '0');
    if (pid == 0){ term.write("cannot kill the kernel (pid 0)\n"); return; }
    if (proc_kill(pid) == 0){ term.write("killed pid "); term.write_dec((int)pid); term.write("\n"); }
    else { term.write("no such pid: "); term.write_dec((int)pid); term.write("\n"); }
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

    // Show NexOS disk layout
    term.write("\nNexOS disk layout:\n");
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

// ----- Disk write tests -----
// "disk"        - show disk + SFS info
// "disk rw"     - raw ATA sector round-trip (write -> read back -> verify)
// "disk sfs"    - SFS file round-trip (create -> read -> rename -> remove)
static void cmd_disk_rw_raw();
static void cmd_disk_sfs_rw();
static void cmd_disk(const char* args){
    if (!args[0] || !strcmp_(args, "info")) {
        term.write("Disk: ");
        if (!g_hw.disk_present) { term.write("not present\n"); return; }
        term.write(g_hw.disk_model);
        term.write("  "); term.write_dec((int)g_hw.disk_size_mb);
        term.write(" MB ("); term.write_dec((int)g_hw.disk_sectors); term.write(" sectors)\n");
        if (sfs.mounted) {
            term.write("SFS @ LBA "); term.write_dec((int)sfs.base);
            term.write("  data_start="); term.write_dec((int)sfs.sb.data_start);
            term.write("  free_lba="); term.write_dec((int)sfs.sb.free_lba);
            term.write("  files="); term.write_dec((int)sfs.sb.file_count); term.write("\n");
        }
        return;
    }
    if (!strcmp_(args, "rw")) { cmd_disk_rw_raw(); return; }
    if (!strcmp_(args, "sfs")) { cmd_disk_sfs_rw(); return; }
    term.write("Usage: disk [info|rw|sfs]\n");
}

// Raw single-sector ATA round-trip at a safe scratch LBA.
// Preferred target: the main SFS volume's free data area (on-disk), which is
// guaranteed unused once SFS_LINUX_LBA (12288) leaves room after the packed
// image.  Fallback: the BIOS growth gap (kernel.bin ends ~LBA 1075, kernel64
// starts at 2048), used when SFS is not mounted.
static void cmd_disk_rw_raw(){
    if (!g_hw.disk_present) { term.write("disk rw: no disk present\n"); return; }
    uint32_t scratch = 0;
    if (sfs.mounted) scratch = sfs.sb.free_lba + (uint32_t)sfs.delta;  // on-disk free space
    if (scratch == 0 || scratch >= SFS_LINUX_LBA - 1)
        scratch = 1600;                                                 // BIOS growth gap
    term.write("disk rw: raw sector round-trip @ LBA ");
    term.write_dec((int)scratch); term.write("...\n");

    memset_(g_fsbuf, 0, 512);
    g_fsbuf[0]='D'; g_fsbuf[1]='I'; g_fsbuf[2]='S'; g_fsbuf[3]='K';
    g_fsbuf[4]='R'; g_fsbuf[5]='W'; g_fsbuf[6]='1';
    g_fsbuf[8]=(uint8_t)(scratch & 0xFF); g_fsbuf[9]=(uint8_t)((scratch>>8)&0xFF);
    g_fsbuf[10]=(uint8_t)((scratch>>16)&0xFF); g_fsbuf[11]=(uint8_t)((scratch>>24)&0xFF);
    for (int i = 12; i < 512; i++) g_fsbuf[i] = (uint8_t)(i & 0xFF);

    ata_write_sector(scratch, (const uint16_t*)g_fsbuf);
    serial_puts("[DISKRW] sector written\n");

    memset_(g_fsbuf, 0xAA, 512);
    ata_read_sector(scratch, (uint16_t*)g_fsbuf);

    bool ok = (g_fsbuf[0]=='D' && g_fsbuf[1]=='I' && g_fsbuf[2]=='S' &&
               g_fsbuf[3]=='K' && g_fsbuf[4]=='R' && g_fsbuf[5]=='W' && g_fsbuf[6]=='1');
    if (ok) for (int i = 12; i < 512; i++)
        if (g_fsbuf[i] != (uint8_t)(i & 0xFF)) { ok = false; break; }
    term.write(ok ? "disk rw: PASS (read-back matches)\n" : "disk rw: FAIL (read-back mismatch)\n");
    serial_puts(ok ? "[DISKRW] PASS\n" : "[DISKRW] FAIL\n");
}

// SFS file round-trip: create a 3-sector file, read back, rename, remove.
static void cmd_disk_sfs_rw(){
    if (!sfs.mounted) { term.write("disk sfs: SFS not mounted\n"); return; }
    term.write("disk sfs: SFS file round-trip (create->read->rename->remove)\n");

    // 3-sector payload with a repeatable pattern
    static uint8_t payload[1600];
    for (int i = 0; i < (int)sizeof(payload); i++) payload[i] = (uint8_t)((i * 7 + 3) & 0xFF);
    const char* fname = "rwtest.bin";
    const char* fname2 = "rwtest2.bin";

    // idempotent: clean leftovers from a previous failed run
    sfs.remove(fname);
    sfs.remove(fname2);

    int r = sfs.create(fname, payload, (int)sizeof(payload));
    if (r != 0) { term.write("disk sfs: FAIL (create rc="); term.write_dec(r); term.write(")\n");
                  serial_puts("[DISKSFS] FAIL create\n"); return; }
    serial_puts("[DISKSFS] create ok\n");

    int sz = sfs.size_of(fname);
    if (sz != (int)sizeof(payload)) {
        term.write("disk sfs: FAIL (size "); term.write_dec(sz);
        term.write(" != "); term.write_dec((int)sizeof(payload)); term.write(")\n");
        serial_puts("[DISKSFS] FAIL size\n"); return;
    }
    static uint8_t rdbuf[2000];
    int n = sfs.read(fname, rdbuf, (int)sizeof(rdbuf));
    bool ok = (n == (int)sizeof(payload));
    if (ok) for (int i = 0; i < n; i++)
        if (rdbuf[i] != payload[i]) { ok = false; break; }
    term.write(ok ? "disk sfs: create+read PASS\n" : "disk sfs: FAIL (data mismatch)\n");
    serial_puts(ok ? "[DISKSFS] read PASS\n" : "[DISKSFS] FAIL read\n");
    if (!ok) return;

    r = sfs.rename(fname, fname2);
    if (r != 0) { term.write("disk sfs: FAIL (rename rc="); term.write_dec(r); term.write(")\n");
                  serial_puts("[DISKSFS] FAIL rename\n"); return; }
    bool renamed = (sfs.find(fname) < 0 && sfs.find(fname2) >= 0 &&
                    sfs.size_of(fname2) == (int)sizeof(payload));
    term.write(renamed ? "disk sfs: rename PASS\n" : "disk sfs: FAIL (rename verify)\n");
    serial_puts(renamed ? "[DISKSFS] rename PASS\n" : "[DISKSFS] FAIL rename\n");

    r = sfs.remove(fname2);
    bool removed = (r == 0 && sfs.find(fname2) < 0);
    term.write(removed ? "disk sfs: remove PASS (cleanup done)\n" : "disk sfs: FAIL (remove)\n");
    serial_puts(removed ? "[DISKSFS] remove PASS\n" : "[DISKSFS] FAIL remove\n");

    bool all = ok && renamed && removed;
    serial_puts(all ? "[DISKSFS] ALL PASS\n" : "[DISKSFS] FAIL\n");
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
    if (!name[0]) { term.write("Usage: run <file> [args...]\n"); return; }
    // 分离文件名与参数
    static char wl_file[64];
    static char wl_args[128];
    int fi = 0;
    const char* p = name;
    while(*p == ' ') p++;
    while(*p && *p != ' ' && fi < 63) wl_file[fi++] = *p++;
    wl_file[fi] = 0;
    int ai = 0;
    while(*p == ' ') p++;
    while(*p && ai < 127) wl_args[ai++] = *p++;
    wl_args[ai] = 0;

    // 文件类型分派
    const char* ext = wl_file;
    for(const char* q = wl_file; *q; q++) if(*q == '.') ext = q;
    bool is_win = (strncmp_(ext, ".bat", 4) == 0 || strncmp_(ext, ".cmd", 4) == 0 ||
                   strncmp_(ext, ".ps1", 4) == 0 || strncmp_(ext, ".psm1", 4) == 0 ||
                   strncmp_(ext, ".exe", 4) == 0 || strncmp_(ext, ".com", 4) == 0 ||
                   strncmp_(ext, ".dll", 4) == 0 || strncmp_(ext, ".sys", 4) == 0);
    if(is_win){
        // Windows 可执行文件: 按需启动 GUI 并打开该文件
        term.set_color(make_color(GREEN, BLACK));
        term.write("Launching in GUI: "); term.write(wl_file); term.put_char('\n');
        term.set_color(make_color(LIGHT_GREY, BLACK));
        // 注册 winloader 回调 (首次)
        static bool wl_registered = false;
        if(!wl_registered){
            winloader_init(
                [](const char* fn, uint8_t* buf, int bufsize) -> int {
                    int r = -1;
                    if(sfs.mounted) r = sfs.read(fn, buf, bufsize);
                    if(r < 0 && mkfs.mounted) r = mkfs.read(fn, buf, bufsize);
                    return r;
                },
                [](const char* text) { term.write(text); }
            );
            wl_registered = true;
        }
        gui_open_file(wl_file, wl_args[0] ? wl_args : nullptr);
        return;
    }
    // NexOS 原生用户程序 (.nex): 平铺 ELF32, 经 linux_compat ELF32 加载器运行
    // (int 0x80 系统调用 ABI; 内核已在启动时注册 SFS 读取器).
    if (strncmp_(ext, ".nex", 4) == 0) {
        term.set_color(make_color(GREEN, BLACK));
        term.write("Running NexOS native app: "); term.write(wl_file); term.put_char('\n');
        term.set_color(make_color(LIGHT_GREY, BLACK));
        const char* nex_av[1] = { wl_file };
        linux_run(wl_file, 1, nex_av);
        return;
    }
    // 默认: NexOS shell 脚本 (.sh)
    int ret = mkfs.read(wl_file, g_iobuf, FS_IOBUF_SIZE);
    if (ret < 0) {
        term.write("File not found: "); term.write(wl_file); term.put_char('\n');
        return;
    }
    term.set_color(make_color(CYAN, BLACK));
    term.write("--- running: "); term.write(wl_file); term.write(" ---\n");
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
//  Win32 subsystem commands
//  ---------------------------------------------------------------------
//    winapp <file.exe> [args]   load + execute a PE32 image, show its GUI
//    winapp /i <file.exe>       inspect only (headers/sections/imports)
//    reg <query|list|tree|add|set|delete> ...
//    winver                     simulated Windows version (from registry)
//    winenv [name]              simulated process environment
// =====================================================================

// Defined below (4945); winapp needs it before the GUI starts so the
// managed desktop is available.
static void clr_ensure_init();
// Defined below (5706); fills gui.cpp's g_cb machine-state table.  Without
// it the managed shell's Host.Hour()/MemTotalKb()/... call through NULL.
static void register_gui_callbacks(void);

static bool g_win32_ready = false;

static void win32_ensure_init(){
    if (g_win32_ready) return;
    win32_init(
        [](const char* fn, uint8_t* buf, int bufsize) -> int {
            int r = -1;
            while (*fn == ' ') fn++;
            if (sfs.mounted)  { r = sfs.read(fn, buf, bufsize); }
            if (r < 0 && mkfs.mounted) { r = mkfs.read(fn, buf, bufsize); }
            return r;
        },
        [](const char* text) {
            // Win32 console output goes to the VGA terminal *and* the
            // serial log so headless tests can assert on it.
            term.write(text);
            serial_puts(text);
        }
    );
    g_win32_ready = true;
}

// Shared scratch buffer for registry text output
static char g_regbuf[3072];

static void cmd_winapp(const char* args){
    win32_ensure_init();

    while (*args == ' ') args++;
    int info_only = 0;
    if (args[0] == '/' && (args[1] == 'i' || args[1] == 'I') &&
        (args[2] == 0 || args[2] == ' ')) {
        info_only = 1;
        args += 2;
        while (*args == ' ') args++;
    }

    if (!args[0]) {
        term.write("Usage: winapp [/i] <file.exe> [args]\n");
        term.write("       /i  inspect the PE32 image without executing it\n");
        return;
    }

    char file[64]; int fi = 0;
    while (*args && *args != ' ' && fi < 63) file[fi++] = *args++;
    file[fi] = 0;
    while (*args == ' ') args++;

    term.set_color(make_color(CYAN, BLACK));
    term.write("=== NexOS Win32 Subsystem ===\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
    term.write("Image  : "); term.write(file); term.put_char('\n');
    if (args[0]) { term.write("Args   : "); term.write(args); term.put_char('\n'); }

    int rc = win32_run(file, args, info_only);

    const char* rep = win32_last_report();
    if (rep && rep[0]) { term.write(rep); serial_puts(rep); }

    if (rc != 0) {
        term.set_color(make_color(RED, BLACK));
        const char* msg = "[X] Load failed.\n";
        switch (rc) {
            case -1: msg = "[X] File not found in SFS/MKFS.\n"; break;
            case -2: msg = "[X] Not a PE32 executable (missing MZ/PE signature).\n"; break;
            case -3: msg = "[X] Unsupported PE (needs 32-bit i386 native PE32).\n"; break;
            case -4: msg = "[X] Out of memory while mapping the image.\n"; break;
            case -5: msg = "[X] Unresolved imports - see the list above.\n"; break;
            case -6: msg = "[X] Image too large for the loader (192 KiB limit).\n"; break;
            default: break;
        }
        term.write(msg);
        serial_puts(msg);
        term.set_color(make_color(LIGHT_GREY, BLACK));
        return;
    }

    if (info_only) {
        term.set_color(make_color(GREEN, BLACK));
        term.write("[OK] Image inspected (not executed).\n");
        term.set_color(make_color(LIGHT_GREY, BLACK));
        return;
    }

    int wn = win32_window_count();
    if (wn <= 0) {
        term.set_color(make_color(GREEN, BLACK));
        term.write("[OK] Console application finished (no GUI window created).\n");
        term.set_color(make_color(LIGHT_GREY, BLACK));
        return;
    }

    term.set_color(make_color(GREEN, BLACK));
    term.write("[OK] Application created ");
    term.write_dec(wn);
    term.write(" window(s) - starting GUI ...\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));

    // The managed (C#) desktop needs the CLR and gui.cpp's callback table
    // before gui_launch_win32() brings the GUI up (same prerequisites
    // cmd_gui satisfies); without them mforms_boot() reports "CLR not
    // initialised" / the shell's Host.* calls #PF inside PaintOverlay.
    clr_ensure_init();
    register_gui_callbacks();

    int made = gui_launch_win32(file);
    if (made <= 0) {
        term.set_color(make_color(RED, BLACK));
        term.write("[!] Could not open a GUI window (no video mode?).\n");
        term.set_color(make_color(LIGHT_GREY, BLACK));
    }
}

static void cmd_reg(const char* args){
    win32_ensure_init();
    while (*args == ' ') args++;

    char sub[16]; int si = 0;
    while (*args && *args != ' ' && si < 15) sub[si++] = *args++;
    sub[si] = 0;
    while (*args == ' ') args++;

    if (!sub[0] || !strcmp_(sub, "help") || !strcmp_(sub, "/?")) {
        term.write("Registry (simulated Windows hive)\n");
        term.write("  reg query  <key> [value]        read a key or one value\n");
        term.write("  reg list   <key>                list subkeys + values\n");
        term.write("  reg tree   <key> [depth]        show the key tree\n");
        term.write("  reg add    <key>                create a key\n");
        term.write("  reg set    <key> <val> <type> <data>   write a value\n");
        term.write("                                  type: SZ|DWORD|EXPAND|BIN|MULTI\n");
        term.write("  reg delete <key> [value]        delete a value or key\n");
        term.write("  reg stat                        hive statistics\n");
        term.write("Roots: HKLM HKCU HKCR HKU HKCC (long names also accepted)\n");
        return;
    }

    if (!strcmp_(sub, "stat")) {
        term.write("Registry keys   : "); term.write_dec(win32_reg_key_count());   term.put_char('\n');
        term.write("Registry values : "); term.write_dec(win32_reg_value_count()); term.put_char('\n');
        return;
    }

    // remaining: <key> [rest]
    char key[192]; int ki = 0;
    while (*args && *args != ' ' && ki < 191) key[ki++] = *args++;
    key[ki] = 0;
    while (*args == ' ') args++;

    if (!key[0]) { term.write("Missing key path. Try 'reg help'.\n"); return; }

    if (!strcmp_(sub, "query")) {
        if (args[0]) {
            char val[64]; int vi = 0;
            while (*args && *args != ' ' && vi < 63) val[vi++] = *args++;
            val[vi] = 0;
            int r = win32_reg_query(key, val, g_regbuf, sizeof(g_regbuf));
            if (r < 0) { term.write("ERROR: value not found.\n"); return; }
            term.write(key); term.write("\n    ");
            term.write(val); term.write("    ");
            term.write(g_regbuf); term.put_char('\n');
        } else {
            int r = win32_reg_list(key, g_regbuf, sizeof(g_regbuf));
            if (r < 0) { term.write("ERROR: key not found.\n"); return; }
            term.write(g_regbuf);
        }
        return;
    }
    if (!strcmp_(sub, "list")) {
        int r = win32_reg_list(key, g_regbuf, sizeof(g_regbuf));
        if (r < 0) { term.write("ERROR: key not found.\n"); return; }
        term.write(g_regbuf);
        return;
    }
    if (!strcmp_(sub, "tree")) {
        int depth = 3;
        if (args[0] >= '1' && args[0] <= '9') depth = args[0] - '0';
        int r = win32_reg_tree(key, g_regbuf, sizeof(g_regbuf), depth);
        if (r < 0) { term.write("ERROR: key not found.\n"); return; }
        term.write(g_regbuf);
        return;
    }
    if (!strcmp_(sub, "add")) {
        int r = win32_reg_set(key, "", "SZ", "");
        term.write(r >= 0 ? "The operation completed successfully.\n"
                          : "ERROR: could not create key.\n");
        return;
    }
    if (!strcmp_(sub, "set")) {
        char val[64];  int vi = 0;
        while (*args && *args != ' ' && vi < 63) val[vi++] = *args++;
        val[vi] = 0; while (*args == ' ') args++;
        char type[16]; int ti = 0;
        while (*args && *args != ' ' && ti < 15) type[ti++] = *args++;
        type[ti] = 0; while (*args == ' ') args++;
        if (!val[0] || !type[0]) {
            term.write("Usage: reg set <key> <value> <SZ|DWORD|EXPAND|BIN|MULTI> <data>\n");
            return;
        }
        int r = win32_reg_set(key, val, type, args);
        term.write(r >= 0 ? "The operation completed successfully.\n"
                          : "ERROR: could not write the value.\n");
        return;
    }
    if (!strcmp_(sub, "delete") || !strcmp_(sub, "del")) {
        int r = win32_reg_delete(key, args[0] ? args : nullptr);
        term.write(r >= 0 ? "The operation completed successfully.\n"
                          : "ERROR: not found.\n");
        return;
    }
    term.write("Unknown sub-command. Try 'reg help'.\n");
}

static void cmd_winver(){
    win32_ensure_init();
    const char* K = "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    char v[128];
    term.set_color(make_color(CYAN, BLACK));
    term.write("=== Simulated Windows environment ===\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
    struct { const char* label; const char* value; } rows[] = {
        {"Product      ", "ProductName"},
        {"Version      ", "CurrentVersion"},
        {"Build        ", "CurrentBuild"},
        {"Display ver. ", "DisplayVersion"},
        {"Edition      ", "EditionID"},
        {"Registered   ", "RegisteredOwner"},
        {"System root  ", "SystemRoot"},
    };
    for (unsigned i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        if (win32_reg_query(K, rows[i].value, v, sizeof(v)) >= 0) {
            term.write(rows[i].label); term.write(": "); term.write(v); term.put_char('\n');
        }
    }
    const char* CPU = "HKLM\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    if (win32_reg_query(CPU, "ProcessorNameString", v, sizeof(v)) >= 0) {
        term.write("Processor    : "); term.write(v); term.put_char('\n');
    }
    term.write("Registry     : ");
    term.write_dec(win32_reg_key_count());   term.write(" keys / ");
    term.write_dec(win32_reg_value_count()); term.write(" values\n");
}

static void cmd_winenv(const char* args){
    win32_ensure_init();
    while (*args == ' ') args++;
    if (args[0]) {
        const char* v = win32_env_get(args);
        if (!v) { term.write("Environment variable not defined.\n"); return; }
        term.write(args); term.write("="); term.write(v); term.put_char('\n');
        return;
    }
    int r = win32_env_list(g_regbuf, sizeof(g_regbuf));
    if (r < 0) { term.write("(no environment)\n"); return; }
    term.write(g_regbuf);
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

    // Triple-fault fallback
    static struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ __volatile__("lidt %0" :: "m"(null_idt));
    __asm__ __volatile__("int $0x03");

    for(;;){ __asm__ __volatile__("hlt"); }
}


struct User {
    bool     exists;
    char     name[USER_NAME_LEN];
    uint32_t uid;
    uint32_t gid;
    char     group[USER_GROUP_LEN];
    char     hash[17];            // FNV-1a 32-bit, 8 hex chars + null
};
static User     g_users[MAX_USERS];
static int      g_user_count = 0;
static int      g_login_idx = -1;    // 当前登录用户索引 (-1 = 未登录)
static uint32_t g_euid = 0;          // effective uid (root when sudo active)
static bool     g_sudo_active = false;  // sudo 提权中 (effective uid = 0)


static void run_command(const char* line);   // forward (defined below)

// ---- FNV-1a hash ----
static uint32_t fnv1a(const char* s){
    uint32_t h = 2166136261u;
    while(*s){ h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
// hash = fnv1a(username + password) as 8 hex chars
static void hash_password(const char* user, const char* pw, char out[17]){
    char tmp[64]; int i = 0;
    while(user[i] && i < 31){ tmp[i] = user[i]; i++; }
    const char* p = pw;
    while(*p && i < 63){ tmp[i++] = *p++; }
    tmp[i] = 0;
    uint32_t h = fnv1a(tmp);
    for(int j = 0; j < 8; j++){
        uint8_t d = (h >> (28 - j*4)) & 0xF;
        out[j] = (d < 10) ? ('0' + d) : ('a' + d - 10);
    }
    out[8] = 0;
}

// ---- DB text helpers ----
static void db_append(char* buf, int* len, int cap, const char* s){
    while(*s && *len < cap - 1) buf[(*len)++] = *s++;
}
static void db_append_u32(char* buf, int* len, int cap, uint32_t v){
    char t[12]; int ti = 0;
    if(v == 0) t[ti++] = '0';
    while(v && ti < 11){ t[ti++] = '0' + (int)(v % 10); v /= 10; }
    while(ti > 0 && *len < cap - 1) buf[(*len)++] = t[--ti];
}
// Split line on ':' into up to max fields (replaces ':' with 0).
static int db_split(char* line, char* fields[], int max){
    int n = 0;
    char* p = line;
    while(*p && n < max){
        fields[n++] = p;
        while(*p && *p != ':') p++;
        if(*p == ':') *p++ = 0;
    }
    return n;
}

// ---- User database (persisted as "shadow" in MKFS root) ----
static void userdb_save(){
    if(!mkfs.mounted) return;
    char buf[512]; int len = 0;
    for(int i = 0; i < g_user_count; i++){
        if(!g_users[i].exists) continue;
        db_append(buf, &len, sizeof(buf), g_users[i].name);
        db_append(buf, &len, sizeof(buf), ":");
        db_append_u32(buf, &len, sizeof(buf), g_users[i].uid);
        db_append(buf, &len, sizeof(buf), ":");
        db_append_u32(buf, &len, sizeof(buf), g_users[i].gid);
        db_append(buf, &len, sizeof(buf), ":");
        db_append(buf, &len, sizeof(buf), g_users[i].group);
        db_append(buf, &len, sizeof(buf), ":");
        db_append(buf, &len, sizeof(buf), g_users[i].hash);
        db_append(buf, &len, sizeof(buf), "\n");
    }
    uint16_t saved = g_cwd;
    g_cwd = FS_ROOT_PARENT;
    mkfs.create("shadow", (const uint8_t*)buf, len);
    g_cwd = saved;
}

static void userdb_load(){
    g_user_count = 0;
    for(int i = 0; i < MAX_USERS; i++){ g_users[i].exists = false; g_users[i].name[0] = 0; }
    if(!mkfs.mounted) return;
    uint16_t saved = g_cwd;
    g_cwd = FS_ROOT_PARENT;
    int rd = mkfs.read("shadow", g_iobuf, FS_IOBUF_SIZE - 1);
    g_cwd = saved;
    if(rd <= 0) return;
    ((char*)g_iobuf)[rd] = 0;
    char* p = (char*)g_iobuf;
    while(*p && g_user_count < MAX_USERS){
        char* nl = p;
        while(*nl && *nl != '\n') nl++;
        if(*nl == '\n') *nl = 0;
        char* f[5];
        int n = db_split(p, f, 5);
        if(n >= 5 && f[0][0]){
            int idx = g_user_count++;
            g_users[idx].exists = true;
            int i = 0; while(f[0][i] && i < USER_NAME_LEN - 1){ g_users[idx].name[i] = f[0][i]; i++; }
            g_users[idx].name[i] = 0;
            g_users[idx].uid = 0; { const char* q = f[1]; while(*q){ g_users[idx].uid = g_users[idx].uid*10 + (*q - '0'); q++; } }
            g_users[idx].gid = 0; { const char* q = f[2]; while(*q){ g_users[idx].gid = g_users[idx].gid*10 + (*q - '0'); q++; } }
            i = 0; while(f[3][i] && i < USER_GROUP_LEN - 1){ g_users[idx].group[i] = f[3][i]; i++; }
            g_users[idx].group[i] = 0;
            i = 0; while(f[4][i] && i < 16){ g_users[idx].hash[i] = f[4][i]; i++; }
            g_users[idx].hash[i] = 0;
        }
        if(*nl == 0) break;
        p = nl + 1;
    }
}

static void seed_default_users(){
    if(g_user_count > 0) return;
    // root / admin (uid 0, gid 0, group root)
    g_users[0].exists = true;
    int i = 0; const char* rn = "root";
    while(rn[i] && i < USER_NAME_LEN - 1){ g_users[0].name[i] = rn[i]; i++; }
    g_users[0].name[i] = 0;
    g_users[0].uid = 0; g_users[0].gid = 0;
    i = 0; const char* rg = "root";
    while(rg[i] && i < USER_GROUP_LEN - 1){ g_users[0].group[i] = rg[i]; i++; }
    g_users[0].group[i] = 0;
    hash_password("root", "admin", g_users[0].hash);
    // guest / guest (uid 1000, gid 1000, group users)
    g_users[1].exists = true;
    i = 0; const char* gn = "guest";
    while(gn[i] && i < USER_NAME_LEN - 1){ g_users[1].name[i] = gn[i]; i++; }
    g_users[1].name[i] = 0;
    g_users[1].uid = 1000; g_users[1].gid = 1000;
    i = 0; const char* gg = "users";
    while(gg[i] && i < USER_GROUP_LEN - 1){ g_users[1].group[i] = gg[i]; i++; }
    g_users[1].group[i] = 0;
    hash_password("guest", "guest", g_users[1].hash);
    // nexos / nexos (uid 1000, gid 1000, group users) -- the default ops account
    g_users[2].exists = true;
    i = 0; const char* nn = "nexos";
    while(nn[i] && i < USER_NAME_LEN - 1){ g_users[2].name[i] = nn[i]; i++; }
    g_users[2].name[i] = 0;
    g_users[2].uid = 1000; g_users[2].gid = 1000;
    i = 0; const char* ng = "users";
    while(ng[i] && i < USER_GROUP_LEN - 1){ g_users[2].group[i] = ng[i]; i++; }
    g_users[2].group[i] = 0;
    hash_password("nexos", "nexos", g_users[2].hash);
    g_user_count = 3;
    userdb_save();
}

// ---- Permission table (persisted as "permdb" in MKFS root) ----
static void permdb_save(){
    if(!mkfs.mounted) return;
    char buf[2048]; int len = 0;
    for(int i = 0; i < g_perm_count; i++){
        // name:uid:gid:mode(3 octal digits)\n
        db_append(buf, &len, sizeof(buf), g_perms[i].name);
        db_append(buf, &len, sizeof(buf), ":");
        db_append_u32(buf, &len, sizeof(buf), g_perms[i].uid);
        db_append(buf, &len, sizeof(buf), ":");
        db_append_u32(buf, &len, sizeof(buf), g_perms[i].gid);
        db_append(buf, &len, sizeof(buf), ":");
        char m3[4];
        m3[0] = '0' + ((g_perms[i].mode >> 6) & 7);
        m3[1] = '0' + ((g_perms[i].mode >> 3) & 7);
        m3[2] = '0' + (g_perms[i].mode & 7);
        m3[3] = 0;
        db_append(buf, &len, sizeof(buf), m3);
        db_append(buf, &len, sizeof(buf), "\n");
    }
    uint16_t saved = g_cwd;
    g_cwd = FS_ROOT_PARENT;
    mkfs.create("permdb", (const uint8_t*)buf, len);
    g_cwd = saved;
}

static void permdb_load(){
    g_perm_count = 0;
    if(!mkfs.mounted) return;
    uint16_t saved = g_cwd;
    g_cwd = FS_ROOT_PARENT;
    int rd = mkfs.read("permdb", g_iobuf, FS_IOBUF_SIZE - 1);
    g_cwd = saved;
    if(rd <= 0) return;
    ((char*)g_iobuf)[rd] = 0;
    char* p = (char*)g_iobuf;
    while(*p && g_perm_count < MAX_PERMS){
        char* nl = p;
        while(*nl && *nl != '\n') nl++;
        if(*nl == '\n') *nl = 0;
        char* f[4];
        int n = db_split(p, f, 4);
        if(n >= 4 && f[0][0]){
            int idx = g_perm_count++;
            int i = 0; while(f[0][i] && i < FS_NAME_LEN - 1){ g_perms[idx].name[i] = f[0][i]; i++; }
            g_perms[idx].name[i] = 0;
            g_perms[idx].uid = 0; { const char* q = f[1]; while(*q){ g_perms[idx].uid = g_perms[idx].uid*10 + (*q - '0'); q++; } }
            g_perms[idx].gid = 0; { const char* q = f[2]; while(*q){ g_perms[idx].gid = g_perms[idx].gid*10 + (*q - '0'); q++; } }
            // octal mode: 3 digits -> 9-bit mode value
            uint32_t m = 0;
            { const char* q = f[3]; while(*q){ m = m*8 + (*q - '0'); q++; } }
            g_perms[idx].mode = (uint16_t)m;
        }
        if(*nl == 0) break;
        p = nl + 1;
    }
}

// mode <-> octal helpers (rwx rwx rwx <-> 3 octal digits)
// mode is already the standard 9-bit value; octal input converts directly
static uint16_t mode_from_octal(uint32_t m){
    return (uint16_t)(m & 0x1FF);
}

static bool perm_get(const char* name, uint32_t* uid, uint32_t* gid, uint16_t* mode){
    for(int i = 0; i < g_perm_count; i++){
        if(strcmp_(g_perms[i].name, name) == 0){
            *uid = g_perms[i].uid; *gid = g_perms[i].gid; *mode = g_perms[i].mode;
            return true;
        }
    }
    *uid = 0; *gid = 0; *mode = DEFAULT_FILE_MODE;
    return false;
}

static void perm_set(const char* name, uint32_t uid, uint32_t gid, uint16_t mode){
    for(int i = 0; i < g_perm_count; i++){
        if(strcmp_(g_perms[i].name, name) == 0){
            g_perms[i].uid = uid; g_perms[i].gid = gid; g_perms[i].mode = mode;
            permdb_save();
            return;
        }
    }
    if(g_perm_count < MAX_PERMS){
        int idx = g_perm_count++;
        int i = 0; while(name[i] && i < FS_NAME_LEN - 1){ g_perms[idx].name[i] = name[i]; i++; }
        g_perms[idx].name[i] = 0;
        g_perms[idx].uid = uid; g_perms[idx].gid = gid; g_perms[idx].mode = mode;
        permdb_save();
    }
}

// ---- Current identity ----
static int cur_uid(){ return g_sudo_active ? 0 : (g_login_idx >= 0 ? (int)g_users[g_login_idx].uid : 0); }
static int cur_gid(){ return g_sudo_active ? 0 : (g_login_idx >= 0 ? (int)g_users[g_login_idx].gid : 0); }
static bool is_root(){ return cur_uid() == 0; }

// ---- Permission check: need = 'r' | 'w' | 'x' ----
static bool perm_check(const char* name, char need, bool quiet){
    if(is_root()) return true;
    uint32_t uid, gid; uint16_t mode;
    perm_get(name, &uid, &gid, &mode);
    uint32_t cu = (uint32_t)cur_uid();
    uint32_t cg = (uint32_t)cur_gid();
    uint8_t bits = 0;
    if(cu == uid){
        bits = (need=='r') ? (mode & P_OWNER_R) : (need=='w') ? (mode & P_OWNER_W) : (mode & P_OWNER_X);
    } else if(cg == gid || gid == 0){
        bits = (need=='r') ? (mode & P_GRP_R) : (need=='w') ? (mode & P_GRP_W) : (mode & P_GRP_X);
    } else {
        bits = (need=='r') ? (mode & P_OTH_R) : (need=='w') ? (mode & P_OTH_W) : 0;
    }
    if(!bits){
        if(!quiet){
            term.write("Permission denied: "); term.write(name);
            term.write(" (need "); term.put_char(need); term.write(")\n");
        }
        return false;
    }
    return true;
}

// ---- Read a line from keyboard (password: hidden with '*') ----
static void read_line_quiet(char* buf, int* len, bool hidden){
    *len = 0;
    kbd.reset();
    for(;;){
        uint8_t st = inb(0x64);
        if(st == 0xFF) st = 0;      // no i8042 (floating bus) - treat as idle
        if(!(st & 0x01)) continue;
        uint8_t data = inb(0x60);
        if(st & 0x20) continue;             // mouse event
        KbdEvent e = kbd.process(data);
        if(e.type != K_CHAR) continue;
        if(e.ch == '\n'){
            term.put_char('\n');
            term.render();
            buf[*len] = 0;
            return;
        }
        if(e.ch == '\b'){
            if(*len > 0){
                (*len)--;
                term.put_char('\b');
                term.render();
            }
            continue;
        }
        if(*len < 63){
            buf[(*len)++] = e.ch;
            term.put_char(hidden ? '*' : e.ch);
            term.render();
        }
    }
}

static void login_prompt(){
    term.write("\n=== NexOS Security Login ===\n");
    for(;;){
        term.set_color(make_color(LIGHT_GREY, BLACK));
        term.write("login: ");
        term.render();
        char user[32]; int ulen = 0;
        read_line_quiet(user, &ulen, false);
        int idx = -1;
        for(int i = 0; i < g_user_count; i++){
            if(g_users[i].exists && strcmp_(g_users[i].name, user) == 0){ idx = i; break; }
        }
        if(idx < 0){
            term.write("Login incorrect\n");
            continue;
        }
        term.write("Password: ");
        term.render();
        char pw[64]; int plen = 0;
        read_line_quiet(pw, &plen, true);
        char hash[17];
        hash_password(g_users[idx].name, pw, hash);
        if(strcmp_(hash, g_users[idx].hash) != 0){
            term.write("Login incorrect\n");
            continue;
        }
        g_login_idx = idx;
        g_euid = g_users[idx].uid;
        g_sudo_active = false;
        term.write("Welcome, "); term.write(g_users[idx].name);
        term.write("! uid="); term.write_dec((int)g_users[idx].uid);
        term.write(" gid="); term.write_dec((int)g_users[idx].gid);
        term.write(" group="); term.write(g_users[idx].group);
        term.write("\n");
        term.render();
        return;
    }
}

// ---- User commands ----
static void cmd_whoami(){
    if(g_sudo_active){ term.write("root\n"); return; }
    if(g_login_idx < 0){ term.write("nobody\n"); return; }
    term.write(g_users[g_login_idx].name); term.put_char('\n');
}

static void cmd_id(){
    term.write("uid="); term.write_dec(cur_uid());
    term.write("(");
    if(is_root() && g_sudo_active) term.write("root");
    else if(g_login_idx >= 0) term.write(g_users[g_login_idx].name);
    else term.write("nobody");
    term.write(") gid="); term.write_dec(cur_gid());
    term.write(" groups=");
    if(g_login_idx >= 0) term.write(g_users[g_login_idx].group);
    else term.write("none");
    term.put_char('\n');
}

static void cmd_users(){
    term.write("Users on NexOS:\n");
    for(int i = 0; i < g_user_count; i++){
        if(!g_users[i].exists) continue;
        term.write("  "); term.write(g_users[i].name);
        term.write("  uid="); term.write_dec((int)g_users[i].uid);
        term.write("  gid="); term.write_dec((int)g_users[i].gid);
        term.write("  group="); term.write(g_users[i].group);
        if(g_users[i].uid == 0) term.write("  [root]");
        if(i == g_login_idx) term.write("  <-- you");
        term.put_char('\n');
    }
}

static void cmd_login(const char* arg){
    // Accept "login <user>" (interactive password) or "login <user> <password>"
    // (one-shot, used by the web ops console / bridge).
    char name[32]; int i = 0;
    while(*arg && *arg != ' ' && i < 31){ name[i++] = *arg++; }
    name[i] = 0;
    const char* pw = (*arg == ' ') ? arg + 1 : "";
    if(!name[0]){ term.write("Usage: login <username> [password]\n"); return; }
    int idx = -1;
    for(int i = 0; i < g_user_count; i++){
        if(g_users[i].exists && strcmp_(g_users[i].name, name) == 0){ idx = i; break; }
    }
    if(idx < 0){ term.write("User not found: "); term.write(name); term.put_char('\n'); return; }
    char hash[17];
    if(pw[0]){
        hash_password(g_users[idx].name, pw, hash);
        if(strcmp_(hash, g_users[idx].hash) != 0){
            term.write("Incorrect password.\n");
            return;
        }
    } else {
        term.write("Password: ");
        term.render();
        char buf[64]; int plen = 0;
        read_line_quiet(buf, &plen, true);
        hash_password(g_users[idx].name, buf, hash);
        if(strcmp_(hash, g_users[idx].hash) != 0){
            term.write("Incorrect password.\n");
            return;
        }
    }
    g_login_idx = idx;
    g_euid = g_users[idx].uid;
    g_sudo_active = false;
    term.write("Logged in as "); term.write(g_users[idx].name); term.write(".\n");
}

static void cmd_logout(){
    if(g_login_idx < 0){ term.write("Not logged in.\n"); return; }
    term.write("Logged out. Returning to login prompt.\n");
    g_login_idx = -1; g_euid = 0; g_sudo_active = false;
    login_prompt();
}

static void cmd_su(const char* name){
    const char* target = name[0] ? name : "root";
    int idx = -1;
    for(int i = 0; i < g_user_count; i++){
        if(g_users[i].exists && strcmp_(g_users[i].name, target) == 0){ idx = i; break; }
    }
    if(idx < 0){ term.write("User not found: "); term.write(target); term.put_char('\n'); return; }
    term.write("Password: ");
    term.render();
    char pw[64]; int plen = 0;
    read_line_quiet(pw, &plen, true);
    char hash[17];
    hash_password(g_users[idx].name, pw, hash);
    if(strcmp_(hash, g_users[idx].hash) != 0){
        term.write("Incorrect password.\n");
        return;
    }
    g_login_idx = idx;
    g_euid = g_users[idx].uid;
    g_sudo_active = false;
    term.write("Switched to user "); term.write(g_users[idx].name); term.write(".\n");
}

static void cmd_useradd(const char* args){
    if(!is_root()){ term.write("Permission denied: only root can add users (use 'sudo useradd ...').\n"); return; }
    char name[USER_NAME_LEN], pw[64];
    int i = 0;
    while(args[i] && args[i] != ' ' && i < USER_NAME_LEN - 1){ name[i] = args[i]; i++; }
    name[i] = 0;
    while(args[i] == ' ') i++;
    int j = 0;
    while(args[i] && args[i] != ' ' && j < 63){ pw[j++] = args[i++]; }
    pw[j] = 0;
    if(!name[0]){ term.write("Usage: useradd <username> [password]\n"); return; }
    // validate name (alnum only)
    for(const char* q = name; *q; q++){
        if(!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')){
            term.write("Invalid username (a-z A-Z 0-9 _ only)\n");
            return;
        }
    }
    for(int k = 0; k < g_user_count; k++){
        if(g_users[k].exists && strcmp_(g_users[k].name, name) == 0){
            term.write("User already exists: "); term.write(name); term.put_char('\n');
            return;
        }
    }
    if(g_user_count >= MAX_USERS){ term.write("User table full (max "); term.write_dec(MAX_USERS); term.write(")\n"); return; }
    if(!pw[0]){ int k = 0; const char* d = "123456"; while(d[k] && k < 63){ pw[k] = d[k]; k++; } pw[k] = 0; }
    int idx = g_user_count++;
    g_users[idx].exists = true;
    i = 0; while(name[i] && i < USER_NAME_LEN - 1){ g_users[idx].name[i] = name[i]; i++; }
    g_users[idx].name[i] = 0;
    g_users[idx].uid = 1000 + (idx - 1);   // root=0, others start at 1000
    g_users[idx].gid = g_users[idx].uid;
    i = 0; const char* grp = "users";
    while(grp[i] && i < USER_GROUP_LEN - 1){ g_users[idx].group[i] = grp[i]; i++; }
    g_users[idx].group[i] = 0;
    hash_password(name, pw, g_users[idx].hash);
    userdb_save();
    term.write("User added: "); term.write(name);
    term.write(" (uid="); term.write_dec((int)g_users[idx].uid);
    term.write(", password="); term.write(pw[0] ? pw : "(none)");
    term.write(")\n");
}

static void cmd_deluser(const char* name){
    if(!is_root()){ term.write("Permission denied: only root can delete users.\n"); return; }
    if(!name[0]){ term.write("Usage: deluser <username>\n"); return; }
    if(strcmp_(name, "root") == 0){ term.write("Cannot delete root.\n"); return; }
    int idx = -1;
    for(int i = 0; i < g_user_count; i++){
        if(g_users[i].exists && strcmp_(g_users[i].name, name) == 0){ idx = i; break; }
    }
    if(idx < 0){ term.write("User not found: "); term.write(name); term.put_char('\n'); return; }
    if(idx == g_login_idx){ term.write("Cannot delete the user you are logged in as.\n"); return; }
    g_users[idx].exists = false;
    g_users[idx].name[0] = 0;
    userdb_save();
    term.write("User deleted: "); term.write(name); term.put_char('\n');
}

static void cmd_passwd(const char* args){
    // passwd [user]  (root can change anyone; a normal user changes own password)
    char target[USER_NAME_LEN];
    int i = 0;
    while(args[i] && args[i] != ' ' && i < USER_NAME_LEN - 1){ target[i] = args[i]; i++; }
    target[i] = 0;
    int idx = g_login_idx;
    if(target[0]){
        int t = -1;
        for(int k = 0; k < g_user_count; k++){
            if(g_users[k].exists && strcmp_(g_users[k].name, target) == 0){ t = k; break; }
        }
        if(t < 0){ term.write("User not found: "); term.write(target); term.put_char('\n'); return; }
        if(t != g_login_idx && !is_root()){
            term.write("Permission denied: only root can change other users' passwords.\n");
            return;
        }
        idx = t;
    }
    if(idx < 0){ term.write("Not logged in.\n"); return; }
    // verify old password unless root
    if(!is_root()){
        term.write("Current password: ");
        term.render();
        char old[64]; int ol = 0;
        read_line_quiet(old, &ol, true);
        char h[17];
        hash_password(g_users[idx].name, old, h);
        if(strcmp_(h, g_users[idx].hash) != 0){
            term.write("Incorrect password.\n");
            return;
        }
    }
    term.write("New password: ");
    term.render();
    char pw1[64]; int l1 = 0;
    read_line_quiet(pw1, &l1, true);
    term.write("Confirm: ");
    term.render();
    char pw2[64]; int l2 = 0;
    read_line_quiet(pw2, &l2, true);
    if(strcmp_(pw1, pw2) != 0){ term.write("Passwords do not match.\n"); return; }
    if(!pw1[0]){ term.write("Password cannot be empty.\n"); return; }
    hash_password(g_users[idx].name, pw1, g_users[idx].hash);
    userdb_save();
    term.write("Password changed for "); term.write(g_users[idx].name); term.write(".\n");
}

// ---- chmod ----
static void cmd_chmod(const char* args){
    char m3[4]; char fname[FS_NAME_LEN];
    int i = 0;
    while(args[i] && args[i] != ' ' && i < 3){ m3[i] = args[i]; i++; }
    m3[i] = 0;
    while(args[i] == ' ') i++;
    int j = 0;
    while(args[i] && args[i] != ' ' && j < FS_NAME_LEN - 1){ fname[j++] = args[i++]; }
    fname[j] = 0;
    if(!fname[0]){ term.write("Usage: chmod <mode3> <file>   (e.g. chmod 644 file.txt)\n"); return; }
    if(strlen_(m3) != 3){ term.write("Mode must be 3 octal digits (e.g. 644, 755)\n"); return; }
    uint32_t m = 0;
    for(int k = 0; k < 3; k++){
        if(m3[k] < '0' || m3[k] > '7'){ term.write("Mode must be octal (0-7)\n"); return; }
        m = m * 8 + (m3[k] - '0');
    }
    uint32_t uid, gid; uint16_t omode;
    perm_get(fname, &uid, &gid, &omode);
    if(!is_root() && (uint32_t)cur_uid() != uid){
        term.write("Permission denied: only the owner or root can chmod "); term.write(fname); term.put_char('\n');
        return;
    }
    uint16_t mode = mode_from_octal(m);
    perm_set(fname, uid, gid, mode);
    if(mkfs.mounted){
        // persist mode into FileEntry.reserved as well
        uint16_t saved = g_cwd;
        // mkfs.set_mode looks in current dir; keep cwd unchanged (files are in cwd)
        for(int s = 0; s < MKFS_TABLE_SECT; s++){
            fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for(int e = 0; e < FS_ENTRY_PER_SEC; e++){
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if(fe->name[0] != 0 && fe->parent == g_cwd && strcmp_(fe->name, fname) == 0){
                    fe->reserved = (uint8_t)(mode & 0xFF);
                    fs_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);
                    break;
                }
            }
        }
        (void)saved;
    }
    term.write("chmod: "); term.write(fname);
    term.write(" -> "); term.write(m3); term.write(" (octal)\n");
}

// ---- stat ----
static void cmd_stat(const char* name){
    if(!name[0]){ term.write("Usage: stat <file>\n"); return; }
    uint32_t uid, gid; uint16_t mode;
    bool in_db = perm_get(name, &uid, &gid, &mode);
    term.write("File: "); term.write(name); term.put_char('\n');
    term.write("  owner: uid="); term.write_dec((int)uid);
    // resolve owner name
    {
        const char* on = "?";
        for(int i = 0; i < g_user_count; i++)
            if(g_users[i].exists && g_users[i].uid == uid){ on = g_users[i].name; break; }
        term.write(" ("); term.write(on); term.write(")");
    }
    term.write("  gid="); term.write_dec((int)gid); term.put_char('\n');
    term.write("  mode: ");
    char bits[10];
    bits[0] = (mode & P_OWNER_R) ? 'r' : '-';
    bits[1] = (mode & P_OWNER_W) ? 'w' : '-';
    bits[2] = (mode & P_OWNER_X) ? 'x' : '-';
    bits[3] = (mode & P_GRP_R)   ? 'r' : '-';
    bits[4] = (mode & P_GRP_W)   ? 'w' : '-';
    bits[5] = (mode & P_GRP_X)   ? 'x' : '-';
    bits[6] = (mode & P_OTH_R)   ? 'r' : '-';
    bits[7] = (mode & P_OTH_W)   ? 'w' : '-';
    bits[8] = '-'; bits[9] = 0;
    term.write(bits);
    term.write("  "); term.write_dec((int)((mode >> 6) & 7));
    term.write_dec((int)((mode >> 3) & 7));
    term.write_dec((int)(mode & 7));
    term.write(" (octal)");
    if(!in_db) term.write("  [default: owner=root 0644]");
    term.put_char('\n');
    // read reserved from FS entry if present
    if(mkfs.mounted){
        for(int s = 0; s < MKFS_TABLE_SECT; s++){
            fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for(int e = 0; e < FS_ENTRY_PER_SEC; e++){
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if(fe->name[0] != 0 && fe->parent == g_cwd && strcmp_(fe->name, name) == 0){
                    term.write("  FS entry: size="); term.write_dec((int)fe->size);
                    term.write(" type="); term.write_dec((int)fe->type);
                    term.write(" mode=0x"); term.write_hex(fe->reserved);
                    term.put_char('\n');
                    return;
                }
            }
        }
    }
}

// ---- sudo ----
static void cmd_sudo(const char* args){
    if(!args[0]){ term.write("Usage: sudo <command> [args...]\n"); return; }
    if(g_login_idx < 0){ term.write("Not logged in.\n"); return; }
    term.write("[sudo] password for "); term.write(g_users[g_login_idx].name); term.write(": ");
    term.render();
    char pw[64]; int plen = 0;
    read_line_quiet(pw, &plen, true);
    char hash[17];
    hash_password(g_users[g_login_idx].name, pw, hash);
    if(strcmp_(hash, g_users[g_login_idx].hash) != 0){
        term.write("Sorry, try again.\n");
        return;
    }
    term.write("OK, running as root: "); term.write(args); term.put_char('\n');
    bool saved = g_sudo_active;
    g_sudo_active = true;
    run_command(args);
    g_sudo_active = saved;
    if(!saved){
        term.write("[sudo] returned to "); term.write(g_users[g_login_idx].name); term.put_char('\n');
    }
}

// =====================================================================
//  AI Engine commands
// =====================================================================

// AI code generator (defined in ai_engine.cpp) -- deterministic, no weights.
extern "C" int ai_generate_code(const char* intent, char* out, int outsize);

// Knowledge-base + reasoning framework hooks (kb.cpp / ai_engine.cpp).
#include "kb.h"
extern "C" int ai_reason(const char* prompt, char* out, int outsize);

// ---- Knowledge-base command (dynamic, rule-driven KB) -------------------
static void kb_console(const char* s){ term.write(s); serial_puts(s); }

static int kb_atoi(const char* s){
    if (!s) return -1;
    if (*s == '#') s++;
    int v = 0;
    while (*s >= '0' && *s <= '9'){ v = v * 10 + (*s - '0'); s++; }
    return v;
}
static void kb_itoa(int v, char* b){
    int i = 0;
    if (v == 0){ b[0] = '0'; b[1] = 0; return; }
    char t[12]; int n = 0;
    while (v){ t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) b[i++] = t[--n];
    b[i] = 0;
}
static void kb_report_status(int idx, int st){
    char b[16]; kb_itoa(idx, b);
    kb_console("KB #"); kb_console(b); kb_console(" -> status=");
    kb_console(st == KB_ACCEPTED ? "ACCEPTED" : (st == KB_REJECTED ? "REJECTED" : "CANDIDATE"));
    kb_console("\n");
}

static void cmd_kb(const char* args){
    kb_init();
    if (!*args){ kb_console("Usage: kb <add|prove|cite|list|query|info>\n"); return; }
    char sub[16]; int si = 0;
    while (*args && *args != ' ' && si < 15) sub[si++] = *args++;
    sub[si] = 0;
    while (*args == ' ') args++;

    if (!strcmp_(sub, "list")){
        char buf[2200];
        int n = kb_list(buf, (int)sizeof(buf));
        if (n > 0) kb_console(buf);
    } else if (!strcmp_(sub, "query")){
        if (!*args){ kb_console("Usage: kb query <keyword>\n"); return; }
        char buf[600];
        if (kb_query(args, buf, (int)sizeof(buf))){
            kb_console("[KB hit] "); kb_console(buf); kb_console("\n");
        } else {
            kb_console("[KB miss] no accepted fact matches.\n");
        }
    } else if (!strcmp_(sub, "add")){
        if (!*args){ kb_console("Usage: kb add <statement>\n"); return; }
        int id = kb_add(args);
        if (id < 0){ kb_console("KB: add failed (full or empty).\n"); return; }
        char b[16]; kb_itoa(id, b);
        kb_console("KB: added candidate #"); kb_console(b); kb_console(": ");
        kb_console(args); kb_console("\n");
    } else if (!strcmp_(sub, "prove")){
        char idtok[16]; int ii = 0;
        while (*args && *args != ' ' && ii < 15) idtok[ii++] = *args++;
        idtok[ii] = 0; while (*args == ' ') args++;
        int pass = (!strcmp_(args, "pass") || !strcmp_(args, "1") || !strcmp_(args, "true"));
        int idx = kb_atoi(idtok);
        int st = kb_prove(idx, pass);
        if (st < 0){ kb_console("KB: no such fact #"); kb_console(idtok); kb_console("\n"); }
        else kb_report_status(idx, st);
    } else if (!strcmp_(sub, "cite")){
        char idtok[16]; int ii = 0;
        while (*args && *args != ' ' && ii < 15) idtok[ii++] = *args++;
        idtok[ii] = 0; while (*args == ' ') args++;
        char src[KB_SRC_LEN]; int k = 0;
        while (*args && *args != ' ' && k < (int)sizeof(src) - 1) src[k++] = *args++;
        src[k] = 0; while (*args == ' ') args++;
        int idx = kb_atoi(idtok);
        int st = kb_cite(idx, src, args);
        if (st < 0){ kb_console("KB: no such fact #"); kb_console(idtok); kb_console("\n"); }
        else kb_report_status(idx, st);
    } else if (!strcmp_(sub, "info")){
        char idtok[16]; int ii = 0;
        while (*args && *args != ' ' && ii < 15) idtok[ii++] = *args++;
        idtok[ii] = 0;
        int idx = kb_atoi(idtok);
        char buf[1200];
        int n = kb_info(idx, buf, (int)sizeof(buf));
        if (n > 0) kb_console(buf);
        else { kb_console("KB: no such fact #"); kb_console(idtok); kb_console("\n"); }
    } else {
        kb_console("Usage: kb <add|prove|cite|list|query|info>\n");
    }
}

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
    } else if(!strcmp_(sub,"py") || !strcmp_(sub,"code")){
        // OS AI engine authors a Python program and runs it in the Linux
        // compat layer. Deterministic generator -- no model weights needed.
        term.write("NexOS AI: generating Python code");
        if (*args){ term.write(" for intent: "); term.write(args); }
        term.put_char('\n');
        static const unsigned int AI_CODE_PHYS = 0x0B800000u;  // 184 MiB (free guest RAM)
        char* code = (char*)AI_CODE_PHYS;
        int n = ai_generate_code(*args ? args : "hello world", code, 16384);
        if (n <= 0){ term.write("AI code generation failed.\n"); return; }
        serial_puts("[AI] generated Python source:\n");
        serial_puts(code);
        serial_puts("\n[AI] launching python interpreter on the generated code...\n");
        char arg1[32];
        { unsigned int a = AI_CODE_PHYS, l = (unsigned int)n; int i = 0;
          arg1[i++] = 'm'; arg1[i++] = 'e'; arg1[i++] = 'm'; arg1[i++] = ':';
          char t[16]; int ti;
          ti = 0; if (a==0) t[ti++]='0'; else { while(a){ t[ti++]=(char)('0'+(a%10)); a/=10; } } while(ti>0) arg1[i++]=t[--ti];
          arg1[i++] = ':';
          ti = 0; if (l==0) t[ti++]='0'; else { while(l){ t[ti++]=(char)('0'+(l%10)); l/=10; } } while(ti>0) arg1[i++]=t[--ti];
          arg1[i] = 0; }
        serial_puts("[AI] argv[1] = mem:"); serial_puts(arg1); serial_puts("\n");
        const char* av[3];
        av[0] = "python"; av[1] = arg1;
        linux_run("python", 2, av);
    } else if(!strcmp_(sub,"reason") || !strcmp_(sub,"infer")){
        if(!*args){ term.write("Usage: ai reason <prompt>\n"); return; }
        term.write("NexOS AI reasoning framework:\n");
        char out[1400];
        int n = ai_reason(args, out, (int)sizeof(out));
        if(n>0) term.write(out);
        else term.write("(reasoning failed)\n");
    } else {
        term.write("Usage: ai [init|info|mode|test|cleanup|py|reason]\n");
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

static void cmd_ask(const char* args){
    if(!g_ai_initialized){
        term.write("AI not initialized. Use 'ai init' (or 'agent init') first.\n");
        return;
    }
    if(!*args){
        term.write("Usage: ask <question>\n");
        return;
    }
    term.write(ai_env_is_vm()
               ? "AI (VM/built-in): "
               : "AI (bare-metal/real): ");
    char* result = ai_generate(args, 160);
    if(result){
        term.write(result);
        term.put_char('\n');
        kfree(result);
    } else {
        term.write("(no response)\n");
    }
}

// ---------------------------------------------------------------------
//  Plugin manager command
// ---------------------------------------------------------------------
static bool g_plugin_inited = false;

static void term_write_int(int v){
    char t[16]; int n = 0;
    if (v == 0) { t[0] = '0'; n = 1; }
    else { int neg = 0; if (v < 0) { neg = 1; v = -v; }
           while (v > 0 && n < 15) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
           if (neg && n < 15) t[n++] = '-'; }
    for (int k = n - 1; k >= 0; k--) term.put_char(t[k]);
}

static void cmd_plugin(const char* args){
    if (!g_plugin_inited) { ai_plugin_init(); g_plugin_inited = true; }

    // No args -> list
    if (!*args) { char buf[3072]; ai_plugin_list(buf, sizeof(buf)); term.write(buf); return; }

    char sub[16]; int si = 0;
    while (*args && *args != ' ' && si < 15) sub[si++] = *args++;
    sub[si] = 0;
    while (*args == ' ') args++;

    if (!strcmp_(sub, "list") || !strcmp_(sub, "ls")) {
        char buf[3072]; ai_plugin_list(buf, sizeof(buf)); term.write(buf);
    } else if (!strcmp_(sub, "persist")) {
        char buf[3072]; int n = ai_plugin_serialize(buf, sizeof(buf));
        int r = kern_fs_create("plugins.lst", (const unsigned char*)buf, n);
        if (r >= 0) { term.write("Plugin catalogue written to plugins.lst ("); term_write_int(n); term.write(" bytes)\n"); }
        else term.write("Failed to write plugins.lst\n");
    } else if (!strcmp_(sub, "toggle") || !strcmp_(sub, "load") || !strcmp_(sub, "unload")) {
        if (!*args) { term.write("Usage: plugin toggle <id>\n"); return; }
        int want = !strcmp_(sub, "unload") ? 0 : 1;
        int s = (!strcmp_(sub, "toggle")) ? ai_plugin_toggle(args) : ai_plugin_set(args, want);
        if (s < 0) { term.write("plugin not found: "); term.write(args); term.put_char('\n'); return; }
        char buf[3072]; int n = ai_plugin_serialize(buf, sizeof(buf));
        kern_fs_create("plugins.lst", (const unsigned char*)buf, n);
        term.write(args); term.write(s ? " -> loaded\n" : " -> unloaded\n");
    } else if (!strcmp_(sub, "info")) {
        if (!*args) { term.write("Usage: plugin info <id>\n"); return; }
        int i = ai_plugin_find(args);
        if (i < 0) { term.write("plugin not found: "); term.write(args); term.put_char('\n'); return; }
        AiPlugin* pl = &g_plugins[i];
        term.write("Plugin: "); term.write(pl->id); term.put_char('\n');
        term.write("  Name : "); term.write(pl->name); term.put_char('\n');
        term.write("  Deps : "); term.write(pl->deps); term.put_char('\n');
        term.write("  Mem  : "); term_write_int(pl->mem_kb); term.write(" KB\n");
        term.write("  State: "); term_write_int(pl->state); term.write(" (0 planned,1 basic,2 available)\n");
        term.write("  Load : "); term_write_int(pl->loaded); term.put_char('\n');
    } else if (!strcmp_(sub, "run")) {
        if (!*args) { term.write("Usage: plugin run <id>\n"); return; }
        if (!strcmp_(args, "nexos.ai.inference")) {
            term.write("Loading core AI inference plugin...\n");
            int ret = ai_init("/boot/model.gguf");
            if (ret == 0) { g_ai_initialized = true; term.write("AI inference engine loaded.\n"); }
            else term.write("AI init failed (no model?).\n");
        } else {
            term.write("Plugin '"); term.write(args); term.write("' is not runnable in this build.\n");
        }
    } else {
        term.write("Usage: plugin [list|persist|toggle|load|unload|info|run] [<id>]\n");
    }
}

// P4 bridge: create/overwrite a file on the writable MKFS volume, used by the
// skill system (skill.cpp) so skills can call system FS APIs without reaching
// into kernel internals. Returns >=0 bytes written, <0 on error.
int kern_fs_create(const char* name, const unsigned char* data, int len){
    if (!mkfs.mounted) return -2;
    return mkfs.create(name, (const uint8_t*)data, len);
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
            // Respect the return code: ai_init can fail to reserve the 4 MB
            // Markov table or the GPT weights.  It used to be ignored, so the
            // flag was set anyway and the next `agent run` walked NULL model
            // pointers.
            if(ai_init("/boot/model.gguf") != 0){
                term.write("AI engine init FAILED (out of kernel heap).\n");
                term.write("  Agent framework not started.\n");
                return;
            }
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
        // P4: try the skill registry first (natural-language intent -> system API).
        char skill_out[256];
        if(agent_skill_dispatch(args, skill_out, (int)sizeof(skill_out))){
            term.write("[Skill] "); term.write(skill_out); term.put_char('\n');
            return;
        }
        // No verbose pipeline text here: while the pipeline runs, the caller
        // shows a status element (the AI desktop's "思考中…" indicator) instead
        // of textual progress, and agent_run() returns ONLY the final answer.
        //
        // First try to REALLY answer the question: ask the host's local LLM
        // (LM Studio / Ollama) through the QEMU user-network bridge.  The
        // built-in Markov engine is only a fallback for offline/VM use.
        char remote[4096];
        if (net_ask_host(args, remote, (int)sizeof(remote)) > 0){
            term.write(remote);
            term.put_char('\n');
            return;
        }
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
    } else if(!strcmp_(sub,"skills")){
        term.write("Registered skills:\n");
        char buf[512];
        agent_skill_list(buf, (int)sizeof(buf));
        term.write(buf);
    } else if(!strcmp_(sub,"abort")){
        agent_abort();
        term.write("Agent abort requested.\n");
    } else if(!strcmp_(sub,"confirm")){
        // `agent confirm on|off` toggles the dangerous-task gate.
        if(!strcmp_(args,"off")){ agent_set_confirm(0); term.write("Confirm mode OFF.\n"); }
        else { agent_set_confirm(1); term.write("Confirm mode ON (dangerous tasks blocked).\n"); }
    } else {
        term.write("Usage: agent [init|run|status|skills|abort|confirm]\n");
    }
}

// Forward declaration: defined much later (near the GUI callback table), but
// cmd_model() below needs it for `model recognize` / `model run`.
static int gui_cb_read_file(int fs_type, const char* name, uint8_t* buf, int bufsize);

// The 32-bit kernel links without libgcc, so a 64-bit `/ 1000000ULL` would
// emit an undefined reference to __udivdi3.  A constant right-shift compiles
// to plain shrd/shr, so report sizes in MiB instead.
static uint32_t approx_mb(unsigned long long bytes){ return (uint32_t)(bytes >> 20); }

static void cmd_model(const char* args){
    if(!*args){
        const struct KnownModel* d = ai_model_default();
        term.write("Model subsystem. Default: ");
        if (d) term.write(d->name); else term.write("(none)");
        term.write("\n  Usage: model [list|info|set-default|recognize|download|run|selftest]\n");
        return;
    }
    char sub[16]; int si=0;
    while(*args && *args!=' ' && si<15) sub[si++]=*args++;
    sub[si]=0;
    while(*args==' ') args++;

    if(!strcmp_(sub,"env")){
        term.write("Environment: ");
        term.write(ai_env_desc());
        if (ai_env_is_vm())
            term.write("  (virtual machine -> built-in engine, no real weights)\n");
        else
            term.write("  (bare metal -> real transformer inference enabled)\n");
    }
    else if(!strcmp_(sub,"list")){
        for (int i=0;i<ai_model_count();i++){
            const struct KnownModel* m = ai_model_get(i);
            term.write((m == ai_model_default()) ? " *" : "  ");
            term.write(m->name); term.write("  ");
            term.write(m->family); term.write("  ");
            term.write(m->params_str); term.write("  ");
            term.write(ai_model_fmt_name(m->fmt)); term.write("  ~");
            char sz[16]; uint_to_str(approx_mb(m->approx_size), sz);
            term.write(sz); term.write(" MB\n");
        }
        term.write("  (* = default model)\n");
    }
    else if(!strcmp_(sub,"info")){
        const struct KnownModel* m = *args ? ai_model_find(args) : ai_model_default();
        if(!m){ term.write("Unknown model. Use 'model list'.\n"); return; }
        term.write("Model: "); term.write(m->name); term.write("\n");
        term.write("  Family: "); term.write(m->family); term.write("\n");
        term.write("  Params: "); term.write(m->params_str); term.write("\n");
        term.write("  Format: "); term.write(ai_model_fmt_name(m->fmt)); term.write("\n");
        term.write("  Quant:  "); term.write(m->quant); term.write("\n");
        term.write("  Size: ~"); char sz[16]; uint_to_str(approx_mb(m->approx_size), sz);
        term.write(sz); term.write(" MB\n");
        term.write("  URL: "); term.write(m->url); term.write("\n");
    }
    else if(!strcmp_(sub,"set-default")){
        if(!*args){ term.write("Usage: model set-default <name>\n"); return; }
        if(ai_model_set_default(args)==0){ term.write("Default model set to: "); term.write(args); term.write("\n"); }
        else term.write("Unknown model name.\n");
    }
    else if(!strcmp_(sub,"recognize")){
        if(!*args){ term.write("Usage: model recognize <file>\n"); return; }
        uint8_t buf[8192];
        int n = gui_cb_read_file(1, args, buf, (int)sizeof(buf));   // SFS
        if(n < 0) n = gui_cb_read_file(0, args, buf, (int)sizeof(buf)); // MKFS
        if(n < 0){ term.write("Cannot read file: "); term.write(args); term.write("\n"); return; }
        struct ModelInfo info;
        int fmt = ai_model_recognize_mem(buf, n, &info);
        term.write("Format: "); term.write(ai_model_fmt_name(fmt)); term.write("\n");
        term.write("  Family: "); term.write(info.family[0]?info.family:"(unknown)"); term.write("\n");
        term.write("  Name:   "); term.write(info.name[0]?info.name:"(unknown)"); term.write("\n");
        term.write("  Quant:  "); term.write(info.quant[0]?info.quant:"(unknown)"); term.write("\n");
        term.write("  Params: "); char pn[24]; uint_to_str((uint32_t)info.params, pn); term.write(pn); term.write("\n");
        term.write("  Bytes read: "); char bn[16]; uint_to_str((uint32_t)n, bn); term.write(bn); term.write("\n");
    }
    else if(!strcmp_(sub,"download")){
        const struct KnownModel* m = *args ? ai_model_find(args) : ai_model_default();
        if(!m){ term.write("Unknown model. Use 'model list'.\n"); return; }
        term.write("Model: "); term.write(m->name); term.write("\n");
        term.write("  URL: "); term.write(m->url); term.write("\n");
        term.write("  Expected size: ~"); char sz[16]; uint_to_str(approx_mb(m->approx_size), sz);
        term.write(sz); term.write(" MB\n");
        term.write("  Attempting fetch (best-effort)...\n");
        if(!g_net_initialized){ term.write("  Network not initialized. Use 'netstart'.\n"); return; }
        char buf[65536];
        int got = net_http_get(m->url, buf, (int)sizeof(buf));
        if(got > 0){
            term.write("  Fetched "); char gn[16]; uint_to_str((uint32_t)got, gn); term.write(gn);
            term.write(" bytes. Full model is GB-scale and cannot be stored in this OS.\n");
        } else {
            term.write("  Fetch failed / host unreachable from emulated NIC.\n");
        }
    }
    else if(!strcmp_(sub,"run")){
        const struct KnownModel* m = 0;
        if(*args) m = ai_model_find(args);
        if(!m){
            if(!*args){ term.write("Usage: model run <name|file>\n"); return; }
            uint8_t buf[8192];
            int n = gui_cb_read_file(1, args, buf, (int)sizeof(buf));
            if(n < 0) n = gui_cb_read_file(0, args, buf, (int)sizeof(buf));
            if(n < 0){ term.write("Cannot read: "); term.write(args); term.write("\n"); return; }
            struct ModelInfo info;
            int fmt = ai_model_recognize_mem(buf, n, &info);
            term.write("Recognized "); term.write(ai_model_fmt_name(fmt));
            term.write(" ("); term.write(info.family[0]?info.family:info.name[0]?info.name:"?"); term.write(").\n");
            ai_model_set_active_name(args);
        } else {
            ai_model_set_active_name(m->name);
            term.write("Selected model: "); term.write(m->name);
            term.write(" ("); term.write(m->family); term.write(", "); term.write(m->params_str);
            term.write(", "); term.write(ai_model_fmt_name(m->fmt)); term.write(")\n");
        }
        if(!g_ai_initialized){ int r = ai_init("/boot/model.gguf"); if(r==0) g_ai_initialized = true; }
        if (ai_env_is_vm())
            term.write("VM detected: using built-in engine (no real weights loaded).\n");
        else
            term.write("Bare metal detected: real transformer inference enabled.\n");
        term.write("AI engine ready. Inference uses the on-board mini-engine;\n");
        term.write("the 1.7B weights need >1GB RAM, so this build routes them\n");
        term.write("through the built-in model. Use 'ask' / 'agent run'.\n");
    }
    else if(!strcmp_(sub,"selftest")){
        uint8_t hdr[96]; int o=0;
        hdr[o++]='G'; hdr[o++]='G'; hdr[o++]='U'; hdr[o++]='F';
        hdr[o++]=3; hdr[o++]=0; hdr[o++]=0; hdr[o++]=0;          // version 3
        for(int i=0;i<8;i++) hdr[o++]=0;                        // tensor_count=0
        hdr[o++]=1; for(int i=1;i<8;i++) hdr[o++]=0;            // kv_count=1
        hdr[o++]=19; for(int i=1;i<8;i++) hdr[o++]=0;           // key len 19
        { const char* k="general.architecture"; for(int i=0;i<19;i++) hdr[o++]=k[i]; }
        hdr[o++]=8; hdr[o++]=0; hdr[o++]=0; hdr[o++]=0;         // STRING
        hdr[o++]=5; for(int i=1;i<8;i++) hdr[o++]=0;            // value len 5
        hdr[o++]='q'; hdr[o++]='w'; hdr[o++]='e'; hdr[o++]='n'; hdr[o++]='2';
        int len=o; while(o<(int)sizeof(hdr)) hdr[o++]=0;
        struct ModelInfo info;
        int fmt = ai_model_recognize_mem(hdr, len, &info);
        term.write("selftest -> fmt="); term.write(ai_model_fmt_name(fmt));
        term.write(" family="); term.write(info.family[0]?info.family:"?");
        term.write(" (expected GGUF/qwen2)\n");
    }
    else {
        term.write("Usage: model [list|info|set-default|recognize|download|run|selftest]\n");
    }
}

// =====================================================================
//  AI bridge for native PE programs
//  win32.cpp publishes these three as NexOS.dll!MiniAi*.  Keeping the
//  implementation here means the engine has one owner and one "is it
//  up?" flag: `ai init` from the shell and the browser's Ask button
//  drive the same instance instead of racing two of them.
// =====================================================================
extern "C" int kern_ai_ready(void){ return g_ai_initialized ? 1 : 0; }

extern "C" int kern_ai_boot(void){
    if(g_ai_initialized) return 0;
    int r = ai_init("/boot/model.gguf");
    if(r == 0) g_ai_initialized = true;
    return r;
}

extern "C" int kern_ai_ask(const char* prompt, char* out, int outsz){
    if(!prompt || !out || outsz < 2) return -1;
    if(!g_ai_initialized && kern_ai_boot() != 0) return -2;
    char* r = ai_generate(prompt, 160);
    if(!r) return -3;
    int i = 0;
    while(r[i] && i < outsz - 1){ out[i] = r[i]; i++; }
    out[i] = 0;
    kfree(r);
    return i;
}

// =====================================================================
//  webapi  -  authenticated agent bridge into the browser start page
//
//  The browser is a real PE with a real window procedure, so an agent
//  does not need to synthesise pixels to drive it: this command posts
//  the private WM_NexOS_API message straight into iexplore.exe's
//  WndProc and prints whatever it answers.
//
//  WHY IT HAS A PASSWORD
//  ---------------------
//  This is an automation surface.  Anything that can reach the shell --
//  a .bat file, a managed app calling Host.Exec, a PE that got itself
//  run -- could otherwise drive the browser and the local AI engine
//  silently.  So the verbs are gated: the session starts locked, and
//  `webapi auth <password>` is the only way in.  The password is not a
//  string constant in the image (a `strings` pass would hand it over);
//  it is XOR-folded with an index-dependent key and rebuilt one byte at
//  a time in webapi_pw_ok(), which never keeps the plaintext around.
//  Three wrong tries wedge the interface for the rest of the boot.
//
//  Nothing in the tree prints the password, and `webapi selftest`
//  exists so the plumbing can be regression-tested without one: it
//  unlocks internally, runs the read-only verbs, and re-locks.
// =====================================================================
#define WEBAPI_PING      1
#define WEBAPI_GET_URL   2
#define WEBAPI_NAVIGATE  3
#define WEBAPI_GET_TEXT  4
#define WEBAPI_ASK       5
#define WEBAPI_CLICK     6
#define WEBAPI_LINKS     7
#define WEBAPI_STATUS    8
#define WM_NexOS_API    0x8000u

static const unsigned char WEBAPI_PW[24] = {
    0x17, 0x50, 0x06, 0x5E, 0x46, 0x0E, 0xC5, 0xB2,
    0xF1, 0xAA, 0xD3, 0xF4, 0xDD, 0xC2, 0x8F, 0xA1,
    0x9A, 0xB0, 0x88, 0xB6, 0xD6, 0x83, 0xC6, 0xB0
};
static bool g_webapi_unlocked = false;
static int  g_webapi_fails    = 0;

static bool webapi_pw_ok(const char* given){
    if(!given) return false;
    bool ok = true;
    for(int i = 0; i < 24; i++){
        char c = (char)(WEBAPI_PW[i] ^ (unsigned char)(0x5A + i * 7));
        if(given[i] != c) ok = false;      // no early exit: constant work
    }
    if(given[24] != 0) ok = false;
    return ok;
}

// The browser registers class "IEFrame"; that is how we find its slot.
static int webapi_find_browser(){
    int n = win32_window_count();
    for(int i = 0; i < n; i++){
        W32WinInfo wi;
        if(!win32_window_info(i, &wi)) continue;
        if(!strcmp_(wi.cls, "IEFrame")) return i;
    }
    return -1;
}

// Send one verb and print the answer.  Verbs that return text hand back
// a pointer into the PE's own statics -- same address space, so it is
// simply a const char* -- and the numeric verbs return a small integer.
static int webapi_call(int idx, unsigned verb, unsigned arg, bool as_text){
    int rc = win32_window_dispatch(idx, WM_NexOS_API, verb, arg);
    if(as_text){
        const char* s = (const char*)(uintptr_t)rc;
        if(!s){ term.write("(no answer)\n"); return 0; }
        term.write(s);
        if(s[0] && s[strlen_(s) - 1] != '\n') term.put_char('\n');
    }
    return rc;
}

static void webapi_usage(){
    term.write("webapi - agent bridge into the browser (authentication required)\n");
    term.write("  webapi auth <password>     unlock this session\n");
    term.write("  webapi lock                lock it again\n");
    term.write("  webapi ping                liveness probe -> 1E1E1E1E\n");
    term.write("  webapi status              page/clicks/focus/ai/url\n");
    term.write("  webapi url                 current address\n");
    term.write("  webapi links               numbered link list\n");
    term.write("  webapi text                rendered page text\n");
    term.write("  webapi nav <url>           navigate to <url>\n");
    term.write("  webapi click <n>           activate link <n> from 'links'\n");
    term.write("  webapi ask <question>      ask the local AI, show the answer\n");
    term.write("  webapi selftest            exercise the read-only verbs\n");
    term.write("Open the browser first (gui -> Browser icon, or 'gui browser').\n");
}

static void cmd_webapi(const char* args){
    char sub[16]; int si = 0;
    while(*args == ' ') args++;
    while(*args && *args != ' ' && si < 15) sub[si++] = *args++;
    sub[si] = 0;
    while(*args == ' ') args++;

    if(!sub[0] || !strcmp_(sub, "help")){ webapi_usage(); return; }

    if(!strcmp_(sub, "auth")){
        if(g_webapi_fails >= 3){
            term.write("webapi: locked out for this boot (too many failures)\n");
            return;
        }
        if(webapi_pw_ok(args)){
            g_webapi_unlocked = true;
            g_webapi_fails = 0;
            term.write("webapi: unlocked\n");
            serial_puts("[webapi] unlocked\n");
        } else {
            g_webapi_fails++;
            term.write("webapi: authentication failed\n");
            serial_puts("[webapi] auth failed\n");
        }
        return;
    }
    if(!strcmp_(sub, "lock")){
        g_webapi_unlocked = false;
        term.write("webapi: locked\n");
        return;
    }

    bool selftest = !strcmp_(sub, "selftest");
    if(!g_webapi_unlocked && !selftest){
        term.write("webapi: locked - run 'webapi auth <password>' first\n");
        serial_puts("[webapi] denied: locked\n");
        return;
    }

    int idx = webapi_find_browser();
    if(idx < 0){
        term.write("webapi: no browser window (open the Browser first)\n");
        serial_puts("[webapi] no IEFrame window\n");
        return;
    }

    if(selftest){
        // Exercises the transport and the read-only verbs without ever
        // materialising the password; mutating verbs still need auth.
        serial_puts("[webapi] selftest begin\n");
        int p = win32_window_dispatch(idx, WM_NexOS_API, WEBAPI_PING, 0);
        term.write("ping: ");
        term.write(p == 0x1E1E1E1E ? "OK\n" : "FAILED\n");
        serial_puts(p == 0x1E1E1E1E ? "[webapi] selftest ping OK\n"
                                    : "[webapi] selftest ping FAILED\n");
        term.write("status: "); webapi_call(idx, WEBAPI_STATUS, 0, true);
        term.write("links:\n"); webapi_call(idx, WEBAPI_LINKS, 0, true);
        term.write("text:\n");  webapi_call(idx, WEBAPI_GET_TEXT, 0, true);
        serial_puts("[webapi] selftest end\n");
        return;
    }

    if(!strcmp_(sub, "ping")){
        int p = win32_window_dispatch(idx, WM_NexOS_API, WEBAPI_PING, 0);
        term.write(p == 0x1E1E1E1E ? "webapi: pong\n" : "webapi: no reply\n");
    } else if(!strcmp_(sub, "status")){
        webapi_call(idx, WEBAPI_STATUS, 0, true);
    } else if(!strcmp_(sub, "url")){
        webapi_call(idx, WEBAPI_GET_URL, 0, true);
    } else if(!strcmp_(sub, "links")){
        webapi_call(idx, WEBAPI_LINKS, 0, true);
    } else if(!strcmp_(sub, "text")){
        webapi_call(idx, WEBAPI_GET_TEXT, 0, true);
    } else if(!strcmp_(sub, "nav")){
        if(!*args){ term.write("Usage: webapi nav <url>\n"); return; }
        int r = win32_window_dispatch(idx, WM_NexOS_API, WEBAPI_NAVIGATE,
                                      (unsigned)(uintptr_t)args);
        term.write(r ? "webapi: navigated\n" : "webapi: navigate refused\n");
        win32_window_repaint(idx);
    } else if(!strcmp_(sub, "click")){
        int n = 0; const char* q = args;
        if(!*q){ term.write("Usage: webapi click <n>\n"); return; }
        while(*q >= '0' && *q <= '9'){ n = n * 10 + (*q - '0'); q++; }
        int r = win32_window_dispatch(idx, WM_NexOS_API, WEBAPI_CLICK, (unsigned)n);
        term.write(r ? "webapi: clicked\n" : "webapi: no such link\n");
        win32_window_repaint(idx);
    } else if(!strcmp_(sub, "ask")){
        if(!*args){ term.write("Usage: webapi ask <question>\n"); return; }
        term.write("Thinking...\n");
        webapi_call(idx, WEBAPI_ASK, (unsigned)(uintptr_t)args, true);
        win32_window_repaint(idx);
    } else {
        webapi_usage();
    }
}

// =====================================================================
//  Network commands
// =====================================================================
static void cmd_ping(const char* args){
    if(!g_net_initialized){
        term.write("Network not initialized. Use 'netstart' to initialize.\n");
        return;
    }
    if(!args || !*args){
        term.write("Usage: ping <hostname|IP>  [attempts]\n");
        return;
    }
    const char* host = args;
    int attempts = 3;
    /* Take the first whitespace-delimited token as the host and drop any
       trailing control/line chars, so the IP-vs-DNS decision in net_ping
       sees a clean host string. */
    char h[200]; int hi = 0;
    while (args[hi] && args[hi] != ' ' && args[hi] != '\r' && args[hi] != '\n'
           && args[hi] != 0x10 && hi < 199) { h[hi] = args[hi]; hi++; }
    h[hi] = 0;
    if (hi > 0) host = h;
    {
        const char* a = args + hi;
        while(*a==' ') a++;
        if(*a){ attempts = 0; while(*a>='0'&&*a<='9'){ attempts=attempts*10+(*a-'0'); a++; } if(attempts<=0)attempts=1; }
    }
    term.write("ping "); term.write(host); term.write(" ...\n");
    serial_puts("[PING] command for "); serial_puts(host); serial_puts("\n");
    int r = net_ping(host, attempts);
    if(r == 1){
        term.write("  reply received\n");
        serial_puts("[PING] OK\n");
    } else if(r == 0){
        term.write("  no reply (timeout/unreachable)\n");
        serial_puts("[PING] TIMEOUT\n");
    } else {
        term.write("  ping unavailable (no NIC)\n");
    }
}

// Download <url> into a MKFS file.  Uses net_http_get (synchronous) and
// mkfs.create to persist the body.  Bodies up to FS_IOBUF_SIZE (8 KiB) fit;
// larger replies are truncated.  Requires network up and MKFS formatted.
static void cmd_download(const char* args){
    if(!g_net_initialized){ term.write("Network not initialized. Use 'netstart'.\n"); return; }
    if(!mkfs.mounted){ term.write("MKFS not mounted. Use 'mkfs' to format a data disk.\n"); return; }
    if(!args || !*args){ term.write("Usage: download <url> <file>\n"); return; }
    // parse <url> up to first space
    char url[256]; int ui = 0;
    while(args[ui] && args[ui] != ' ' && args[ui] != '\r' && args[ui] != '\n' && ui < 255){ url[ui]=args[ui]; ui++; }
    url[ui]=0;
    const char* f = args + ui;
    while(*f==' ') f++;
    if(!*f){ term.write("Usage: download <url> <file>\n"); return; }
    char file[FS_NAME_LEN]; int fi=0;
    while(f[fi] && f[fi] != ' ' && f[fi] != '\r' && f[fi] != '\n' && fi < FS_NAME_LEN-1){ file[fi]=f[fi]; fi++; }
    file[fi]=0;
    term.write("downloading "); term.write(url); term.write(" -> "); term.write(file); term.write(" ...\n");
    serial_puts("[DL] get "); serial_puts(url); serial_puts(" -> "); serial_puts(file); serial_puts("\n");
    g_iobuf[0] = 0;
    int n = net_http_get(url, (char*)g_iobuf, FS_IOBUF_SIZE);
    if(n <= 0){
        term.write("  no data received\n");
        serial_puts("[DL] FAIL (no data)\n");
        return;
    }
    int wr;
    wr = mkfs.create(file, g_iobuf, n);   // create() overwrites an existing file
    if(wr >= 0){
        term.write("  saved ");
        term.write(file);
        term.write(" (");
        // print byte count
        { char nn[16]; int np=0, v=n; if(v==0){term.write("0");} else { char tt[12]; int tq=0; while(v){tt[tq++]='0'+v%10; v/=10;} while(tq) nn[np++]=tt[--tq]; nn[np]=0; term.write(nn);} }
        term.write(" bytes)\n");
        serial_puts("[DL] OK ");
        serial_puts(file);
        serial_puts(" (");
        serial_puts_dec(n);   // serial decimal helper defined below
        serial_puts(")\n");
        // fetch + print first line so the user sees what came back
        { int k=0; while(g_iobuf[k] && k<n && k<240){ term.put_char((char)g_iobuf[k]); k++; } term.write("\n"); }
    } else {
        term.write("  write failed\n");
        serial_puts("[DL] FAIL (write)\n");
    }
}

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

// Unified `net` command: up / info / wifi / time / http.  Parses the first
// token as a subcommand and dispatches to the network stack in net.cpp.
static void cmd_net(const char* args){
    const char* a = args ? args : "";
    while (*a == ' ' || *a == '\t') a++;
    char sub[32]; int si = 0;
    while (*a && *a != ' ' && si < 31) sub[si++] = *a++;
    sub[si] = 0;
    const char* rest = a; while (*rest == ' ') rest++;
    char buf[2048];
    if (!strcmp_(sub, "") || !strcmp_(sub, "up") || !strcmp_(sub, "start")){
        if (!g_net_initialized){ int r = net_init(); if (r == 0) g_net_initialized = true; }
        int n = net_status(buf, sizeof(buf));
        term.write(n > 0 ? buf : "Network status unavailable.\n");
    } else if (!strcmp_(sub, "info") || !strcmp_(sub, "status")){
        int n = net_status(buf, sizeof(buf));
        term.write(n > 0 ? buf : "Network status unavailable.\n");
    } else if (!strcmp_(sub, "wifi")){
        char wsub[32]; int wi = 0;
        while (*rest && *rest != ' ' && wi < 31) wsub[wi++] = *rest++;
        wsub[wi] = 0;
        const char* wr = rest; while (*wr == ' ') wr++;
        if (!strcmp_(wsub, "scan")){ net_wifi_scan(buf, sizeof(buf)); term.write(buf); }
        else if (!strcmp_(wsub, "connect")){ net_wifi_connect(wr, buf, sizeof(buf)); term.write(buf); }
        else if (!strcmp_(wsub, "disconnect")){ net_wifi_disconnect(buf, sizeof(buf)); term.write(buf); }
        else if (!strcmp_(wsub, "status")){ net_wifi_status(buf, sizeof(buf)); term.write(buf); }
        else term.write("Usage: net wifi <scan|connect <ssid> [pass]|disconnect|status>\n");
    } else if (!strcmp_(sub, "time")){
        net_time(buf, sizeof(buf)); term.write(buf);
    } else if (!strcmp_(sub, "http")){
        int got = net_http_get(rest, buf, (int)sizeof(buf) - 1);
        if (got > 0) term.write(buf); else term.write("HTTP GET failed (no network or unreachable).\n");
    } else {
        term.write("Usage: net <up|info|wifi scan|wifi connect <ssid> [pass]|wifi status|time|http <url>>\n");
    }
}

// Set the guest's IPv4 address at runtime (host order, see net.cpp IPV4()).
// Used to give two distnet peers distinct addresses on a shared L2 link.
static void cmd_setip(const char* args){
    const char* p = args ? args : "";
    while(*p==' '||*p=='\t') p++;
    int parts[4]; int n=0; int cur=0; bool ok=true;
    for(int i=0;p[i];i++){
        char c=p[i];
        if(c=='.'){ if(n>=4){ok=false;break;} parts[n++]=cur; cur=0; }
        else if(c>='0'&&c<='9') cur=cur*10+(c-'0');
        else { ok=false; break; }
    }
    if(ok && n==3){ parts[n++]=cur; } else ok=false;
    if(!ok || n!=4){ term.write("setip: bad IP, expect a.b.c.d\n"); return; }
    uint32_t ip = ((uint32_t)parts[0]<<24)|((uint32_t)parts[1]<<16)|
                  ((uint32_t)parts[2]<<8) |(uint32_t)parts[3];
    net_set_ip(ip);
    char buf[40]; int bi=0;
    for(int o=0;o<4;o++){
        int v=parts[o]; char t[8]; int tn=0;
        if(v==0) t[tn++]='0'; else { while(v){ t[tn++]='0'+(v%10); v/=10; } }
        for(int j=tn-1;j>=0;j--) buf[bi++]=t[j];
        if(o<3) buf[bi++]='.';
    }
    buf[bi]=0;
    term.write("IP set to "); term.write(buf); term.write("\n");
}

// =====================================================================
//  GUI mode - enter graphical desktop environment
// =====================================================================
static void clr_ensure_init();        // defined below; the GUI needs the CLR live
static void register_gui_callbacks(void);  // defined below; fills gui.cpp's g_cb

static void seed_desktop_shortcuts(void);

static void cmd_gui(const char* args){
    if(!g_vbe_active){
        term.set_color(make_color(RED, BLACK));
        term.write("GUI not available (VBE graphics mode not set).\n");
        term.set_color(make_color(CYAN, BLACK));
        term.write("  VBE requires BIOS boot with graphics mode support.\n");
        term.set_color(make_color(WHITE, BLACK));
        return;
    }
    // Bring up the GUI subsystem on demand (allocates backbuffer, etc.)
    if(gui_init() != 0){
        term.set_color(make_color(RED, BLACK));
        term.write("GUI initialization failed.\n");
        term.set_color(make_color(WHITE, BLACK));
        return;
    }

    // Fill gui.cpp's g_cb machine-state callback table.  The 64-bit kernel
    // does this in its boot path; the 32-bit path (which is the one that
    // actually runs MiniCLR/mforms) previously never did, leaving every
    // g_cb.* pointer NULL.  mforms_boot() copies those into g_h, so the
    // managed shell's Host.MemTotalKb()/PagesUsed()/... would call through
    // a NULL pointer and #PF.  Register them before gui_enter().
    register_gui_callbacks();

    // The desktop can EXECUTE real Windows PE images now (double-click a
    // .exe in the File Explorer, or the Browser icon -> chrome.exe), so the
    // Win32 subsystem must be live before we hand over.  It used to be
    // initialised only by `winapp`/`reg`/`winver`/`winenv`, so a
    // double-click in a fresh session found no PE file reader and failed
    // with "file not found".  Idempotent.
    win32_ensure_init();

    // Seed the Desktop folder with .lnk shortcut files (idempotent).
    seed_desktop_shortcuts();

    term.set_color(make_color(CYAN, BLACK));
    term.write("Entering Win11 Desktop GUI mode...\n");
    serial_puts("[K32] Entering Win11 GUI mode\n");

    // The desktop and every window is now painted by the managed (C#)
    // NexOS.Forms shell, so the CLR must be live before we hand over.
    clr_ensure_init();

    // Optional: `gui <app>` opens straight into an app after GUI starts,
    // e.g. `gui calc`, `gui files`, `gui about` (the managed C# apps) or
    // `gui browser`.  Unknown names just land on the desktop.
    if (args) {
        while (*args == ' ') args++;
        if (args[0]) {
            int aid = gui_app_id_by_name(args);
            if (aid >= 0) gui_set_startup_app(aid);
        }
    }

    gui_enter();

    term.set_color(make_color(WHITE, BLACK));
}

// Disable auto-launch of the GUI on subsequent boots (debug / headless).
// Takes effect after the next reboot; the currently-running session is
// unaffected.  Re-enable by editing g_auto_gui in kernel.cpp (or reboot
// into text mode and never re-set it).
static void cmd_nogui(const char* args){
    (void)args;
    g_auto_gui = 0;
    term.set_color(make_color(CYAN, BLACK));
    term.write("Auto-GUI disabled. The desktop will not start automatically\n");
    term.write("on the next boot (this session keeps its current mode).\n");
    term.set_color(make_color(WHITE, BLACK));
}

// =====================================================================
//  Switch to 64-bit long-mode kernel
//  Reads kernel64.bin from disk (LBA 2048) into memory at 0x100000,
//  then calls switch_to_64bit() (defined in .attic64/switch32to64.asm,
//  linked into this 32-bit kernel) which transitions the CPU to long
//  mode and jumps to the 64-bit kernel entry at 0x100000.
// =====================================================================
//  The image may NOT be read straight into 0x100000: this 32-bit kernel's
//  own .bss lives at 0x120000..~0x20B460, so a direct load would overwrite
//  the ATA driver's globals half-way through the read loop and every sector
//  after the 256th would land as garbage (the symptom was a #GP at the
//  64-bit entry's `mov ds,0x10`, because the GDT sitting near the end of the
//  image was never written correctly).  Stage the image on the heap first
//  and let switch_to_64bit() blit it down once we are in pure asm and the
//  32-bit kernel state is expendable.
extern "C" void switch_to_64bit(uint32_t stage_phys);

#define KERNEL64_LBA_DISK   2048
#define KERNEL64_ADDR       0x100000
// 1320*512 = 660 KiB.  Raised from 1280 because net.cpp grew (POST client +
// OpenAI-compatible remote agent + CORS + /agent web UI): kernel64.bin is now
// ~717584 bytes (vector font rasterizer added).  Raised again to 1461 because
// the GUI ghosting fix + mouse-move refactor pushed kernel64.bin to ~747744
// bytes; SFS_LBA (Makefile) was moved 3508 -> 3520 to keep the gap large
// enough.  The rounded-rect clipping pipeline (Graphics::push_round_clip /
// fill_rect / blend_rect honour the clip mask) pushed it to ~754912 bytes,
// so SFS_LBA moved 3520 -> 3536 (gap = (3536-2048)*512 = 762368 and
// KERNEL64_SECTORS=1475 => 755200 bytes still fits with margin).
#define KERNEL64_SECTORS    1500    // raised for ps/kill

// Load kernel64.bin from the disk into a staging buffer and jump to long
// mode.  Shared by `switch` and `ask64`; never returns on success.
// EFI handoff struct written by BOOTX64.EFI at physical 0x7000.  Must
// match struct k64_handoff in uefi/bootuefi.c.
struct __attribute__((packed)) k64_handoff {
    uint32_t magic;   // 0x4B36344E ("K64N")
    uint32_t phys;    // physical address of preloaded 64-bit kernel
    uint32_t size;    // size in bytes
};

static void do_switch64(void){
    // The 32-bit kernel's own ATA driver reads garbage (all zeros) under
    // UEFI, so BOOTX64.EFI preloads the 64-bit image and leaves a handoff
    // struct at 0x7000.  Prefer that; fall back to the disk read only when
    // booting via the legacy BIOS path (no handoff present).
    volatile struct k64_handoff* h = (volatile struct k64_handoff*)0x7000;
    int use_handoff = (h->magic == 0x4B36344EUL);

    void* stage = nullptr;
    uint32_t stage_phys = 0;

    if (use_handoff) {
        stage_phys = h->phys;
        stage = (void*)(uintptr_t)stage_phys;
        serial_puts("[K32] using EFI-preloaded K64 @");
        serial_hex(stage_phys);
        serial_puts(" size=");
        serial_hex(h->size);
        serial_puts("\n");
    } else {
        // Staging buffer: heap lives at 3-19 MiB, well clear of both the
        // destination window (1-1.5 MiB) and our own .bss.
        stage = kmalloc(KERNEL64_SECTORS * 512);
        if (!stage) {
            term.set_color(make_color(RED, BLACK));
            term.write("ERROR: cannot allocate 512 KiB staging buffer.\n");
            serial_puts("[K32] ERROR: kmalloc(512K) failed\n");
            term.set_color(make_color(WHITE, BLACK));
            return;
        }
        stage_phys = (uint32_t)(uintptr_t)stage;
        const uint32_t stage_end = stage_phys + KERNEL64_SECTORS * 512;
        // The staged image must not sit on live kernel globals (.bss) or on the
        // boot stacks / long-mode page tables (.lmboot).  This is a range test:
        // .lmboot is at 24 MiB, ABOVE the heap kmalloc draws from, so the old
        // "stage_phys < __lmboot_end" form rejected every valid buffer and
        // silently disabled the whole 64-bit transition.
        if (ranges_overlap(stage_phys, stage_end,
                           (uint32_t)(uintptr_t)__bss_start,
                           (uint32_t)(uintptr_t)__bss_end) ||
            ranges_overlap(stage_phys, stage_end,
                           (uint32_t)(uintptr_t)__lmboot_start,
                           (uint32_t)(uintptr_t)__lmboot_end)) {
            term.set_color(make_color(RED, BLACK));
            term.write("ERROR: staging buffer overlaps .bss/.lmboot -- raise HEAP_START.\n");
            serial_puts("[K32] ERROR: stage ");
            serial_hex(stage_phys);
            serial_puts("..");
            serial_hex(stage_end);
            serial_puts(" hits .bss/.lmboot\n");
            term.set_color(make_color(WHITE, BLACK));
            kfree(stage);
            return;
        }
        if (stage_phys < 0x00200000u || stage_phys + KERNEL64_SECTORS * 512 > 0x02000000u) {
            // Must be outside the destination window and inside the identity map.
            term.set_color(make_color(RED, BLACK));
            term.write("ERROR: staging buffer at a bad address.\n");
            serial_puts("[K32] ERROR: bad stage addr\n");
            term.set_color(make_color(WHITE, BLACK));
            kfree(stage);
            return;
        }

        serial_puts("[K32] staging at ");
        serial_hex(stage_phys);
        serial_puts(", sectors=");
        serial_hex(KERNEL64_SECTORS);
        serial_puts("\n");

        uint16_t* dst = (uint16_t*)stage;
        for(int i=0; i<KERNEL64_SECTORS; i++){
            ata_read_sector(KERNEL64_LBA_DISK + i, dst);
            dst += 256;  // 512 bytes = 256 words
        }
    }

    serial_puts("[K32] stage[0..3]=");
    serial_hex(*(volatile uint32_t*)stage);
    serial_puts(" (expect 03F8BA66 = '66 BA F8 03' entry64)\n");

    serial_puts("[K32] K64 image staged; entering long mode\n");

    switch_to_64bit(stage_phys);

    // Should never reach here
    term.set_color(make_color(RED, BLACK));
    term.write("ERROR: switch_to_64bit() returned!\n");
    serial_puts("[K32] ERROR: switch_to_64bit returned!\n");
    term.set_color(make_color(WHITE, BLACK));
}

static void cmd_switch64(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("\nSwitching to 64-bit kernel...\n");
    serial_puts("[K32] Switching to 64-bit kernel...\n");

    term.write("Loading kernel64.bin from LBA ");
    term.write_dec(KERNEL64_LBA_DISK);
    term.write("...\n");

    do_switch64();
}

// `ask64 <question>`: stage the question in shared memory for the 64-bit
// kernel's auto-answer backdoor (NEXQ magic at 0x5100, UTF-8 text at 0x5104),
// then enter long mode.  kernel64 loads the embedded GGUF and answers with
// the real transformer engine, streaming the reply to serial/terminal.
static void cmd_ask64(const char* args){
    if(!*args){
        term.write("Usage: ask64 <question>\n");
        term.write("  Answers with the built-in GGUF engine in 64-bit mode.\n");
        return;
    }
    volatile char* dst = (volatile char*)0x5104;
    int i = 0;
    while (args[i] && i < 120){ dst[i] = args[i]; i++; }
    dst[i] = 0;
    *(volatile uint32_t*)0x5100 = 0x5145584Eu;   // 'NEXQ'
    term.set_color(make_color(CYAN, BLACK));
    term.write("\nask64: switching to 64-bit for real GGUF inference...\n");
    serial_puts("[K32] ask64 staged; switching\n");
    term.write("Loading kernel64.bin from LBA ");
    term.write_dec(KERNEL64_LBA_DISK);
    term.write("...\n");
    do_switch64();
    term.set_color(make_color(WHITE, BLACK));
}

// `session` -- inspect or clear the persisted GUI session.  The session is
// saved automatically when the GUI exits and restored on the next `gui`.
static int gui_cb_session_load(const char* name, void* buf, int bufsize);
static int gui_cb_session_clear(const char* name);
static void cmd_session(const char* args){
    while (args && *args == ' ') args++;
    if (args && !strcmp_(args, "clear")) {
        int r = gui_cb_session_clear("session");
        term.set_color(make_color(GREEN, BLACK));
        term.write(r == 0 ? "Session cleared.\n" : "No session to clear.\n");
        term.set_color(make_color(WHITE, BLACK));
        return;
    }
    uint8_t buf[2 + 8 * 9];
    int rd = gui_cb_session_load("session", buf, sizeof(buf));
    term.set_color(make_color(CYAN, BLACK));
    if (rd < 3 || buf[0] != 'S') {
        term.write("No saved GUI session.\n");
    } else {
        int n = buf[1];
        term.write("Saved session: ");
        term.write_dec(n);
        term.write(" window(s). `gui` will reopen them.\n");
        term.write("Use `session clear` to discard.\n");
    }
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
    "lsfs", "catfs", "part", "mount", "lsfat", "fatinfo", "disk",
    "run", "runfs", "ai", "generate", "agent", "ask",
    "winapp", "start", "reg", "regedit", "winver", "winenv",
    "netinfo", "netstat", "netstart", "ping", "download", "dl",
    "shutdown", "reboot", "exit",
    "gui", "session",
    "whoami", "id", "users", "login", "logout", "su",
    "useradd", "deluser", "passwd", "chmod", "stat", "sudo"
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
        fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
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

// Run a Linux ELF32 image from the SFS volume (Wine-on-NexOS shim).
// `linux <file> [args...]` -- argv is forwarded to the guest.
static void cmd_linux(const char* args){
    if (!args[0]){ term.write("Usage: linux <file.elf> [args...]\n"); return; }
    while (*args == ' ') args++;

    // argv words must be NUL-terminated copies: the shell line buffer has
    // spaces (not NULs) between words, and the guest reads argv strings up
    // to their terminator.
    char words[33][64];
    int  nwords = 0;
    const char* p = args;
    while (*p && nwords < 32){
        while (*p == ' ') p++;
        if (!*p) break;
        int w = 0;
        while (*p && *p != ' ' && w < 63) words[nwords][w++] = *p++;
        words[nwords][w] = 0;
        nwords++;
    }
    if (nwords == 0){ term.write("Usage: linux <file.elf> [args...]\n"); return; }

    const char* av[33];
    int ac = nwords;
    for (int i = 0; i < nwords; i++) av[i] = words[i];

    term.write("Launching Linux ELF: "); term.write(av[0]); term.write("\n");
    linux_run(av[0], ac, av);
}

// ---------------------------------------------------------------------
//  MiniCLR:  run a managed C# app (.mex) from the SFS volume.
//  The image was produced by real Roslyn and flattened by
//  tools/mex_pack.py, so every metadata token is already an index.
// ---------------------------------------------------------------------
static bool g_clr_ready = false;

static void clr_ensure_init(){
    if (g_clr_ready) return;
    clr_init(
        [](const char* fn, unsigned char* buf, int bufsize) -> int {
            int r = -1;
            while (*fn == ' ') fn++;
            if (sfs.mounted)           r = sfs.read(fn, (uint8_t*)buf, bufsize);
            if (r < 0 && mkfs.mounted) r = mkfs.read(fn, (uint8_t*)buf, bufsize);
            return r;
        });
    g_clr_ready = true;
}

static void cmd_clr(const char* args){
    clr_ensure_init();

    while (*args == ' ') args++;
    if (!args[0]){
        term.write("Usage: clr <app.mex>\n");
        term.write("       runs a C# assembly compiled by Roslyn and packed by mex_pack.py\n");
        return;
    }

    char file[64]; int fi = 0;
    while (args[fi] && args[fi] != ' ' && fi < 63){ file[fi] = args[fi]; fi++; }
    file[fi] = 0;

    term.set_color(make_color(CYAN, BLACK));
    term.write("=== NexOS .NET (MiniCLR) ===\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
    term.write("Assembly: "); term.write(file); term.put_char('\n');

    int rc = clr_run(file);

    const char* rep = clr_last_report();
    if (rep && rep[0]) term.write(rep);

    if (rc != 0){
        term.set_color(make_color(RED, BLACK));
        term.write("[X] ");
        switch (rc){
            case -1: term.write("File not found in SFS/MKFS.\n"); break;
            case -2: term.write("Not a valid .mex image.\n"); break;
            case -3: term.write("Out of managed heap.\n"); break;
            case -4: term.write("Unbound internal call.\n"); break;
            case -5: term.write("Execution fault.\n"); break;
            default: term.write("CLR error.\n"); break;
        }
        term.set_color(make_color(LIGHT_GREY, BLACK));
    } else {
        term.set_color(make_color(GREEN, BLACK));
        term.write("[OK] managed execution finished.\n");
        term.set_color(make_color(LIGHT_GREY, BLACK));
    }
}

// Launch a standalone per-application .mex as the resident managed context.
// The .mex's Program::Main opens exactly one app window via Host.OpenApp,
// and the kernel's native GUI loop then paints and drives it.  This is how
// the 12 packaged app images (Calc.mex, Terminal.mex, ...) run standalone.
static void cmd_clrapp(const char* args){
    clr_ensure_init();

    while (*args == ' ') args++;
    if (!args[0]){
        term.write("Usage: clrapp <app.mex>\n");
        term.write("       loads the .mex as the resident GUI context and runs\n");
        term.write("       its Program::Main (opens that one managed app).\n");
        return;
    }

    char file[64]; int fi = 0;
    while (args[fi] && args[fi] != ' ' && fi < 63){ file[fi] = args[fi]; fi++; }
    file[fi] = 0;

    term.set_color(make_color(CYAN, BLACK));
    term.write("=== NexOS .NET app (MiniCLR) ===\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
    term.write("Assembly: "); term.write(file); term.put_char('\n');

    int rc = clr_run_resident(file);

    const char* rep = clr_last_report();
    if (rep && rep[0]) term.write(rep);

    if (rc != 0){
        term.set_color(make_color(RED, BLACK));
        term.write("[X] ");
        switch (rc){
            case -1: term.write("File not found in SFS/MKFS.\n"); break;
            case -2: term.write("Not a valid .mex image.\n"); break;
            case -3: term.write("Out of managed heap.\n"); break;
            case -4: term.write("Unbound internal call.\n"); break;
            case -5: term.write("Execution fault.\n"); break;
            default: term.write("CLR error.\n"); break;
        }
        term.set_color(make_color(LIGHT_GREY, BLACK));
    } else {
        term.set_color(make_color(GREEN, BLACK));
        term.write("[OK] app launched (resident).\n");
        term.set_color(make_color(LIGHT_GREY, BLACK));
    }
}

// Launch a ring-3 user-mode demo (Foundation 0): load the flat 'userdemo'
// blob from SFS into the PG_USER region and enter it via the syscall ABI.
// =====================================================================
//  Y/N permission prompt UI  (security doc v1.0, 3.3)
// ---------------------------------------------------------------------
//  This runs in ring 0 on the kernel stack, in the middle of the calling
//  process's syscall.  That is the whole point: the requesting app is
//  suspended mid-instruction and cannot draw over the dialog, cannot
//  synthesise a keystroke, and cannot dismiss it.  We poll the i8042
//  directly rather than going through any input queue an app could feed.
//
//  Three guards worth calling out:
//    * the keyboard buffer is DRAINED and input is ignored until the RTC
//      second ticks over, so a key the user was already holding down when
//      the prompt appeared cannot auto-confirm it (tapjacking guard);
//    * no answer within PERM_TIMEOUT_SEC resolves to DENY;
//    * no keyboard controller at all -> PERM_UI_NONE, which perm.cpp also
//      treats as DENY.  Silence is never consent.
// =====================================================================
#define PERM_TIMEOUT_SEC 15

// CMOS seconds, BCD-decoded, skipping the update-in-progress window.
static int rtc_seconds(){
    for (int guard = 0; guard < 100000; guard++){
        outb(0x70, 0x0A);
        if (!(inb(0x71) & 0x80)) break;      // wait out UIP
    }
    outb(0x70, 0x00);
    uint8_t s = inb(0x71);
    outb(0x70, 0x0B);
    if (inb(0x71) & 0x04) return s;          // already binary
    return (s & 0x0F) + ((s >> 4) & 0x0F) * 10;
}

static const char* perm_risk_word(int risk){
    switch (risk){
        case 0:  return "LOW";
        case 1:  return "MEDIUM";
        case 2:  return "HIGH";
        default: return "CRITICAL";
    }
}

// Amber warning panel over the desktop when the GUI owns the screen.
static void perm_draw_gui_panel(const PermRequest* req){
    int W = gui_get_width(), H = gui_get_height();
    if (W <= 0 || H <= 0) return;
    int pw = 560, ph = 200;
    int px = (W - pw) / 2, py = (H - ph) / 3;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    gui_fill_rect(px - 3, py - 3, pw + 6, ph + 6, 0x00C01010);   // red bezel
    gui_fill_rect(px, py, pw, ph, 0x00FFE9A8);                   // amber body
    gui_draw_text(px + 16, py + 14,  "NexOS SECURITY REQUEST");
    gui_draw_text(px + 16, py + 40,  req->app);
    gui_draw_text(px + 16, py + 60,  req->action);
    gui_draw_text(px + 16, py + 80,  req->category);
    gui_draw_text(px + 16, py + 100, req->resource);
    gui_draw_text(px + 16, py + 132, "[Y] allow once   [N] deny");
    gui_draw_text(px + 16, py + 152, "[A] always allow [D] always deny");
    gui_draw_text(px + 16, py + 172, "no answer = DENY");
}

static int perm_ui_console(const PermRequest* req, int* remember){
    *remember = 0;

    // No input device -> we cannot obtain consent, so we must not assume it.
    if (!g_hw.keyboard_present){
        serial_puts("[PERM] no keyboard controller - cannot ask, failing safe\n");
        return PERM_UI_NONE;
    }

    bool in_gui = gui_is_active() != 0;
    if (in_gui) perm_draw_gui_panel(req);

    // --- text console rendering (also the headless/serial-visible path) ---
    term.write("\n");
    term.set_color(make_color(WHITE, RED));
    term.write(" NexOS SECURITY REQUEST                        RISK: ");
    term.write(perm_risk_word(req->risk));
    term.write(" \n");
    term.set_color(make_color(YELLOW, BLACK));
    term.write("  Application : "); term.write(req->app);
    term.write("  (pid ");          term.write_dec((int)req->pid);
    term.write(", uid ");           term.write_dec((int)req->uid);
    term.write(")\n");
    term.write("  Wants to    : "); term.write(req->action); term.write("\n");
    term.write("  Category    : "); term.write(req->category); term.write("\n");
    term.write("  Resource    : "); term.write(req->resource); term.write("\n");
    term.set_color(make_color(WHITE, BLACK));
    term.write("  [Y] allow once   [N] deny   [A] always allow   [D] always deny\n");
    term.write("  No answer in "); term.write_dec(PERM_TIMEOUT_SEC);
    term.write("s = DENY\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
    term.render();

    // --- tapjacking guard: drain, then ignore input for one RTC tick ---
    int s0 = rtc_seconds();
    for (;;){
        uint8_t st = inb(0x64);
        if (st != 0xFF && (st & 0x01)) inb(0x60);     // discard
        if (rtc_seconds() != s0) break;
    }

    // --- blocking poll with a fail-safe deadline ---
    int last = rtc_seconds();
    int elapsed = 0;
    for (;;){
        int now = rtc_seconds();
        if (now != last){
            last = now;
            if (++elapsed >= PERM_TIMEOUT_SEC){
                serial_puts("[PERM] prompt timed out - defaulting to DENY\n");
                term.write("  (timed out - denied)\n");
                term.render();
                if (in_gui) gui_render();
                return PERM_DENY;
            }
        }
        uint8_t st = inb(0x64);
        if (st == 0xFF || !(st & 0x01)) continue;
        uint8_t data = inb(0x60);
        if (st & 0x20) continue;                       // mouse byte
        KbdEvent e = kbd.process(data);
        if (e.type != K_CHAR) continue;
        char c = e.ch;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);

        int answer;
        if      (c == 'y'){ answer = PERM_ALLOW; }
        else if (c == 'n'){ answer = PERM_DENY;  }
        else if (c == 'a'){ answer = PERM_ALLOW; *remember = 1; }
        else if (c == 'd'){ answer = PERM_DENY;  *remember = 1; }
        else continue;                                 // ignore everything else

        term.set_color(answer == PERM_ALLOW ? make_color(GREEN, BLACK)
                                            : make_color(RED, BLACK));
        term.write(answer == PERM_ALLOW ? "  -> ALLOWED" : "  -> DENIED");
        term.write(*remember ? " (remembered)\n" : "\n");
        term.set_color(make_color(LIGHT_GREY, BLACK));
        term.render();
        if (in_gui) gui_render();                      // repaint over the panel
        return answer;
    }
}

// Inspect / clear the remembered Y/N answers.
static void cmd_perm(const char* args){
    if (args[0] == 'r'){        // "perm reset"
        perm_reset();
        term.write("All remembered permission grants cleared.\n");
        return;
    }
    perm_dump();
    term.write("Remembered grants dumped to serial. Usage: perm [reset]\n");
}

static void cmd_user(const char* args){
    (void)args;
    serial_puts("[user] loading ring-3 demo 'userdemo'\n");
    uint8_t* dst = (uint8_t*)USER_BASE;
    int sz = sfs.read("userdemo", dst, (int)USER_SIZE);
    if (sz <= 0){
        serial_puts("[user] FAILED: userdemo not in SFS\n");
        term.write("userdemo not found in SFS\n");
        return;
    }
    serial_puts("[user] loaded "); serial_hex((uint32_t)sz);
    serial_puts(" bytes @0x"); serial_hex((uint32_t)USER_BASE); serial_puts("\n");

    Process* p = 0;
    if (proc_spawn("userdemo", 1000, "/apps/userdemo", &p) != 0){
        serial_puts("[user] FAILED: process table full\n");
        return;
    }
    p->entry = USER_BASE;
    uint32_t stack_top = USER_END - 16;   // top of user region

    Process* prev = g_current;
    g_current = p;                         // becomes ring 3 inside enter_user
    int rc = enter_user(USER_BASE, stack_top);
    g_current = prev;                      // restore kernel process

    serial_puts("[user] returned from ring-3 (rc=");
    serial_hex((uint32_t)rc);
    serial_puts(")\n");
    term.write("Ring-3 demo finished.\n");
}

// Inspect the VFS: mount state, the caller's sandbox root, and a couple of
// resolves.  Run as the kernel process, so every check takes the SYSTEM
// bypass - that contrast is the point of the demo.
static void cmd_vfs(const char* args){
    vfs_dump();
    if (!args[0]){
        term.write("VFS mounted on SFS. Usage: vfs <path>  (resolve+open as SYSTEM)\n");
        return;
    }
    char abs[192];
    if (vfs_resolve(args, abs, sizeof(abs)) != 0){
        term.write("vfs: cannot resolve path\n");
        serial_puts("[vfs] cannot resolve path\n");
        return;
    }
    term.write("resolved: ");   term.write(abs);   term.write("\n");
    serial_puts("[vfs] resolved: "); serial_puts(abs); serial_puts("\n");

    int fd = vfs_open(args, 0);
    if (fd < 0){
        term.write("vfs: open failed\n");
        serial_puts("[vfs] open failed\n");
        return;
    }
    char buf[128];
    int n = vfs_read(fd, buf, (int)sizeof(buf) - 1);
    if (n > 0){
        buf[n] = 0;
        term.write(buf);       if (buf[n-1] != '\n') term.write("\n");
        serial_puts("[vfs] content: ");
        serial_puts(buf);      if (buf[n-1] != '\n') serial_puts("\n");
    }
    vfs_close(fd);
}

static void run_command(const char* line){
    while(*line==' ') line++;
    if(*line==0) return;
    // Echo every dispatched command to the serial port so headless tests can
    // verify which commands actually reached the shell.
    serial_puts("[SHELL] $ "); serial_puts(line); serial_puts("\n");
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
    else if(!strcmp_(cmd,"ps"))                         cmd_ps();
    else if(!strcmp_(cmd,"kill"))                        cmd_kill(args);
    // SFS commands
    else if(!strcmp_(cmd,"lsfs"))  cmd_lsfs();
    else if(!strcmp_(cmd,"catfs")) cmd_catfs(args);
    // Partition commands
    else if(!strcmp_(cmd,"part"))  cmd_part();
    else if(!strcmp_(cmd,"mount")) cmd_mount(args);
    else if(!strcmp_(cmd,"lsfat")) cmd_lsfat();
    else if(!strcmp_(cmd,"fatinfo"))cmd_fatinfo();
    else if(!strcmp_(cmd,"disk"))  cmd_disk(args);
    // Script execution
    else if(!strcmp_(cmd,"run"))   cmd_run(args);
    else if(!strcmp_(cmd,"runfs")) cmd_runfs(args);
    // Win32 subsystem
    else if(!strcmp_(cmd,"winapp")||!strcmp_(cmd,"start")) cmd_winapp(args);
    else if(!strcmp_(cmd,"reg")||!strcmp_(cmd,"regedit"))  cmd_reg(args);
    else if(!strcmp_(cmd,"winver"))                        cmd_winver();
    else if(!strcmp_(cmd,"winenv")||!strcmp_(cmd,"set32")) cmd_winenv(args);
    // Linux binary-compat (Wine-on-NexOS): run an ELF32 image from SFS
    else if(!strcmp_(cmd,"linux"))                         cmd_linux(args);
    else if(!strcmp_(cmd,"clr")||!strcmp_(cmd,"dotnet"))   cmd_clr(args);
    else if(!strcmp_(cmd,"clrapp"))                          cmd_clrapp(args);
    // Foundation 0: ring-3 user-mode demo (proves isolation + syscall ABI)
    else if(!strcmp_(cmd,"user"))                          cmd_user(args);
    else if(!strcmp_(cmd,"vfs"))                           cmd_vfs(args);
    else if(!strcmp_(cmd,"perm"))                          cmd_perm(args);
    // AI engine commands
    else if(!strcmp_(cmd,"ai"))         cmd_ai(args);
    else if(!strcmp_(cmd,"kb"))         cmd_kb(args);
    else if(!strcmp_(cmd,"generate")||!strcmp_(cmd,"gen")) cmd_generate(args);
    else if(!strcmp_(cmd,"agent"))      cmd_agent(args);
    else if(!strcmp_(cmd,"model"))       cmd_model(args);
    else if(!strcmp_(cmd,"ask"))        cmd_ask(args);
    else if(!strcmp_(cmd,"plugin"))     cmd_plugin(args);
    else if(!strcmp_(cmd,"webapi"))     cmd_webapi(args);
    // Network commands
    else if(!strcmp_(cmd,"ping"))        cmd_ping(args);
    else if(!strcmp_(cmd,"download")||!strcmp_(cmd,"dl")) cmd_download(args);
    else if(!strcmp_(cmd,"netinfo")||!strcmp_(cmd,"netstat")) cmd_netinfo();
    else if(!strcmp_(cmd,"netstart"))                        cmd_netstart();
    else if(!strcmp_(cmd,"net"))                            cmd_net(args);
    else if(!strcmp_(cmd,"setip"))       cmd_setip(args);
    else if(!strcmp_(cmd,"distnet"))     cmd_distnet(args);
    // GUI
    else if(!strcmp_(cmd,"gui"))  cmd_gui(args);
    else if(!strcmp_(cmd,"nogui")) cmd_nogui(args);
    else if(!strcmp_(cmd,"serialecho")){
        if(!strcmp_(args,"off"))      g_term_serial = false;
        else if(!strcmp_(args,"on"))  g_term_serial = true;
        term.write("serialecho: ");
        term.write(g_term_serial ? "on\n" : "off\n");
    }
    else if(!strcmp_(cmd,"switch")||!strcmp_(cmd,"switch64")) cmd_switch64();
    else if(!strcmp_(cmd,"ask64")) cmd_ask64(args);
    // Memory management
    else if(!strcmp_(cmd,"meminfo"))  cmd_meminfo();
    else if(!strcmp_(cmd,"memtest"))  cmd_memtest();
    else if(!strcmp_(cmd,"pagetest")) cmd_pagetest();
    // User / permission / sudo system
    else if(!strcmp_(cmd,"whoami"))   cmd_whoami();
    else if(!strcmp_(cmd,"id"))       cmd_id();
    else if(!strcmp_(cmd,"users"))    cmd_users();
    else if(!strcmp_(cmd,"login"))    cmd_login(args);
    else if(!strcmp_(cmd,"logout"))   cmd_logout();
    else if(!strcmp_(cmd,"su"))       cmd_su(args);
    else if(!strcmp_(cmd,"useradd"))  cmd_useradd(args);
    else if(!strcmp_(cmd,"deluser"))  cmd_deluser(args);
    else if(!strcmp_(cmd,"passwd"))   cmd_passwd(args);
    else if(!strcmp_(cmd,"chmod"))    cmd_chmod(args);
    else if(!strcmp_(cmd,"stat"))     cmd_stat(args);
    else if(!strcmp_(cmd,"sudo"))     cmd_sudo(args);
    // GUI session persistence: `session` shows / `session clear` wipes the
    // saved running-app list that `gui` reopens on entry.
    else if(!strcmp_(cmd,"session"))  cmd_session(args);
    // Power management
    else if(!strcmp_(cmd,"shutdown")||!strcmp_(cmd,"exit")||!strcmp_(cmd,"poweroff")) cmd_shutdown();
    else if(!strcmp_(cmd,"reboot")||!strcmp_(cmd,"restart")) cmd_reboot();
    else { term.write("Unknown command: "); term.write(cmd);
           term.write("  (Type 'help' for available commands)\n"); }
}

// ---- SSH server hooks (called from net.cpp) ----
// Validate a username/password pair against the kernel user table.
// Returns 1 on success, 0 otherwise.
extern "C" int nexos_auth(const char* user, const char* pw){
    for(int i = 0; i < g_user_count; i++){
        if(!g_users[i].exists) continue;
        if(strcmp_(g_users[i].name, user) != 0) continue;
        char hash[17];
        hash_password(g_users[i].name, pw, hash);
        if(strcmp_(hash, g_users[i].hash) == 0){
            // establish the session identity for commands run over SSH
            g_login_idx = i;
            g_euid = g_users[i].uid;
            g_sudo_active = false;
            return 1;
        }
    }
    return 0;
}

// Run a command line through the normal shell dispatcher.  Used by the SSH
// server's channel-data / exec handlers so remote sessions share the same
// command surface as the local console.  The SSH layer is responsible for
// arming g_ssh_out_fn (via term_set_ssh_sink) so command output is forwarded
// over the encrypted channel; kernel_exec_line itself does not touch the sink.
extern "C" void kernel_exec_line(const char* line){
    run_command(line);
}

// SSH output sink setters (called from net.cpp).  When fn is non-null, every
// terminal character emitted by the shell is forwarded to the SSH channel.
extern "C" void term_set_ssh_sink(ssh_out_fn_t fn){ g_ssh_out_fn = fn; }
extern "C" void term_clear_ssh_sink(void){ g_ssh_out_fn = 0; }

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

    // In fbcon (graphics) mode the hardware text cursor is replaced by a
    // steady software cursor drawn by fb_console_render() -- the VGA block
    // cursor would otherwise blink on the emulated text layer.
    if (m_at_bottom) {
        set_cursor_pos(cur_row, m_cur_pos);
        if (!g_fb_console_mode) show_cursor();
        else                    hide_cursor();
    } else {
        hide_cursor();
    }

    // If in framebuffer console mode, render VGA text buffer to VBE framebuffer
    if(g_fb_console_mode) fb_console_render();
}

// =====================================================================
//  GUI Callback implementations
// =====================================================================

// Forward declarations for file system objects
extern uint16_t g_cwd;

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
    serial_puts("[K-MEM] Memory optimization requested\n");
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
    serial_puts("[K-MEM] Heap coalesced ");
    char buf[16]; int_to_str(coalesced, buf); serial_puts(buf);
    serial_puts(" blocks\n");
}

// ---- File listing callback ----
// fs_type: 0=MKFS, 1=SFS, 2=FAT32
// ---- Desktop directory (fs==3) ----------------------------------
// The desktop shortcuts live as .lnk files inside a "Desktop" folder on
// the writable MKFS volume.  fs==3 addresses that folder so the managed
// shell can enumerate / open / rename / delete shortcuts like any file.
static int desktop_dir_index(void) {
    if (!mkfs.mounted) return -1;
    for (int s = 0; s < MKFS_TABLE_SECT; s++) {
        fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
            FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
            if (fe->name[0] != 0 && fe->parent == FS_ROOT_PARENT &&
                strcmp_(fe->name, "Desktop") == 0)
                return s * FS_ENTRY_PER_SEC + e;
        }
    }
    return -1;
}

// Seed the Desktop folder with one .lnk per built-in shortcut.  A .lnk
// file's body is a single decimal digit: the managed Kind it launches.
static void seed_desktop_shortcuts(void) {
    if (!mkfs.mounted) return;
    uint16_t saved = g_cwd;
    g_cwd = FS_ROOT_PARENT;
    if (mkfs.find("Desktop") < 0) mkfs.mkdir("Desktop");
    int di = mkfs.find("Desktop");
    if (di < 0) { g_cwd = saved; return; }
    g_cwd = (uint16_t)di;
    struct { const char* name; const char* kind; } seed[] = {
        {"This PC.lnk", "1"}, {"Terminal.lnk", "3"}, {"Calculator.lnk", "4"},
        {"Task Mgr.lnk", "2"}, {"Settings.lnk", "0"}, {"Optimizer.lnk", "6"},
        {"Notepad.lnk", "7"}, {"About.lnk", "5"}, {"Browser.lnk", "8"},
        {"AI Setup.lnk", "9"}, {"AI Agent.lnk", "10"}, {"Demo.lnk", "11"},
    };
    for (unsigned i = 0; i < sizeof(seed)/sizeof(seed[0]); i++) {
        if (mkfs.find(seed[i].name) < 0)
            mkfs.create(seed[i].name, seed[i].kind, 1);
    }
    g_cwd = saved;
}

static int gui_cb_list_files(int fs_type, char* outbuf, int bufsize) {
    int count = 0;
    int pos = 0;
    outbuf[0] = 0;

    if (fs_type == 0) {
        // MKFS
        if (!mkfs.mounted) return 0;
        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
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
            ata_read_sector(sfs.dir_lba(s), (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0) {
                    int nlen = strlen_(fe->name);
                    if (pos + nlen + 12 < bufsize) {
                        memcpy_(outbuf + pos, fe->name, nlen); pos += nlen;
                        // Add size info
                        outbuf[pos++] = ' ';
                        outbuf[pos++] = '(';
                        char sz[12]; uint_to_str(fe->size, sz);
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
    } else if (fs_type == 3) {
        // Desktop folder (MKFS subdir): list its .lnk shortcut files.
        int di = desktop_dir_index();
        if (di >= 0) {
            for (int s = 0; s < MKFS_TABLE_SECT; s++) {
                fs_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
                for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                    FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                    if (fe->name[0] != 0 && fe->parent == (uint16_t)di) {
                        int nlen = strlen_(fe->name);
                        if (pos + nlen + 2 < bufsize) {
                            memcpy_(outbuf + pos, fe->name, nlen); pos += nlen;
                            outbuf[pos++] = '\n';
                            count++;
                        }
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
                // Extract 8.3 name
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

// ---- File read callback ----
// fs_type: 0=MKFS, 1=SFS, 2=FAT32
// Returns bytes read, or -1 on error
static int gui_cb_read_file(int fs_type, const char* name, uint8_t* buf, int bufsize) {
    // Strip leading spaces from prefix like "    filename"
    while (*name == ' ') name++;

    if (fs_type == 0) {
        // MKFS
        if (!mkfs.mounted) return -1;
        return mkfs.read(name, buf, bufsize);
    } else if (fs_type == 1) {
        // SFS
        if (!sfs.mounted) return -1;
        return sfs.read(name, buf, bufsize);
    } else if (fs_type == 3) {
        // Desktop folder (MKFS subdir): read the .lnk body (the Kind).
        if (!mkfs.mounted) return -1;
        int di = desktop_dir_index();
        if (di < 0) return -1;
        uint16_t saved = g_cwd; g_cwd = (uint16_t)di;
        int n = mkfs.read(name, buf, bufsize);
        g_cwd = saved;
        return n;
    } else if (fs_type == 2) {
        // FAT32 - need to read file by cluster chain
        if (!fat32.mounted) return -1;
        // Parse FAT32 directory to find the file and get its starting cluster
        // For simplicity, use the same approach as the shell
        // FAT32 read is complex; return -1 for now if not easily readable
        return -1;
    }
    return -1;
}

// ---- File write callback (MKFS data disk) ----
// fs_type: 0=MKFS only for now.  Returns bytes written, or -1 on error.
static int gui_cb_write_file(int fs_type, const char* name, const uint8_t* buf, int size) {
    while (*name == ' ') name++;
    if (fs_type != 0) return -1;          // MKFS only
    if (!mkfs.mounted) return -1;
    return mkfs.create(name, buf, size);
}

// ---- Session persistence callbacks (MKFS data disk) ----
// Store the running-app list so a reboot can reopen the GUI session.
static int gui_cb_session_save(const char* name, const void* data, int size) {
    if (!mkfs.mounted) return -1;
    return mkfs.create(name, (const uint8_t*)data, size);
}
static int gui_cb_session_load(const char* name, void* buf, int bufsize) {
    if (!mkfs.mounted) return -1;
    return mkfs.read(name, (uint8_t*)buf, bufsize);
}
static int gui_cb_session_clear(const char* name) {
    if (!mkfs.mounted) return -1;
    if (mkfs.find(name) >= 0) return mkfs.remove(name);
    return 0;
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
    return "NexOS v2.0 (Win11 Desktop)";
}

// ---- 64-bit detection ----
// Returns whether we are CURRENTLY running in 64-bit long mode.
// This 32-bit kernel always runs in 32-bit protected mode, so return false.
// (g_hw.has_long_mode indicates CPU *capability*, not current mode.)
// 32-bit kernel only: the long-mode kernel was retired (see .attic64/).
// and its own gui_cb_is_64bit() returns true.
static bool gui_cb_is_64bit(void) {
    return false;  // 32-bit protected mode
}

// Returns whether the CPU *supports* 64-bit (capability, not current mode)
static int gui_cb_cpu_64bit_capable(void) {
    return g_hw.has_long_mode ? 1 : 0;
}

// ---- File-mutation callbacks (context-menu: new folder / delete / rename) ----
// Only the in-RAM MKFS volume (fs==0) is writable from the GUI; the SFS
// boot image and FAT32 are read-only, so they return -1 here.
static int gui_cb_mkdir(int fs, const char* name) {
    while (name && *name == ' ') name++;
    if (!name || !name[0]) return -1;
    if (fs == 3) {
        if (!mkfs.mounted) return -1;
        int di = desktop_dir_index();
        if (di < 0) return -1;
        uint16_t saved = g_cwd; g_cwd = (uint16_t)di;
        int r = mkfs.mkdir(name);
        g_cwd = saved;
        return r;
    }
    if (fs != 0 || !mkfs.mounted) return -1;
    return mkfs.mkdir(name);
}
static int gui_cb_remove(int fs, const char* name) {
    while (name && *name == ' ') name++;
    if (!name || !name[0]) return -1;
    int r = -1;
    if (fs == 3) {
        if (!mkfs.mounted) return -1;
        int di = desktop_dir_index();
        if (di < 0) return -1;
        uint16_t saved = g_cwd; g_cwd = (uint16_t)di;
        r = mkfs.remove(name);
        g_cwd = saved;
    } else if (fs == 0 && mkfs.mounted) {
        r = mkfs.remove(name);
    } else {
        return -1;
    }
    return r;
}
static int gui_cb_rename(int fs, const char* old_name, const char* new_name) {
    while (old_name && *old_name == ' ') old_name++;
    while (new_name && *new_name == ' ') new_name++;
    if (!old_name || !old_name[0] || !new_name || !new_name[0]) return -1;
    if (fs == 3) {
        if (!mkfs.mounted) return -1;
        int di = desktop_dir_index();
        if (di < 0) return -1;
        uint16_t saved = g_cwd; g_cwd = (uint16_t)di;
        int r = -1;
        if (mkfs.copy(old_name, new_name) >= 0) r = mkfs.remove(old_name);
        g_cwd = saved;
        return r;
    }
    if (fs != 0 || !mkfs.mounted) return -1;
    if (mkfs.copy(old_name, new_name) < 0) return -1;
    return mkfs.remove(old_name);
}

// ---- Browser callbacks ----
static int gui_cb_browser_navigate(const char* url) {
    if (!g_net_initialized) return -1;
    return browser_navigate(url);
}

static int gui_cb_browser_status(void) {
    if (!g_net_initialized) return 0;
    return browser_status();
}

static int gui_cb_browser_get_page(char* buf, int bufsize) {
    if (!g_net_initialized) return -1;
    return browser_get_page(buf, bufsize);
}

static void gui_cb_browser_reset(void) {
    if (!g_net_initialized) return;
    browser_reset();
}

// Synchronous HTTP GET for the managed Browser control.  Spins the kernel
// network state machine until the response is in (or errors out) and
// returns the body via a static buffer the icall copies into a string.
static const char* gui_cb_http_get(const char* url) {
    static char buf[16384];
    buf[0] = 0;
    if (g_net_initialized) net_http_get(url, buf, (int)sizeof(buf));
    return buf;
}

// ---- Terminal command execution callback ----
// Captures terminal output by temporarily redirecting term.write to a buffer
// (Variables declared earlier near Terminal class)

// Wrapper to capture command output
static void gui_cb_exec_command(const char* cmd, char* output, int outsize) {
    g_exec_output_len = 0;
    g_exec_output[0] = 0;
    g_capturing = true;

    // Execute the command using the existing shell command dispatcher
    // We redirect output by using a capture flag in the terminal
    // For simplicity, we save the current terminal state, run the command,
    // then extract the output from the terminal's current line buffer
    run_command(cmd);

    g_capturing = false;

    // Copy captured output
    int len = g_exec_output_len;
    if (len > outsize - 1) len = outsize - 1;
    memcpy_(output, g_exec_output, len);
    output[len] = 0;
}

// ---- Hardware info callbacks for GUI ----
static const char* gui_cb_cpu_vendor(void)      { return g_hw.cpu_vendor; }
static const char* gui_cb_disk_model(void)      { return g_hw.disk_model; }
static uint32_t    gui_cb_disk_size_mb(void)    { return g_hw.disk_size_mb; }
static int         gui_cb_nic_present(void)     { return g_hw.nic_present ? 1 : 0; }
static int         gui_cb_mouse_present(void)   { return g_hw.mouse_present ? 1 : 0; }
static int         gui_cb_keyboard_present(void){ return g_hw.keyboard_present ? 1 : 0; }
static uint32_t    gui_cb_pci_count(void)       { return g_hw.pci_devices_found; }
static int         gui_cb_bga_available(void)   { return gui_bga_available(); }
static int         gui_cb_vbe_mode_set(void)    { return gui_vbe_mode_set_by_bios(); }

// ---- Register all callbacks ----
// =====================================================================
//  Graphical sign-in bridge
// ---------------------------------------------------------------------
//  The lock screen is managed code (csharp/apps/Shell/Login.cs), but the
//  account database and the password hashes stay in the kernel.  These
//  four callbacks are the entire surface the managed side can reach:
//  it can ask "is anybody signed in?", enumerate the account names for
//  the user picker, and submit one credential pair at a time.
// =====================================================================
static int gui_cb_login_check(const char* user, const char* pass) {
    if (!user || !pass) return -1;
    for (int i = 0; i < g_user_count; i++) {
        if (!g_users[i].exists) continue;
        if (strcmp_(g_users[i].name, user) != 0) continue;
        char hash[17];
        hash_password(g_users[i].name, pass, hash);
        if (strcmp_(hash, g_users[i].hash) != 0) {
            serial_puts("[K32-LOGIN] reject user="); serial_puts(user); serial_puts("\n");
            return -1;
        }
        // Commit the session exactly like the text login_prompt() does.
        g_login_idx   = i;
        g_euid        = g_users[i].uid;
        g_sudo_active = false;
        serial_puts("[K32-LOGIN] OK user="); serial_puts(g_users[i].name); serial_puts("\n");
        return (int)g_users[i].uid;
    }
    serial_puts("[K32-LOGIN] no such user\n");
    return -1;
}

static int gui_cb_login_uid(void) {
    if (g_login_idx < 0) return -1;
    return (int)g_users[g_login_idx].uid;
}

static int gui_cb_user_count(void) {
    int n = 0;
    for (int i = 0; i < g_user_count; i++) if (g_users[i].exists) n++;
    return n;
}

static const char* gui_cb_user_name(int idx) {
    int n = 0;
    for (int i = 0; i < g_user_count; i++) {
        if (!g_users[i].exists) continue;
        if (n == idx) return g_users[i].name;
        n++;
    }
    return "";
}

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
    cb.read_file           = gui_cb_read_file;
    cb.write_file          = gui_cb_write_file;
    cb.get_time            = gui_cb_get_time;
    cb.mkdir               = gui_cb_mkdir;
    cb.remove              = gui_cb_remove;
    cb.rename              = gui_cb_rename;
    cb.get_os_name         = gui_cb_os_name;
    cb.is_64bit            = gui_cb_is_64bit;
    cb.browser_navigate    = gui_cb_browser_navigate;
    cb.browser_status      = gui_cb_browser_status;
    cb.browser_get_page    = gui_cb_browser_get_page;
    cb.browser_reset       = gui_cb_browser_reset;
    cb.exec_command        = gui_cb_exec_command;
    cb.shutdown            = cmd_shutdown;
    cb.reboot              = cmd_reboot;
    // Hardware info callbacks
    cb.get_cpu_vendor      = gui_cb_cpu_vendor;
    cb.get_disk_model      = gui_cb_disk_model;
    cb.get_disk_size_mb    = gui_cb_disk_size_mb;
    cb.get_nic_present     = gui_cb_nic_present;
    cb.get_mouse_present   = gui_cb_mouse_present;
    cb.get_keyboard_present= gui_cb_keyboard_present;
    cb.get_pci_count       = gui_cb_pci_count;
    cb.get_bga_available   = gui_cb_bga_available;
    cb.get_vbe_mode_set    = gui_cb_vbe_mode_set;
    cb.get_cpu_64bit_capable = gui_cb_cpu_64bit_capable;
    cb.http_get             = gui_cb_http_get;
    cb.session_save         = gui_cb_session_save;
    cb.session_load         = gui_cb_session_load;
    cb.session_clear        = gui_cb_session_clear;
    // Graphical lock screen
    cb.login_check          = gui_cb_login_check;
    cb.login_uid            = gui_cb_login_uid;
    cb.user_count           = gui_cb_user_count;
    cb.user_name            = gui_cb_user_name;
    gui_set_callbacks(&cb);
}

// =====================================================================
//  Minimal IDT + 8259 PIC setup
// =====================================================================
// The kernel runs with interrupts disabled and polls devices, but QEMU and
// real hardware still assert IRQs.  Without a valid IDT any unexpected
// interrupt causes a triple fault.  We install a useful handler for CPU
// exceptions (0-31) that prints the vector/error code and halts, plus a
// dummy handler for hardware IRQs so stray PIC interrupts don't crash us.

// In IA-32e long mode every IDT gate is 16 bytes (8-byte gates are
// ILLEGAL and are decoded as 16 bytes, yielding a non-canonical 64-bit
// 32-bit protected-mode IDT gate (8 bytes).  The CPU parses each gate at
// 8-byte strides in 32-bit mode; using a 64-bit (16-byte) gate struct here
// misaligns every odd vector (they read back as not-present -> #GP) and,
// worse, maps even vectors onto the *previous* gate's low half -- e.g. the
// int 0x80 syscall gate silently pointed at isr_dummy.  Fixed to the true
// 32-bit layout: offset_low(16) + selector(16) + zero(8) + type_attr(8) +
// offset_high(16).  Handlers are <4GB so offset_high only needs 16 bits.
struct IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;        // must be 0 for 32-bit gates (no IST in 32-bit)
    uint8_t  type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct IdtPtr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static IdtEntry g_idt[256];
static IdtPtr   g_idtp;

// ASM stubs: each pushes vector number, then a placeholder for the error
// code (or the real error code pushed by the CPU).  See isr_common.
#define ISR_STUB_BODY \
    "    pushal\n" \
    "    mov %esp, %eax\n" \
    "    push %eax\n" \
    "    call isr_common_c\n" \
    "    add $4, %esp\n" \
    "    popal\n" \
    "    add $4, %esp\n" \
    "    iret\n"

extern "C" void isr_common_c(uint32_t* esp);

extern "C" void isr_err_8(void);   // #DF has error code
extern "C" void isr_err_10(void);  // #TS
extern "C" void isr_err_11(void);  // #NP
extern "C" void isr_err_12(void);  // #SS
extern "C" void isr_err_13(void);  // #GP
extern "C" void isr_err_14(void);  // #PF
extern "C" void isr_err_17(void);  // #AC
extern "C" void isr_err_30(void);  // #SX

__asm__ (
    ".global isr_common_c\n"
    ".global isr_err_8,isr_err_10,isr_err_11,isr_err_12,isr_err_13,isr_err_14,isr_err_17,isr_err_30\n"
    "isr_err_8:  pushl $8;"  ISR_STUB_BODY
    "isr_err_10: pushl $10;" ISR_STUB_BODY
    "isr_err_11: pushl $11;" ISR_STUB_BODY
    "isr_err_12: pushl $12;" ISR_STUB_BODY
    "isr_err_13: pushl $13;" ISR_STUB_BODY
    "isr_err_14: pushl $14;" ISR_STUB_BODY
    "isr_err_17: pushl $17;" ISR_STUB_BODY
    "isr_err_30: pushl $30;" ISR_STUB_BODY
);

extern "C" void isr_dummy(void);
__asm__ (
    ".global isr_dummy\n"
    "isr_dummy:\n"
    "    pushal\n"
    "    movb $0x20, %al\n"
    "    outb %al, $0xA0\n"   // EOI to slave PIC
    "    outb %al, $0x20\n"   // EOI to master PIC
    "    popal\n"
    "    iret\n"
);

static void idt_set_gate(uint8_t num, uint32_t offset, uint16_t selector, uint8_t flags){
    g_idt[num].offset_low  = (uint16_t)(offset & 0xFFFF);
    g_idt[num].selector    = selector;
    g_idt[num].zero        = 0;
    g_idt[num].type_attr   = flags;           // 0x8E = present,DPL0,32-bit intr gate
    g_idt[num].offset_high = (uint16_t)((offset >> 16) & 0xFFFF);
}

// Names for the first 32 x86 exception vectors.
static const char* const g_exc_name[32] = {
    "#DE divide error",      "#DB debug",            "NMI",                  "#BP breakpoint",
    "#OF overflow",          "#BR bound range",      "#UD invalid opcode",   "#NM device not avail",
    "#DF double fault",      "coproc seg overrun",   "#TS invalid TSS",    "#NP segment not present",
    "#SS stack fault",       "#GP general protection","#PF page fault",       "reserved",
    "#MF x87 FPE",          "#AC alignment check",  "#MC machine check",   "#XM SIMD FPE",
    "#VE virtualization",    "reserved",             "reserved",             "reserved",
    "reserved",              "reserved",             "reserved",             "reserved",
    "reserved",              "reserved",             "#SX security",         "reserved"
};

// Forward declaration so the fault handler below can paint a screen beacon.
// (The canonical definition/declaration of boot_beacon lives in gui.cpp and
// is also declared near kmain, but isr_common_c precedes that.)
extern "C" void boot_beacon(uint8_t, uint8_t, uint8_t);

// 32-bit kernel: diag_step is a no-op.  The 32-bit kernel has its own ISR
// framework (isr_common_c) and does not use the 64-bit IDT / disk-record
// diagnostic path.  gui.cpp / mforms.cpp call diag_step on both kernels, so
// we provide a linkable stub here.
extern "C" void diag_step(uint32_t, const char*) {}

extern "C" void isr_common_c(uint32_t* esp){
    // Layout on stack after our stub:
    //   esp[0..7]  = eax, ecx, edx, ebx, esp, ebp, esi, edi   (pushal)
    //   esp[8]     = vector number (pushed by stub)
    //   esp[9]     = error code (pushed by CPU for error-code exceptions)
    //   esp[10]    = eip
    //   esp[11]    = cs
    //   esp[12]    = eflags
    uint32_t vec   = esp[8];
    uint32_t err   = esp[9];
    uint32_t eip   = esp[10];
    uint32_t eax   = esp[0];
    uint32_t ebx   = esp[3];
    uint32_t ecx   = esp[1];
    uint32_t edx   = esp[2];
    uint32_t old_esp = esp[4];
    uint32_t ebp   = esp[5];
    uint32_t esi   = esp[6];
    uint32_t edi   = esp[7];
    uint32_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));

    serial_puts("\n*** EXCEPTION ");
    char nb[4]; nb[0] = (char)('0' + vec/10); nb[1] = (char)('0' + vec%10); nb[2] = ' '; nb[3] = 0;
    serial_puts(nb);
    serial_puts(g_exc_name[vec < 32 ? vec : 31]);
    serial_puts(" ***\n");

    serial_puts("EIP="); serial_hex(eip); serial_puts(" ERR="); serial_hex(err);
    serial_puts(" CR2="); serial_hex(cr2); serial_puts(" ESP="); serial_hex(old_esp);
    serial_puts("\nEAX="); serial_hex(eax); serial_puts(" EBX="); serial_hex(ebx);
    serial_puts(" ECX="); serial_hex(ecx); serial_puts(" EDX="); serial_hex(edx);
    serial_puts(" ESI="); serial_hex(esi); serial_puts(" EDI="); serial_hex(edi);
    serial_puts(" EBP="); serial_hex(ebp);
    serial_puts("\nSTACK=");
    uint32_t* s = (uint32_t*)old_esp;
    for (int i = 0; i < 12; i++){
        serial_hex(s[i]); serial_puts(" ");
    }
    serial_puts("\nHALT\n");

    // Visible-on-real-hardware fault indicator.  No serial console is
    // available on bare metal, so paint the WHOLE framebuffer GRAY and drop a
    // fault marker at 0x5100.  Gray is format-independent and distinct from
    // every boot-stage beacon colour (red/blue/yellow/magenta/green/orange/
    // white), so a GRAY screen unambiguously means "a CPU exception (e.g.
    // #PF/#GP) was caught and the machine halted here" rather than a clean
    // hang at a particular milestone.  If the screen instead REBOOTS, the
    // fault came from a broken page table (the repaint itself #PF'd ->
    // double fault -> triple fault).  boot_beacon() safely no-ops if VbeInfo
    // width/height are still zero.
    *(volatile uint8_t*)0x5100 = 0xFA;   // 'z' fault marker (NEXQ area)
    boot_beacon(128, 128, 128);          // GRAY: caught CPU exception


    // Try to print to VGA text buffer as well for visibility.
    volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
    const char* msg = "EXCEPTION - see serial";
    for (int i = 0; msg[i] && i < 80; i++) vga[i] = (uint16_t)msg[i] | ((uint16_t)0x4F << 8);

    __asm__ __volatile__("cli; hlt");
    while (1);
}

static void idt_init(){
    // Exceptions 0-31 -> useful handlers.
    for (int i = 0; i < 32; i++){
        uint32_t handler = (uint32_t)&isr_dummy;
        if (i == 8)  handler = (uint32_t)&isr_err_8;   // #DF
        if (i == 10) handler = (uint32_t)&isr_err_10;
        if (i == 11) handler = (uint32_t)&isr_err_11;
        if (i == 12) handler = (uint32_t)&isr_err_12;
        if (i == 13) handler = (uint32_t)&isr_err_13;
        if (i == 14) handler = (uint32_t)&isr_err_14;
        if (i == 17) handler = (uint32_t)&isr_err_17;
        if (i == 30) handler = (uint32_t)&isr_err_30;
        idt_set_gate((uint8_t)i, handler, 0x08, 0x8E);
    }
    // Vectors 32-255 -> dummy handler (hardware IRQs, spurious interrupts).
    for (int i = 32; i < 256; i++)
        idt_set_gate((uint8_t)i, (uint32_t)&isr_dummy, 0x08, 0x8E);

    // Linux syscall compat shim: int 0x80 (DPL 3 so a future ring-3 guest can
    // use it; it is also callable from ring 0 for the Milestone-0 proof).
    idt_set_gate(0x80, (uint32_t)&sys_enter, 0x08, 0xEE);   // unified syscall ABI

    g_idtp.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idtp.base  = (uint32_t)&g_idt;
    __asm__ __volatile__("lidt %0" :: "m"(g_idtp));
}

static void pic_init(){
    // ICW1: start initialization, cascade, expect ICW4
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    // ICW2: vector offsets
    outb(0x21, 0x20); // master IRQ0..7 -> vectors 0x20..0x27
    outb(0xA1, 0x28); // slave  IRQ8..15 -> vectors 0x28..0x2F
    // ICW3: cascade identity (slave on IRQ2)
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    // ICW4: 8086 mode, normal EOI, non-buffered
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    // OCW1: mask everything by default.  The shell polls the keyboard.
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

// =====================================================================
//  kmain
// =====================================================================
// Boot progress beacon helpers (screen-visible, no serial needed).
// boot_beacon() fills the framebuffer with a solid colour; boot_stage()
// records the current milestone in low RAM (0x5101) so a captured serial
// log or a fault marker can pin down where the kernel died.
extern "C" void boot_beacon(uint8_t, uint8_t, uint8_t);
static void boot_stage(uint8_t s){ *(volatile uint8_t*)0x5101 = s; }

extern "C" void kmain(){
    // RAW early marker: proves kmain entry is reached (bypasses C++ serial_puts)
    __asm__ __volatile__("movb $0x5A, %%al\n\tmovw $0x3F8, %%dx\n\toutb %%al, %%dx" ::: "eax","edx");
    // ---- Mask all 8259 PIC interrupts ----
    // We have no protected-mode IDT yet. If a hardware IRQ (e.g. PIT timer
    // IRQ0) fires, the CPU reads a garbage IDT gate (IDT base=0 inherited
    // from real-mode IVT) and jumps to a bogus address -> #UD -> #GP ->
    // double fault -> triple fault (QEMU exits with -no-reboot).
    // Mask both PICs so no IRQ can fire until we install a real IDT.
    outb(0x21, 0xFF);   // mask all IRQ0-7  (PIT=IRQ0, keyboard=IRQ1)
    outb(0xA1, 0xFF);   // mask all IRQ8-15
    __asm__ __volatile__("cli");
    serial_puts("[K0] kmain entered, PIC masked\n");
    boot_stage(1);          // milestone: 32-bit kmain entered

    gdt_init();          // Foundation 0: ring-0 + ring-3 GDT/TSS
    serial_puts("[K-g] gdt_init done\n");
    idt_init();
    serial_puts("[K-i] idt_init done\n");
    pic_init();
    serial_init();
    serial_puts("[K-s] serial init done\n");
    serial_puts("[K-p] pic_init done\n");
    boot_stage(2);          // milestone: GDT/IDT/PIC up

    // ---- Map the >4 GiB framebuffer into the 0xF0000000 window EARLY ----
    // On real hardware the GOP LFB lives above 4 GiB, which the 32-bit
    // compat-mode kernel cannot address until paging maps it.  Doing this
    // first makes every later framebuffer write actually visible; otherwise
    // they target an unreachable address and the screen stays on the EFI
    // loader's last colour.  On <4 GiB setups (e.g. QEMU) this branch is a
    // no-op.
    vmm_init();
    boot_stage(5);          // milestone: paging/MMU reconfigured (high FB mapped)

    serial_puts("[K1] kmain entered\n");
#ifdef TEXT_BOOT
    // Text-boot variant (built by `make textboot`): stay at the textual
    // shell so headless security tests can drive the command line and still
    // screendump the VBE framebuffer.  GUI is entered only on demand, e.g.
    // by `winapp <file.exe>`.  The normal build defaults to 1 (auto GUI).
    g_auto_gui = 0;
#else
    // Default into the Win11 GUI desktop on boot (the C# mforms shell now
    // loads and runs; conv.u8 + SFS layout fixes made the desktop stable).
    g_auto_gui = 1;
#endif

    // Headless / server mode flag, set by QEMU:
    //   -device loader,addr=0x501E,data=1,data-len=1
    // When present we still boot the 64-bit kernel (so the 64-bit agent /
    // HTTP stack runs) but stay in the text shell instead of the GUI, which
    // would otherwise triple-fault headlessly.  `make play` never sets this
    // flag, so the desktop keeps coming up there.  The flag lives in the
    // VbeInfo reserved area (0x501C is the existing GUI-request byte), so it
    // survives the 32->64 handoff untouched.
    uint8_t boot_no_gui = *(volatile uint8_t*)0x501E;
    if (boot_no_gui) g_auto_gui = 0;

    // ---- Hardware detection (adapt to all devices) ----
    boot_stage(3);          // milestone: about to probe hardware
    detect_hardware();

    // ---- Display mode selection ----
    // If VBE mode was set by BIOS (INT 10h) or UEFI (GOP), we're already
    // in graphics mode. Use framebuffer console instead of VGA text mode.
    // If not, use VGA text mode and rely on BGA ports for GUI (emulators).
    // Always wipe B8000 first - BIOS startup + boot.asm/stage2 leave a
    // coloured character soup behind which gets rendered as multicolour

    // ---- Probe BGA fallback BEFORE reading the VBE flag ----
    // gui_probe_vbe is allocation-free (no kmalloc), so it's safe to run
    // before heap_init. It writes the default VBE info block to 0x5000
    // when BIOS/UEFI didn't (e.g. SeaBIOS+Cirrus or VMSVGA).
    gui_probe_vbe();
    // "snow" (VGA text) or is copied to the framebuffer (fb_console).
    for (volatile uint16_t* p = (volatile uint16_t*)0xB8000;
         p < (volatile uint16_t*)0xB8000 + 80*25; p++) {
        *p = (uint16_t)' ' | ((uint16_t)0x07 << 8);
    }
    if (g_hw.vbe_available && g_hw.vbe_mode_set) {
        // VBE graphics mode already active (set by stage2 INT 10h or UEFI GOP)
        // Don't call vga_set_text_mode() - display is in VBE mode
        g_fb_console_mode = true;
        serial_puts("[K2] VBE graphics mode active (set by BIOS/UEFI)\n");
        // In VBE mode the legacy 0xB8000 aperture is not reliably readable
        // (Bochs-VBE returns 0xFF), so redirect the terminal to a shadow
        // buffer and initialize it with the same blank attribute we just
        // wrote to 0xB8000.
        VGA_MEMORY = VGA_SHADOW;
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
            VGA_SHADOW[i] = make_entry(' ', make_color(LIGHT_GREY, BLACK));
        // Initialize framebuffer console (renders VGA text buffer to VBE LFB)
        fb_console_init();
        boot_stage(4);          // milestone: framebuffer console up
    } else {
        // Standard VGA text mode (BIOS boot without VBE mode set)
        vga_set_text_mode();
        serial_puts("[K2] VGA text mode set\n");
    }

    // ---- Check if VBE info is available (for GUI) ----
    volatile uint8_t* vbe_flag = (volatile uint8_t*)0x500D;
    // Debug: output the value at 0x500D as hex
    {
        serial_puts("[K-DBG] 0x500D=");
        uint8_t v = *vbe_flag;
        const char* hex = "0123456789ABCDEF";
        char buf[4];
        buf[0] = hex[v >> 4];
        buf[1] = hex[v & 0xF];
        buf[2] = '\n';
        buf[3] = 0;
        serial_puts(buf);
        volatile uint8_t* fb = (volatile uint8_t*)0x5000;
        serial_puts("[K-DBG] 0x5000=");
        buf[0] = hex[fb[0] >> 4];
        buf[1] = hex[fb[0] & 0xF];
        serial_puts(buf);
        serial_puts("\n");
    }
    if(*vbe_flag == 1){
        g_vbe_active = true;
        serial_puts("[K-VBE] VBE info available (GUI ready)\n");
    }

    memset_(g_hist,0,sizeof(g_hist));
    memset_(g_diskbuf,0,sizeof(g_diskbuf));
    memset_(g_iobuf,0,sizeof(g_iobuf));
    memset_(g_writebuf,0,sizeof(g_writebuf));

    term.init();
    serial_puts("[K3] terminal init done\n");
#ifdef BOOT_LOGO_TEST
    if (g_vbe_active) {
        // Earliest possible "kernel loaded" signal: clear the framebuffer
        // and draw a NexOS boot logo via the framebuffer console.  This path
        // writes the LFB directly (no backbuffer/present()), so a GUI
        // compositor white-line bug cannot mask it.  Built by `make boot-logo`
        // to triage real-hardware boots: if the logo shows, the kernel loaded
        // and the framebuffer is writable; if the screen stays black/garbled
        // with no logo, the kernel never reached graphics mode.
        term.clear_screen();
        term.write("\n\n");
        term.write("   NexOS\n");
        term.write("   kernel loaded - framebuffer writable\n");
        term.write("   [K1] kmain entered, VBE/GOP active\n");
        for (volatile uint32_t i = 0; i < 500000000U; i++) { /* hold for inspection */ }
        __asm__ __volatile__("hlt");
        for (;;) { __asm__ __volatile__("hlt"); }
    }
#endif
    kbd  = Keyboard();
    mouse.init();
    serial_puts("[K4] mouse init done\n");

    mkfs.init();
    // On a CD/ISO boot the SFS image was streamed into RAM by the bootloader
    // and flagged at 0x0900 (no ATA disk exists to hold it).  Mount from RAM
    // when the flag is present; otherwise fall back to the disk probe.
    {
        const uint32_t* cdf = (const uint32_t*)0x0900;
        if (cdf[0] == SFS_RAM_MAGIC) {
            serial_puts("[K] CD-boot RAM-SFS handoff flag present\n");
            sfs.set_ram(cdf[1], cdf[2]);
        }
    }
    sfs.init();
    fat32.init();
    // Independent Linux user-space partition (SFS volume at SFS_LINUX_LBA).
    // Separate from the main SFS volume so `linux <file>` resolves ELF
    // binaries from a dedicated on-disk region.
    if (linux_fs.mount_at(SFS_LINUX_LBA)) {
        serial_puts("[K6] Linux partition mounted at LBA ");
        serial_puts_dec(SFS_LINUX_LBA);
        serial_puts("\n");
    } else {
        serial_puts("[K6] WARN: no Linux partition at LBA ");
        serial_puts_dec(SFS_LINUX_LBA);
        serial_puts(" (linux <file> falls back to main SFS)\n");
    }
    serial_puts("[K6] filesystem init done\n");

    // Register the SFS reader with the Linux-compat shim so `linux <file>`
    // can load ELF images.  The dedicated Linux partition wins; fall back to
    // the main SFS volume so the feature works even without the partition.
    linux_compat_init([](const char* name, unsigned char* buf, int sz) -> int {
        if (linux_fs.mounted) {
            int r = linux_fs.read(name, buf, sz);
            if (r >= 0) return r;
        }
        if (!sfs.mounted) return -1;
        return sfs.read(name, buf, sz);
    });

    // Mount the sandboxed VFS on top of the same SFS volume. Every ring-3
    // file access goes through vfs_check_access() from here on.
    vfs_init(
        [](const char* name, unsigned char* buf, int sz) -> int {
            if (!sfs.mounted) return -1;
            return sfs.read(name, buf, sz);
        },
        [](const char* name) -> int {
            if (!sfs.mounted) return -1;
            return sfs.size_of(name);
        });

    // Arm the Y/N consent prompt (doc 3.3). Until this call, and any time
    // the UI is unavailable, perm_request() denies by default.
    perm_init(perm_ui_console);

    // ---- Memory management ----
    pmm_init();
    // NOTE: vmm_init() (high-FB window mapping) now runs right after the
    // GDT/IDT/PIC come up, so the framebuffer is reachable before any
    // framebuffer console / beacon write.
    heap_init();
    serial_puts("[K7] memory management init done\n");
    serial_puts("[K] Command-line shell (GUI starts on demand via 'run <winfile>')\n");

    // ---- Network initialization ----
    serial_puts("[K8] Initializing network...\n");
    {
        int net_ret = net_init();
        if(net_ret == 0){
            g_net_initialized = true;
            serial_puts("[K8] Network initialized successfully\n");
        } else {
            serial_puts("[K8] Network init failed (no NIC?)\n");
        }
    }

    term.set_color(make_color(GREEN,BLACK));
    term.write("Hello world from C++ kernel!\n");
    serial_puts("[K5] Hello world written\n");
    term.set_color(make_color(WHITE,BLACK));
    term.write("Two-stage boot succeeded. 32-bit protected mode.\n\n");

    // Real-hardware notice: without a legacy i8042 controller there is no
    // keyboard input at all (modern laptops in pure UEFI mode). Say so on
    // screen instead of leaving the user staring at a dead prompt.
    if (!g_hw.keyboard_present) {
        term.set_color(make_color(BROWN,BLACK));
        term.write("WARNING: no PS/2 (i8042) keyboard controller detected.\n");
        term.write("         Keyboard input is unavailable on this machine.\n");
        term.write("         A USB HID driver is required for real hardware.\n\n");
        term.set_color(make_color(WHITE,BLACK));
    }

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
    else if (vmm_long_mode)   term.write(" (UEFI long mode)\n");
    else                      term.write(" (firmware)\n");

    // Network status
    if(g_net_initialized){
        term.set_color(make_color(GREEN,BLACK));
        term.write("NET:  UP  HTTP server on http://");
        term.write(net_ip_str());
        term.write(":8080\n");
    } else {
        term.set_color(make_color(BROWN,BLACK));
        term.write("NET:  not detected (use 'netstart' to retry)\n");
    }

    // GUI status
    if(g_vbe_active){
        term.set_color(make_color(GREEN,BLACK));
        term.write("GUI:  VBE "); term.write_dec(gui_get_width());
        term.write("x"); term.write_dec(gui_get_height());
        term.write(" (text shell by default; 'gui' to enter desktop)\n");
    } else {
        term.set_color(make_color(BROWN,BLACK));
        term.write("GUI:  not available (text mode)\n");
    }

    term.set_color(make_color(CYAN,BLACK));
    term.write("\nInitializing security system...\n");
    userdb_load();
    if(g_user_count == 0) seed_default_users();
    permdb_load();

    // Drain any PS/2 bytes left over from power-on (keyboard BAT / ACK /
    // identify responses) and clear stuck modifier / extended-key state
    // so the very first keystroke typed is not silently dropped.
    kbd.drain();
    kbd.reset();

    // ---- Sign-in ----
    // On a machine with a framebuffer the desktop comes up automatically
    // and the managed lock screen (csharp/apps/Shell/Login.cs) collects
    // the credentials, so asking for them here as well would mean typing
    // the password twice.  Only a text-mode boot -- no VBE/BGA, or the
    // `nogui` flag -- falls back to the console prompt.
    const bool graphical_login = (g_auto_gui != 0) && g_vbe_active;
    if (graphical_login) {
        serial_puts("[K32] sign-in deferred to the graphical lock screen\n");
        term.write("Sign-in deferred to the graphical lock screen.\n");
    } else {
        login_prompt();
    }

    term.set_color(make_color(CYAN,BLACK));
    term.write("\nShell ready. Type 'help' for commands.\n");
    term.write("Tab=autocomplete, Arrows=cursor/history, PgUp/PgDn=scroll, Home/End, Ctrl+C/V/L/Z/A.\n");
    term.write("In the GUI: Ctrl+Left/Right/Up switch virtual desktops (default / AI / toggle).\n\n");

    // ---- Default-enable GUI ----
    // Automatically enter the Win11 desktop after boot when a graphics
    // framebuffer is available.  `cmd_gui` self-guards on g_vbe_active, so
    // text-only boots (no VBE/BGA) safely fall through to the shell.  The
    // `nogui` command flips g_auto_gui off for subsequent boots.
    //
    // The desktop itself now runs on the 64-bit kernel: that is where the
    // real GGUF LLM lives, so the AI desktop answers with the built-in
    // model.  Ask for the GUI via byte 0x501C and switch to long mode.
    //
    // 0x501C, NOT 0x5010: 0x5010 is the FIRST BYTE of VbeInfo.framebuffer_phys64,
    // so the old flag silently ORed 1 into the framebuffer address.  With a
    // <4GB framebuffer nobody noticed (the 64-bit kernel used the 32-bit
    // framebuffer_phys field), but on real hardware -- where GOP reports the
    // LFB at 0x4000000000 -- it turned into 0x4000000001 and every pixel write
    // landed one byte off, i.e. a permanently garbled/black display.
    // 0x501C is VbeInfo.reserved[0] and is unused by both kernels.
    // ---- Default boot policy (anti-crash) ----
    // The 64-bit Win11 desktop (C# mforms shell) triple-faults under QEMU
    // inside mforms_paint_desktop -- the last serial line before the hang is
    // "[DIAG] step=200 mforms_paint_desktop enter" (see build/boot_crash.log).
    // So os.img boots to a STABLE shell by default and only reaches the 64-bit
    // kernel when explicitly asked:
    //   * default (g_auto_gui==0, no flag)  -> stay in the 32-bit shell.
    //         Proven to serve the HTTP agent API (same stack as os_textboot).
    //   * g_auto_gui==1  -> set 0x501C=1 and switch to 64-bit GUI (opt-in;
    //         still crashes under QEMU for now -- use `gui` to try it).
    //   * boot_no_gui (0x501E, set by QEMU -device loader) -> switch to the
    //         64-bit kernel in TEXT mode (real GGUF LLM + agent/HTTP stack),
    //         stable.  Used for headless/server scenarios.
    if (g_auto_gui && g_vbe_active) {
        // Default: boot straight into the 32-bit Win11 GUI desktop (the
        // managed C# NexOS.Forms shell).  This path is stable under both
        // QEMU and real hardware, so it is the default desktop.  The 64-bit
        // kernel (native GGUF LLM) is reached on demand via the `switch64`
        // command -- not automatically, since the 64-bit mforms desktop
        // used to triple-fault headlessly under QEMU.
        serial_puts("[K] boot: auto-launch Win11 GUI desktop (32-bit)\n");
        boot_stage(6);
        cmd_gui("");
    } else if (boot_no_gui && g_vbe_active) {
        // Headless / server mode: stay in the 32-bit text shell (no crashing
        // GUI, and no automatic 64-bit switch).  The 64-bit GGUF kernel is
        // reached on demand via `ask64` / `switch64`, which is the path that
        // actually answers with the embedded transformer.  This matches the
        // 0x501E flag's own comment ("stay in the text shell instead of the
        // GUI") and lets a headless session stage a question and run real
        // inference.  (The old behaviour auto-switched here, which left no
        // chance to type `ask64` before the handoff.)
        serial_puts("[K] boot: 32-bit text shell (headless, no auto-switch)\n");
        boot_stage(6);
        // (no cmd_switch64 -- user drives 64-bit inference via `ask64`)
    } else {
        // Default: stable 32-bit shell.  Network + HTTP agent API work here.
        serial_puts("[K] boot: 32-bit shell (stable default)\n");
    }

    char inbuf[HIST_LEN];
    int  hist_recall = -1;      // command history recall index (-1 = not recalling)

    for(;;){
        // ---- GUI event loop ----
        if(gui_is_active()){
            bool gui_prev_left = false;
            bool gui_prev_right = false;
            int gui_tick_counter = 0;
            while(gui_is_active()){
                if(g_net_initialized) net_poll();

                // Remote console over COM1 (frontend ops terminal / bridge).
                // Characters arriving on the serial port are accumulated and
                // dispatched to the shell dispatcher on newline, so the web
                // ops console can drive NexOS directly. Output is mirrored
                // back to COM1 by Terminal::put_char (g_term_serial).
                {
                    int ch;
                    while ((ch = serial_try_getc()) >= 0) {
                        if (ch == '\r' || ch == '\n') {
                            if (g_serial_inlen > 0) {
                                g_serial_inbuf[g_serial_inlen] = 0;
                                run_command(g_serial_inbuf);
                                g_serial_inlen = 0;
                            }
                        } else if (ch == 0x7F || ch == '\b') {
                            if (g_serial_inlen > 0) g_serial_inlen--;
                        } else if (g_serial_inlen < (int)sizeof(g_serial_inbuf) - 1) {
                            g_serial_inbuf[g_serial_inlen++] = (char)ch;
                        }
                    }
                }

                // Update clock every ~50 iterations
                if(++gui_tick_counter > 50){
                    gui_tick();
                    gui_tick_counter = 0;
                }

                // Drive window animations (open/close/minimize) at ~60fps
                gui_animate_frame();

                uint8_t st = inb(0x64);
                if(st == 0xFF) st = 0;  // no i8042 (floating bus) - treat as idle
                if(st & 0x01){
                    uint8_t data = inb(0x60);
                    if(st & 0x20){
                        MouseEvent me = mouse.process(data);
                        if(me.valid){
                            if(me.dx != 0 || me.dy != 0){
                                gui_mouse_move(me.dx, -me.dy);
                            }
                            if(me.left && !gui_prev_left){
                                gui_mouse_down();
                            } else if(!me.left && gui_prev_left){
                                gui_mouse_up();
                            }
                            if(me.right && !gui_prev_right){
                                gui_mouse_down_right();
                            }
                            gui_prev_left = me.left;
                            gui_prev_right = me.right;
                        }
                    } else {
                        KbdEvent e = kbd.process(data);
                        if(e.type == K_CHAR){
                            // ESC no longer tears the desktop down.  It used
                            // to call gui_exit() right here, so one stray
                            // keypress dumped the whole session back to the
                            // text terminal and looked like a crash.  The
                            // key now goes to gui_handle_key() like any
                            // other, where it only cancels the IME.  The
                            // terminal stays reachable through the Start
                            // menu / desktop "Terminal" shortcut.
                            gui_handle_key(e.ch);
                        } else if(e.type == K_TAB){
                            gui_handle_key('\t');
                        } else if(e.type == K_SHIFT){
                            gui_toggle_ime();   // Shift toggles 中文/EN
                        }                         else if(e.type == K_CTRL_C){ gui_handle_ctrl(1); }
                        else if(e.type == K_CTRL_V){ gui_handle_ctrl(2); }
                        else if(e.type == K_CTRL_Z){ gui_handle_ctrl(3); }
                        else if(e.type == K_CTRL_A){ gui_handle_ctrl(4); }
                        else if(e.type == K_CTRL_S){ gui_handle_ctrl(8); }
                        else if(e.type == K_DESK_L){ serial_puts("[desk] switch -> default (desktop 1)\n"); gui_handle_ctrl(5); }
                        else if(e.type == K_DESK_R){ serial_puts("[desk] switch -> AI (desktop 2)\n"); gui_handle_ctrl(6); }
                        else if(e.type == K_DESK_TGL){ serial_puts("[desk] toggle virtual desktop\n"); gui_handle_ctrl(7); }
                    }
                }
                // Idle poll throttle: when no keyboard/mouse data is pending the
                // inner branch above does nothing, so without this the guest
                // spins the vCPU at 100% polling port 0x64.  That starves
                // QEMU's device emulation (LFB flush, disk, net) and the GUI
                // compositor, which is what makes the desktop feel sluggish on
                // a 512 MB VM.  Busy-wait ~1.5 ms on the TSC between idle polls
                // so the host gets time slices.  (PIT timer IRQs are masked, so
                // we cannot use msleep/hlt.)
                if (st == 0) {
                    uint32_t _t0, _t1;
                    __asm__ __volatile__("rdtsc" : "=a"(_t0) : : "edx");
                    do { __asm__ __volatile__("rdtsc" : "=a"(_t1) : : "edx"); }
                    while ((uint32_t)(_t1 - _t0) < 3000000u);
                }
            }
            // The desktop was torn down (Start menu -> Terminal, or a
            // managed ExitGui).  If the boot deferred sign-in to the lock
            // screen and it was never satisfied, the command line would
            // otherwise start as "nobody": collect the credentials here.
            if (g_login_idx < 0) login_prompt();
            term.render();
            continue;
        }

        if (g_mode == MODE_WRITE) {
            term.set_color(make_color(MAGENTA,BLACK));
            term.write(">> ");
        } else {
            // PowerShell-style prompt: "PS user@NexOS /path> "
            term.set_color(make_color(GREEN,BLACK));
            term.write("PS ");
            term.set_color(make_color(CYAN, BLACK));
            if(is_root() && g_sudo_active) term.write("root");
            else if(g_login_idx >= 0) term.write(g_users[g_login_idx].name);
            else term.write("nobody");
            term.write("@NexOS");
            char pathbuf[FS_NAME_LEN * 4];
            build_prompt_path(pathbuf, sizeof(pathbuf));
            term.set_color(make_color(CYAN, BLACK));
            term.write(" ");
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
            if(g_net_initialized) { net_poll(); }
            uint8_t st=inb(0x64);
            // No i8042 controller (real UEFI laptops): the port floats high and
            // 0xFF would be decoded as an endless stream of mouse packets,
            // scrolling the screen forever. Treat it as "no data".
            if(st == 0xFF) st = 0;
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
                        term.push_undo();
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
                    } else if(e.type==K_CTRL_Z){
                        // Ctrl+Z: undo last edit
                        term.snap_bottom();
                        if(term.pop_undo()){
                            term.get_line(inbuf, &inlen);
                        }
                    } else if(e.type==K_CTRL_A){
                        // Ctrl+A: select entire current input line
                        term.snap_bottom();
                        term.select_all();
                    } else if(e.type==K_CHAR){
                        term.snap_bottom();
                        term.push_undo();
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
