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
    while (inb(0x64) & 0x02) {}  // Wait for input buffer empty
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
    // Browser HTTP client API
    int  browser_navigate(const char* url);
    int  browser_status(void);
    int  browser_get_page(char* buf, int bufsize);
    void browser_reset(void);
}
static bool g_net_initialized = false;

// =====================================================================
//  64-bit kernel switcher (implemented in switch32to64.asm)
// =====================================================================
extern "C" void switch_to_64bit(void);

#define KERNEL64_LBA     2048   // LBA where kernel64.bin is stored on disk (after kernel.bin region)
#define KERNEL64_SECTORS 2048   // up to 1 MiB (kernel64.bin ~260 KB)
#define KERNEL64_ADDR    0x100000  // Load address: 1MB

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

// ---- GUI terminal command output capture ----
// When g_capturing is true, terminal output is also captured into g_exec_output
// so the GUI terminal can display command results.
static char g_exec_output[2048];
static int  g_exec_output_len;
static bool g_capturing;

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

    void write_hex(uint32_t v){
        put_char('0'); put_char('x');
        char b[8]; int i=0;
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
    int      (*get_cpu_64bit_capable)(void);  // CPU supports 64-bit (capability)
};

extern "C" {
    void gui_set_callbacks(const GuiCallbacks* cb);
    int  gui_init(void);
    void gui_probe_vbe(void);
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
    void gui_exit(void);
    void gui_render(void);
    void gui_tick(void);
    // Open a Windows executable file (exe/bat/ps1/com) inside the GUI.
    // Initializes the GUI on demand (kernel boots to command line only).
    void gui_open_file(const char* filename, const char* args);
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

#define MKFS_SUPER_LBA    512
#define MKFS_TABLE_LBA    513
#define MKFS_TABLE_SECT   16
#define MKFS_DATA_LBA     529
#define MKFS_DATA_SECTORS 271
// User-data disk (secondary ATA / VHD): data area size in sectors.
// data.vhd must be at least (512 + 15200) sectors = ~7.7 MB.
#define DATA_DISK_SECTORS 15200

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
        sb.version = 1;
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
        FileEntry* fe = (FileEntry*)g_fsbuf;
        int size = (int)fe->size;
        if (size > bufsize) size = bufsize;
        int sectors = ((int)fe->size + 511) / 512;
        uint8_t* dst = (uint8_t*)buf;
        int remaining = size;
        for (int i = 0; i < sectors && remaining > 0; i++) {
            fs_read_sector(fe->start_lba + i, (uint16_t*)g_fsbuf);
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
static uint32_t* pmm_bitmap   = (uint32_t*)PMM_BITMAP_ADDR;
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
    // EFER MSR (0xC0000080), bit 8 = LME
    uint32_t eax, edx;
    __asm__ __volatile__("rdmsr":"=a"(eax),"=d"(edx):"c"(0xC0000080u));
    return (edx & (1u << 8)) != 0;
}

static void vmm_init(){
    vmm_long_mode = vmm_check_long_mode();
    vmm_paging_on = (read_cr0() & 0x80000000u) != 0;

    if (vmm_long_mode){
        // UEFI path: firmware already set up 4-level identity-mapped paging.
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
    }

    // Enable PSE (CR4.PSE = bit 4)
    uint32_t cr4 = read_cr4();
    write_cr4(cr4 | 0x10);

    // Load page directory into CR3
    write_cr3(0x70000);

    // Enable paging (CR0.PG = bit 31)
    uint32_t cr0 = read_cr0();
    write_cr0(cr0 | 0x80000000u);

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
    term.write("  Ctrl+Up/Dn  Cycle clipboard history\n");
    term.write("\nMouse: wheel=scroll, drag=select, click=refocus\n");
}

static void cmd_about(){
    term.write("MiniOS v4.0  -  C++ freestanding kernel\n");
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
    // 默认: MiniOS shell 脚本 (.sh)
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
    g_user_count = 2;
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
    for(;;){
        uint8_t st = inb(0x64);
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
    term.write("\n=== MiniOS Security Login ===\n");
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
    term.write("Users on MiniOS:\n");
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

static void cmd_login(const char* name){
    if(!name[0]){ term.write("Usage: login <username>\n"); return; }
    int idx = -1;
    for(int i = 0; i < g_user_count; i++){
        if(g_users[i].exists && strcmp_(g_users[i].name, name) == 0){ idx = i; break; }
    }
    if(idx < 0){ term.write("User not found: "); term.write(name); term.put_char('\n'); return; }
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
//  Switch to 64-bit kernel
//  Reads kernel64.bin from disk (LBA 400) into memory at 0x100000,
//  then calls switch_to_64bit() which transitions the CPU to long mode.
// =====================================================================
static void cmd_switch64(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("\nSwitching to 64-bit kernel...\n");
    serial_puts("[K32] Switching to 64-bit kernel...\n");

    // ---- Load kernel64.bin from disk to 0x100000 (1MB) ----
    term.write("Loading kernel64.bin from LBA ");
    term.write_dec(KERNEL64_LBA);
    term.write("...\n");

    uint16_t* dst = (uint16_t*)KERNEL64_ADDR;
    for(int i=0; i<KERNEL64_SECTORS; i++){
        ata_read_sector(KERNEL64_LBA + i, dst);
        dst += 256;  // 512 bytes = 256 words
        if(i % 32 == 0){
            term.put_char('\r');
            term.write("  [");
            term.write_dec(i);
            term.write("/");
            term.write_dec(KERNEL64_SECTORS);
            term.write("] sectors loaded");
        }
    }

    term.write("\n  Kernel64 loaded at 0x100000\n");
    term.write("  Transitioning to 64-bit long mode...\n\n");
    serial_puts("[K32] Kernel64 loaded, calling switch_to_64bit()...\n");

    // ---- Call the mode switcher ----
    // This function never returns — it far-jumps to the 64-bit kernel
    switch_to_64bit();

    // Should never reach here
    term.set_color(make_color(RED, BLACK));
    term.write("ERROR: switch_to_64bit() returned!\n");
    serial_puts("[K32] ERROR: switch_to_64bit returned!\n");
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
    // Bring up the GUI subsystem on demand (allocates backbuffer, etc.)
    if(gui_init() != 0){
        term.set_color(make_color(RED, BLACK));
        term.write("GUI initialization failed.\n");
        term.set_color(make_color(WHITE, BLACK));
        return;
    }

    term.set_color(make_color(CYAN, BLACK));
    term.write("Entering Win11 Desktop GUI mode...\n");
    serial_puts("[K32] Entering Win11 GUI mode\n");

    gui_enter();

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
    "gui",
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
    else if(!strcmp_(cmd,"switch")||!strcmp_(cmd,"switch64")) cmd_switch64();
    // GUI
    else if(!strcmp_(cmd,"gui"))  cmd_gui();
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
            ata_read_sector(SFS_DIR_LBA + s, (uint16_t*)g_fsbuf);
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
// This 32-bit kernel always runs in 32-bit protected mode, so return false.
// (g_hw.has_long_mode indicates CPU *capability*, not current mode.)
// After 'switch' command, the 64-bit kernel (kernel64.cpp) takes over
// and its own gui_cb_is_64bit() returns true.
static bool gui_cb_is_64bit(void) {
    return false;  // 32-bit protected mode
}

// Returns whether the CPU *supports* 64-bit (capability, not current mode)
static int gui_cb_cpu_64bit_capable(void) {
    return g_hw.has_long_mode ? 1 : 0;
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
    cb.get_time            = gui_cb_get_time;
    cb.get_os_name         = gui_cb_os_name;
    cb.is_64bit            = gui_cb_is_64bit;
    cb.browser_navigate    = gui_cb_browser_navigate;
    cb.browser_status      = gui_cb_browser_status;
    cb.browser_get_page    = gui_cb_browser_get_page;
    cb.browser_reset       = gui_cb_browser_reset;
    cb.exec_command        = gui_cb_exec_command;
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
    gui_set_callbacks(&cb);
}

// =====================================================================
//  kmain
// =====================================================================
extern "C" void kmain(){
    serial_puts("[K1] kmain entered\n");

    // ---- Hardware detection (adapt to all devices) ----
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
        // Initialize framebuffer console (renders VGA text buffer to VBE LFB)
        fb_console_init();
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
    kbd  = Keyboard();
    mouse.init();
    serial_puts("[K4] mouse init done\n");

    mkfs.init();
    sfs.init();
    fat32.init();
    serial_puts("[K6] filesystem init done\n");

    // ---- Memory management ----
    pmm_init();
    vmm_init();
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
        term.write(" (type 'gui' to enter desktop)\n");
    } else {
        term.set_color(make_color(BROWN,BLACK));
        term.write("GUI:  not available (text mode)\n");
    }

    term.set_color(make_color(CYAN,BLACK));
    term.write("\nInitializing security system...\n");
    userdb_load();
    if(g_user_count == 0) seed_default_users();
    permdb_load();
    login_prompt();

    term.set_color(make_color(CYAN,BLACK));
    term.write("\nShell ready. Type 'help' for commands.\n");
    term.write("Tab=autocomplete, Arrows=cursor/history, PgUp/PgDn=scroll, Home/End, Ctrl+C/V/L.\n\n");

    char inbuf[HIST_LEN];
    int  hist_recall = -1;      // command history recall index (-1 = not recalling)

    for(;;){
        // ---- GUI event loop ----
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
                            gui_prev_left = me.left;
                        }
                    } else {
                        KbdEvent e = kbd.process(data);
                        if(e.type == K_CHAR){
                            if(e.ch == 0x1B){
                                gui_exit();
                                term.set_color(make_color(GREEN,BLACK));
                                term.write("\n[Returned to command line]\n");
                                term.set_color(make_color(LIGHT_GREY,BLACK));
                                term.render();
                            } else {
                                gui_handle_key(e.ch);
                            }
                        } else if(e.type == K_TAB){
                            gui_handle_key('\t');
                        }
                    }
                }
            }
            term.render();
            continue;
        }

        if (g_mode == MODE_WRITE) {
            term.set_color(make_color(MAGENTA,BLACK));
            term.write(">> ");
        } else {
            // PowerShell-style prompt: "PS user@minios /path> "
            term.set_color(make_color(GREEN,BLACK));
            term.write("PS ");
            term.set_color(make_color(CYAN, BLACK));
            if(is_root() && g_sudo_active) term.write("root");
            else if(g_login_idx >= 0) term.write(g_users[g_login_idx].name);
            else term.write("nobody");
            term.write("@minios");
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
