// =====================================================================
//  net_virtio.cpp - virtio-net PCI driver for NexOS
// ---------------------------------------------------------------------
//  Stage-1 transport upgrade from docs/PERFORMANCE.md: replace the 10 Mbps
//  NE2000 ISA polling NIC with a virtio-net PCI device (>=1 Gbps) when one
//  is present.  This file is PROBE-GATED: net.cpp only switches to it after
//  virtio_net_probe() finds a real virtio-net PCI device.  With the default
//  QEMU "-net nic,model=ne2k_isa" config no such device exists, so the
//  existing NE2000 path is used unchanged.  See PERFORMANCE.md for caveats.
//
//  STATUS: prototype.  Written against the QEMU legacy (virtio 0.9.5) PCI
//  register layout (VIRTIO_PCI_*).  Requires real-hardware / QEMU validation
//  before relying on it.  The NE2000 fallback is always safe.
//
//  Legacy virtio-pci IO-region register offsets (QEMU hw/virtio/virtio-pci.c):
//    0x00 HOST_FEATURES   (RO u32)
//    0x04 GUEST_FEATURES  (RW u32)
//    0x08 QUEUE_PFN       (RW u32, page frame << 12)
//    0x0E QUEUE_SEL       (RW u16)
//    0x10 QUEUE_NOTIFY    (RW u16, queue index)
//    0x12 STATUS          (RW u8)
//    0x13 ISR             (RO u8)
//    0x14 CONFIG          (device-specific; MAC at +0 for virtio-net)
// =====================================================================
#include <stdint.h>
#include <stddef.h>

// ---- kernel exports used here ----
extern "C" void* kmalloc(uint32_t size);
extern "C" void  kfree(void* ptr);
extern "C" void  net_log(const char* s);

// ---- port IO ----
static inline uint8_t  io_inb(uint16_t p){ uint8_t v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t io_inw(uint16_t p){ uint16_t v; __asm__ __volatile__("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t io_inl(uint16_t p){ uint32_t v; __asm__ __volatile__("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void     io_outb(uint16_t p, uint8_t  v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void     io_outw(uint16_t p, uint16_t v){ __asm__ __volatile__("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void     io_outl(uint16_t p, uint32_t v){ __asm__ __volatile__("outl %0,%1"::"a"(v),"Nd"(p)); }

// ---- PCI config access (0xCF8/0xCFC) ----
static uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off){
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFC);
    io_outl(0xCF8, addr);
    return io_inl(0xCFC);
}

// ---- virtio register window (IO BAR of the virtio-net device) ----
static uint16_t g_vio_base = 0;          // IO BAR base (0 = not mapped)
static uint8_t  g_vio_mac[6];
static bool     g_vio_present = false;

static inline uint8_t  vio_rb(uint16_t o){ return io_inb((uint16_t)(g_vio_base + o)); }
static inline uint16_t vio_rw(uint16_t o){ return io_inw((uint16_t)(g_vio_base + o)); }
static inline uint32_t vio_rl(uint16_t o){ return io_inl((uint16_t)(g_vio_base + o)); }
static inline void     vio_wb(uint16_t o, uint8_t  v){ io_outb((uint16_t)(g_vio_base + o), v); }
static inline void     vio_ww(uint16_t o, uint16_t v){ io_outw((uint16_t)(g_vio_base + o), v); }
static inline void     vio_wl(uint16_t o, uint32_t v){ io_outl((uint16_t)(g_vio_base + o), v); }

// ---- virtqueue (legacy layout, all rings in one physically contiguous page) ----
struct VioQueue {
    uint32_t* desc;       // descriptor table (16-byte entries)
    uint16_t* avail;      // avail ring (2 + 2*num + 2)
    uint16_t* used;       // used ring (2 + 2*num + 2*num) -- status bytes follow
    uint32_t  phys;       // physical base of the whole ring block
    uint16_t  num;        // queue size
    uint16_t  free_head;  // next free descriptor
    uint16_t  avail_idx;  // producer index
};

#define VIO_Q_RX 0
#define VIO_Q_TX 1
#define VIO_Q_NUM 256
#define VQ_ALIGN 4096

// NOTE: kmalloc in this kernel returns an identity-mapped low-memory address;
// treat the returned pointer as its own physical address.  If the kernel ever
// moves to a non-identity map this must become a real virt_to_phys().
static inline uint32_t virt_to_phys(void* p){ return (uint32_t)(uintptr_t)p; }

// Local memcpy (kernel has no libc); declared here, defined below.
static void net_memcpy_local(void* d, const void* s, int n);

static void vq_init(VioQueue* q, uint16_t num, void* mem, uint32_t phys){
    q->num = num;
    q->phys = phys;
    q->desc  = (uint32_t*)mem;
    q->avail = (uint16_t*)((uint8_t*)mem + num * 16);
    q->used  = (uint16_t*)((uint8_t*)mem + num * 16 + 4 + num * 2);
    q->free_head = 0;
    q->avail_idx = 0;
    // chain all descriptors into a free list
    for (uint16_t i = 0; i < num; i++){
        q->desc[i*4 + 0] = 0;       // addr (filled on use)
        q->desc[i*4 + 1] = 0;       // len
        q->desc[i*4 + 2] = 0;       // flags
        q->desc[i*4 + 3] = 0;       // next
        if (i + 1 < num) q->desc[i*4 + 3] = i + 1;  // link
    }
}

// Allocate one descriptor (or a chain).  Returns descriptor index, -1 if full.
static int vq_alloc_desc(VioQueue* q){
    if (q->free_head == q->num) return -1;
    int idx = q->free_head;
    q->free_head = q->desc[idx*4 + 3] & 0xFFFF;
    return idx;
}

static bool vio_setup_queue(uint16_t sel, VioQueue* q){
    vio_ww(0x0E, sel);                       // QUEUE_SEL
    uint16_t sz = vio_rw(0x0C);               // queue size (legacy: some devices)
    if (sz == 0) sz = VIO_Q_NUM;
    q->num = sz;
    // ring block size: desc(num*16) + avail(4+num*2) + used(4+num*4), aligned
    uint32_t blocksz = (uint32_t)sz*16 + 4 + (uint32_t)sz*2 + 4 + (uint32_t)sz*4;
    blocksz = (blocksz + VQ_ALIGN - 1) & ~(VQ_ALIGN - 1);
    void* mem = kmalloc(blocksz);
    if (!mem) return false;
    vq_init(q, sz, mem, virt_to_phys(mem));
    vio_wl(0x08, q->phys >> 12);              // QUEUE_PFN = phys >> 12
    return true;
}

static VioQueue g_vrxq, g_vtxq;
static uint8_t* g_rx_buf[VIO_Q_NUM];          // RX packet buffers
static uint8_t* g_tx_pool[VIO_Q_NUM];         // TX packet buffers (kmalloc'd at probe)
static bool     g_vio_active = false;

// ---- probe + init ----
static int virtio_net_probe_and_init(void){
    // Scan PCI bus 0 for a virtio-net device (vendor 0x1AF4, device 0x1000).
    for (uint8_t slot = 0; slot < 32; slot++){
        uint32_t id = pci_cfg_read32(0, slot, 0, 0x00);
        uint16_t vendor = (uint16_t)(id & 0xFFFF);
        uint16_t device = (uint16_t)(id >> 16);
        if (vendor != 0x1AF4) continue;
        if (device != 0x1000) continue;      // virtio-net (legacy)
        // Map IO BAR0.
        uint32_t bar0 = pci_cfg_read32(0, slot, 0, 0x10);
        if ((bar0 & 0x01) == 0) { net_log("[VIO] BAR0 not IO, skip\n"); continue; }
        g_vio_base = (uint16_t)(bar0 & 0xFFFC);
        if (g_vio_base == 0) continue;
        // Reset + ACK + DRIVER + FEATURES_OK status dance (legacy subset).
        vio_wb(0x12, 0x00);                   // STATUS = reset
        vio_wb(0x12, 0x01);                   // ACKNOWLEDGE
        vio_wb(0x12, 0x03);                   // ACKNOWLEDGE | DRIVER
        // Feature negotiation: accept MAC (1<<5), reject everything else risky.
        uint32_t host_feat = vio_rl(0x00);
        (void)host_feat;
        vio_wl(0x04, (1u << 5));              // GUEST_FEATURES = VIRTIO_NET_F_MAC
        vio_wb(0x12, 0x07);                   // DRIVER_OK
        // Read MAC from device-specific config (offset 0x14).
        for (int i = 0; i < 6; i++) g_vio_mac[i] = vio_rb((uint16_t)(0x14 + i));
        // Set up RX/TX virtqueues.
        if (!vio_setup_queue(VIO_Q_RX, &g_vrxq)) { net_log("[VIO] rxq fail\n"); return 0; }
        if (!vio_setup_queue(VIO_Q_TX, &g_vtxq)) { net_log("[VIO] txq fail\n"); return 0; }
        // Pre-post RX buffers: each RX descriptor points at its own 1514-byte
        // buffer; chain not needed (one descriptor per packet).
        for (uint16_t i = 0; i < g_vrxq.num; i++){
            g_rx_buf[i] = (uint8_t*)kmalloc(1514);
            if (!g_rx_buf[i]) { net_log("[VIO] rxbuf fail\n"); return 0; }
        }
        for (uint16_t i = 0; i < g_vrxq.num; i++){
            g_vrxq.desc[i*4 + 0] = virt_to_phys(g_rx_buf[i]);
            g_vrxq.desc[i*4 + 1] = 1514;
            g_vrxq.desc[i*4 + 2] = 1 << 1;     // F_WRITE (device writes)
            g_vrxq.desc[i*4 + 3] = 0;
            g_vrxq.avail[2 + i] = i;
        }
        g_vrxq.avail[0] = 0;                   // flags
        g_vrxq.avail[1] = g_vrxq.num;            // avail_idx
        vio_ww(0x10, VIO_Q_RX);               // notify RX queue

        // Allocate TX packet buffers lazily (only when a virtio device is
        // actually present) so the default NE2000 build's .bss is untouched
        // -- a static 64x1514 pool would otherwise bloat kernel .bss and
        // overlap HEAP_START.
        for (uint16_t i = 0; i < g_vtxq.num; i++){
            g_tx_pool[i] = (uint8_t*)kmalloc(1514);
            if (!g_tx_pool[i]) { net_log("[VIO] txbuf fail\n"); return 0; }
        }
        g_vio_active = true;
        g_vio_present = true;
        net_log("[VIO] virtio-net initialized\n");
        return 1;
    }
    return 0;
}

extern "C" int  virtio_net_active(void){ return g_vio_active ? 1 : 0; }
extern "C" bool virtio_net_present(void){ return g_vio_present; }
extern "C" const uint8_t* virtio_net_mac(void){ return g_vio_mac; }

// ---- send ----
extern "C" void virtio_net_send(const uint8_t* data, int len){
    if (!g_vio_active || len <= 0) return;
    if (len > 1514) len = 1514;
    if (len < 60) len = 60;
    int di = vq_alloc_desc(&g_vtxq);
    if (di < 0) return;
    net_memcpy_local(g_tx_pool[di], data, len);  // see note below
    g_vtxq.desc[di*4 + 0] = virt_to_phys(g_tx_pool[di]);
    g_vtxq.desc[di*4 + 1] = (uint16_t)len;
    g_vtxq.desc[di*4 + 2] = 0;                   // F_NEXT clear -> device reads
    g_vtxq.desc[di*4 + 3] = 0;
    uint16_t a = g_vtxq.avail_idx & 0xFFFF;
    g_vtxq.avail[2 + (a % g_vtxq.num)] = (uint16_t)di;
    g_vtxq.avail_idx++;
    g_vtxq.avail[1] = g_vtxq.avail_idx & 0xFFFF;
    vio_ww(0x10, VIO_Q_TX);                      // notify TX queue
}
// Local memcpy (kernel has no libc); defined here to avoid pulling net.cpp's.
static void net_memcpy_local(void* d, const void* s, int n){
    uint8_t* dp = (uint8_t*)d; const uint8_t* sp = (const uint8_t*)s;
    for (int i = 0; i < n; i++) dp[i] = sp[i];
}

// ---- receive (polling) ----
extern "C" int virtio_net_recv(uint8_t* buf, int maxlen){
    if (!g_vio_active) return 0;
    // used ring: used->idx is at offset 2; each element is (id:u32, len:u32).
    volatile uint16_t* used_idx = &g_vrxq.used[1];
    static uint16_t last_used = 0;
    uint16_t cur = *used_idx;
    if (cur == last_used) return 0;
    // One packet consumed (prototype: process one per call).
    uint16_t e = last_used % g_vrxq.num;
    uint32_t id   = ((uint32_t*)&g_vrxq.used[2])[e*2 + 0];
    uint32_t plen = ((uint32_t*)&g_vrxq.used[2])[e*2 + 1];
    int n = (int)plen;
    if (n > maxlen) n = maxlen;
    if (n < 14) n = 0;
    if (n > 0) net_memcpy_local(buf, g_rx_buf[id], n);
    // Return the descriptor to the RX ring for reuse.
    if ((int)id < g_vrxq.num){
        g_vrxq.desc[id*4 + 0] = virt_to_phys(g_rx_buf[id]);
        g_vrxq.desc[id*4 + 1] = 1514;
        g_vrxq.desc[id*4 + 2] = 1 << 1;
        g_vrxq.desc[id*4 + 3] = 0;
        uint16_t a = g_vrxq.avail_idx & 0xFFFF;
        g_vrxq.avail[2 + (a % g_vrxq.num)] = (uint16_t)id;
        g_vrxq.avail_idx++;
        g_vrxq.avail[1] = g_vrxq.avail_idx & 0xFFFF;
        vio_ww(0x10, VIO_Q_RX);
    }
    last_used = cur;
    return n;
}

// Called once from net.cpp's net_init() BEFORE the NE2000 init path. Returns
// nonzero if a virtio-net device was found and brought up; net.cpp then routes
// NIC traffic through this driver instead of NE2000.
extern "C" int virtio_net_probe(void){
    return virtio_net_probe_and_init();
}
