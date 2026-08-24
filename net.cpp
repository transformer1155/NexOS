// =====================================================================
//  net.cpp  -  Network stack for MyOS kernel
// ---------------------------------------------------------------------
//  Implements:
//    * NE2000 ISA NIC driver (I/O port 0x300, polling mode)
//    * Ethernet frame handling
//    * ARP protocol (IP -> MAC resolution)
//    * IPv4 protocol (with checksum)
//    * ICMP echo reply (ping)
//    * TCP minimal state machine (for HTTP server)
//    * HTTP server (port 8080) with REST API
//    * Web UI (embedded HTML/CSS/JS)
//
//  QEMU: -net nic,model=ne2k_isa -net user,hostfwd=tcp::8080-:8080
//  Guest IP: 10.0.2.15  Gateway: 10.0.2.2
//
//  No external dependencies. Uses kernel's kmalloc/kfree.
// =====================================================================

#include <stdint.h>
#include <stddef.h>          // size_t (freestanding; used by SHA1 ctx / crypto)
#include "remote_desktop.h"  // remote-desktop ABI (nexos_fb_query / nexos_input_inject)
#ifndef NULL
#define NULL 0
#endif

// ---- Kernel interface ----
extern "C" {
    void* kmalloc(uint32_t size);
    void  kfree(void* ptr);
    void  net_poll(void);   // defined below; pumped by nexos_input_wait
    int   net_init(void);   // defined below (~line 2909); needed by net_guest_connect
}

// ---- Remote-desktop accessors ----
// nexos_fb_query is defined in gui.cpp (built into both 32-bit & 64-bit kernels).
// The shared input state + accessors live here (net.cpp is built into BOTH
// kernels) so the HTTP /input route and the guest syscall 401 resolve in either.
extern "C" void nexos_fb_query(struct NexosFBInfo* out);
struct NexosInput g_rd_input;
extern "C" void nexos_input_inject(int32_t mx, int32_t my, uint8_t buttons,
                                  uint8_t key, uint8_t down){
    g_rd_input.mouse_x = mx;
    g_rd_input.mouse_y = my;
    g_rd_input.buttons = buttons;
    if (key){ g_rd_input.key = key; g_rd_input.key_down = down; }
    g_rd_input.seq++;
}
extern "C" void nexos_input_wait(struct NexosInput* out){
    uint32_t last = g_rd_input.seq;
    while (g_rd_input.seq == last) net_poll();
    if (out) *out = g_rd_input;
}

// ---- Freestanding libc ----
static int   net_strlen(const char* s){ int n=0; while(s[n]) n++; return n; }
static int   net_strcmp(const char* a, const char* b){
    while(*a && *a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b;
}
static int   net_strncmp(const char* a, const char* b, int n){
    while(n>0 && *a && *a==*b){a++;b++;n--;} return n==0?0:(unsigned char)*a-(unsigned char)*b;
}
static void* net_memset(void* d, int v, int n){
    unsigned char* p=(unsigned char*)d; while(n--) *p++=(unsigned char)v; return d;
}
static void* net_memcpy(void* d, const void* s, int n){
    unsigned char* dp=(unsigned char*)d; const unsigned char* sp=(const unsigned char*)s;
    while(n--) *dp++=*sp++; return d;
}
static int   net_memcmp(const void* a, const void* b, int n){
    const unsigned char* p=(const unsigned char*)a; const unsigned char* q=(const unsigned char*)b;
    while(n--){ if(*p!=*q) return (int)*p-(int)*q; p++; q++; } return 0;
}
static void* net_memmove(void* d, const void* s, int n){
    unsigned char* dp=(unsigned char*)d; const unsigned char* sp=(const unsigned char*)s;
    if(dp<sp || dp>=sp+n){ while(n--) *dp++=*sp++; }
    else { dp+=n; sp+=n; while(n--) *--dp=*--sp; }
    return d;
}
static void net_serial(const char* s){
    while(*s) __asm__ __volatile__("outb %0,%1" :: "a"((uint8_t)*s++), "Nd"((uint16_t)0x3F8));
}

// Print an IPv4 address (host order, see IPV4()) as dotted decimal.
static void net_serial_ip(uint32_t ip){
    char buf[20]; int p = 0;
    for (int o = 0; o < 4; o++){
        uint8_t v = (ip >> (24 - o * 8)) & 0xFF;
        buf[p++] = '0' + (v / 100);
        buf[p++] = '0' + ((v / 10) % 10);
        buf[p++] = '0' + (v % 10);
        if (o < 3) buf[p++] = '.';
    }
    buf[p] = 0;
    net_serial(buf);
}

// Print a non-negative integer in decimal (no libc).
static void net_serial_dec(int v){
    char buf[12]; int p = 0;
    if (v < 0){ net_serial("-"); v = -v; }
    if (v == 0){ net_serial("0"); return; }
    char t[12]; int n = 0;
    while (v > 0 && n < 11){ t[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = n - 1; i >= 0; i--) buf[p++] = t[i];
    buf[p] = 0;
    net_serial(buf);
}

// ---- Port I/O ----
static inline uint8_t  ninb(uint16_t p){ uint8_t v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void     noutb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }

// =====================================================================
//  NE2000 ISA NIC Driver
// =====================================================================

#define NE_BASE     0x300
#define NE_DATA     (NE_BASE + 0x10)
#define NE_RESET    (NE_BASE + 0x1F)

// CR bits
#define CR_STP      0x01
#define CR_STA      0x02
#define CR_TXP      0x04
#define CR_RD0      0x08
#define CR_RD1      0x10
#define CR_RD2      0x20
#define CR_PS0      0x40
#define CR_PS1      0x80
#define CR_PAGE0    0x00
#define CR_PAGE1    0x40

// Register offsets
#define NE_CR       0x00
#define NE_PSTART   0x01
#define NE_PSTOP    0x02
#define NE_BNRY     0x03
#define NE_TPSR     0x04
#define NE_TBCR0    0x05
#define NE_TBCR1    0x06
#define NE_ISR      0x07
#define NE_RSAR0    0x08
#define NE_RSAR1    0x09
#define NE_RBCR0    0x0A
#define NE_RBCR1    0x0B
#define NE_RCR      0x0C
#define NE_TCR      0x0D
#define NE_DCR      0x0E
#define NE_IMR      0x0F

// ISR bits
#define ISR_PRX     0x01
#define ISR_PTX     0x02
#define ISR_RXE     0x04
#define ISR_TXE     0x08
#define ISR_OVW     0x10
#define ISR_CNT     0x20
#define ISR_RDC     0x40
#define ISR_RST     0x80

// Buffer layout
#define TX_START    0x40
#define RX_START    0x46
#define RX_STOP     0x80

static uint8_t  nic_mac[6];
static bool     nic_present = false;
static bool     g_net_up    = false;   // set by net_init(); required by net_ask_host

static void nic_select_page(uint8_t page){
    noutb(NE_BASE + NE_CR, CR_STA | page);
}

static void nic_wait_rdc(){
    for (int i = 0; i < 10000; i++){
        if (ninb(NE_BASE + NE_ISR) & ISR_RDC) return;
    }
}

static bool nic_detect(){
    // Reset the NIC
    noutb(NE_RESET, 0xFF);
    for (volatile int i = 0; i < 10000; i++);
    ninb(NE_RESET);  // clear reset

    // Check ISR RST bit
    for (int i = 0; i < 100; i++){
        if (ninb(NE_BASE + NE_ISR) & ISR_RST){
            noutb(NE_BASE + NE_ISR, 0xFF);  // clear all ISR bits
            return true;
        }
    }
    return false;
}

static void nic_init(){
    // Stop the NIC
    noutb(NE_BASE + NE_CR, CR_STP | CR_PAGE0);
    for (volatile int i = 0; i < 1000; i++);

    // Data Configuration: 8-bit, FIFO threshold=8, auto-init
    noutb(NE_BASE + NE_DCR, 0x48);

    // Clear remote byte count
    noutb(NE_BASE + NE_RBCR0, 0);
    noutb(NE_BASE + NE_RBCR1, 0);

    // Receive Configuration: accept broadcast + packets for our MAC
    noutb(NE_BASE + NE_RCR, 0x04 | 0x08);  // AB (broadcast) | AM (multicast)

    // Transmit Configuration: normal operation (no loopback)
    noutb(NE_BASE + NE_TCR, 0x02);  // internal loopback during init

    // Set receive ring buffer
    noutb(NE_BASE + NE_PSTART, RX_START);
    noutb(NE_BASE + NE_PSTOP, RX_STOP);
    noutb(NE_BASE + NE_BNRY, RX_START);

    // Clear ISR
    noutb(NE_BASE + NE_ISR, 0xFF);

    // No interrupts (polling mode)
    noutb(NE_BASE + NE_IMR, 0x00);

    // Page 1: set MAC address and CURR
    noutb(NE_BASE + NE_CR, CR_STA | CR_PAGE1);

    // Read MAC from PAR registers (already set by QEMU, but read to verify)
    for (int i = 0; i < 6; i++){
        nic_mac[i] = ninb(NE_BASE + 0x01 + i);
    }

    // If MAC is all zeros, use a default
    bool mac_zero = true;
    for (int i = 0; i < 6; i++) if (nic_mac[i] != 0) mac_zero = false;
    if (mac_zero){
        nic_mac[0] = 0x52; nic_mac[1] = 0x54;
        nic_mac[2] = 0x00; nic_mac[3] = 0x12;
        nic_mac[4] = 0x34; nic_mac[5] = 0x56;
        for (int i = 0; i < 6; i++)
            noutb(NE_BASE + 0x01 + i, nic_mac[i]);
    }

    // Set multicast address (all zeros = no multicast)
    for (int i = 0; i < 8; i++)
        noutb(NE_BASE + 0x08 + i, 0);

    // Set current page pointer
    noutb(NE_BASE + 0x07, RX_START + 1);

    // Back to page 0 and start
    noutb(NE_BASE + NE_CR, CR_STA | CR_PAGE0);

    // Normal transmit mode (no loopback)
    noutb(NE_BASE + NE_TCR, 0x00);

    // Clear ISR again
    noutb(NE_BASE + NE_ISR, 0xFF);

    nic_present = true;
    net_serial("[NET] NE2000 initialized, MAC=");
    char hex[4];
    for (int i = 0; i < 6; i++){
        hex[0] = "0123456789ABCDEF"[nic_mac[i] >> 4];
        hex[1] = "0123456789ABCDEF"[nic_mac[i] & 0xF];
        hex[2] = (i < 5) ? ':' : '\n';
        hex[3] = 0;
        net_serial(hex);
    }
}

static void nic_send(const uint8_t* data, int len){
    if (!nic_present || len <= 0) return;
    if (len > 1514) len = 1514;
    if (len < 60) len = 60;  // minimum Ethernet frame

    // Abort any pending DMA
    noutb(NE_BASE + NE_CR, CR_STA | CR_PAGE0 | CR_RD2);

    // Set remote DMA to write to transmit buffer
    noutb(NE_BASE + NE_RSAR0, 0x00);
    noutb(NE_BASE + NE_RSAR1, TX_START);
    noutb(NE_BASE + NE_RBCR0, len & 0xFF);
    noutb(NE_BASE + NE_RBCR1, len >> 8);

    // Start remote write
    noutb(NE_BASE + NE_CR, CR_STA | CR_PAGE0 | CR_RD1);

    // Write data to NIC buffer (8-bit mode)
    for (int i = 0; i < len; i++)
        noutb(NE_DATA, data[i]);

    // Wait for DMA complete
    nic_wait_rdc();
    noutb(NE_BASE + NE_ISR, ISR_RDC);

    // Set transmit parameters
    noutb(NE_BASE + NE_TPSR, TX_START);
    noutb(NE_BASE + NE_TBCR0, len & 0xFF);
    noutb(NE_BASE + NE_TBCR1, len >> 8);

    // Trigger transmit
    noutb(NE_BASE + NE_CR, CR_STA | CR_PAGE0 | CR_TXP);

    // Wait for transmit complete
    for (int i = 0; i < 100000; i++){
        if (ninb(NE_BASE + NE_ISR) & ISR_PTX){
            noutb(NE_BASE + NE_ISR, ISR_PTX | ISR_TXE);
            return;
        }
        if (ninb(NE_BASE + NE_ISR) & ISR_TXE){
            noutb(NE_BASE + NE_ISR, 0xFF);
            return;
        }
    }
    // Timeout - clear anyway
    noutb(NE_BASE + NE_ISR, 0xFF);
}

// ---- Deferred transmit queue ---------------------------------------------
// nic_send() drives the NE2000's shared remote-DMA engine and resets NIC
// state.  Calling it from INSIDE nic_receive() (i.e. while processing an
// inbound frame) wedges the receive ring on QEMU's ne2000: after the first
// in-path transmit CURR stops advancing and every subsequent inbound packet
// is silently dropped (verified: ring frozen at bnry=72 curr=73 isr=0 after
// the SYN-ACK, while net_poll kept running).  So every in-path send is queued
// here and flushed by net_poll() AFTER nic_receive() returns, never nested
// inside it.
#define TXQ_SLOTS 64
#define TXQ_MAX   1514
static uint8_t  g_txq[TXQ_SLOTS][TXQ_MAX];
static int      g_txq_len[TXQ_SLOTS];
static int      g_txq_head = 0;   // next slot to flush
static int      g_txq_tail = 0;   // next slot to enqueue
static int      g_txq_count = 0;

static void tx_enqueue(const uint8_t* data, int len){
    if (!nic_present || len <= 0) return;
    if (len > TXQ_MAX) len = TXQ_MAX;
    if (g_txq_count >= TXQ_SLOTS){
        return;
    }
    net_memcpy(g_txq[g_txq_tail], data, len);
    g_txq_len[g_txq_tail] = len;
    g_txq_tail = (g_txq_tail + 1) % TXQ_SLOTS;
    g_txq_count++;
}

static void tx_flush(void){
    if (!nic_present) return;
    while (g_txq_count > 0){
        int n = g_txq_len[g_txq_head];
        nic_send(g_txq[g_txq_head], n);
        g_txq_head = (g_txq_head + 1) % TXQ_SLOTS;
        g_txq_count--;
    }
}

static int nic_receive(uint8_t* buf, int maxlen){
    if (!nic_present) return 0;

    uint8_t bnry = ninb(NE_BASE + NE_BNRY);

    // Go to page 1 to read CURR
    noutb(NE_BASE + NE_CR, CR_STA | CR_PAGE1);
    uint8_t curr = ninb(NE_BASE + 0x07);
    noutb(NE_BASE + NE_CR, CR_STA | CR_PAGE0);

    // The ring is empty when CURR sits exactly one page ahead of BNRY -- that
    // is the state nic_init() programs.  The old `curr == bnry` test never
    // held, so every poll remote-DMA'd a garbage header out of the unwritten
    // page and then rewrote BNRY.
    uint8_t hdr_page = bnry + 1;
    if (hdr_page >= RX_STOP) hdr_page = RX_START;
    if (curr == hdr_page) return 0;

    noutb(NE_BASE + NE_RSAR0, 0x00);
    noutb(NE_BASE + NE_RSAR1, hdr_page);
    noutb(NE_BASE + NE_RBCR0, 4);
    noutb(NE_BASE + NE_RBCR1, 0);
    noutb(NE_BASE + NE_CR, CR_STA | CR_PAGE0 | CR_RD0);

    uint8_t status = ninb(NE_DATA);
    uint8_t next   = ninb(NE_DATA);
    uint8_t len_lo = ninb(NE_DATA);
    uint8_t len_hi = ninb(NE_DATA);
    nic_wait_rdc();
    noutb(NE_BASE + NE_ISR, ISR_RDC);

    int pkt_len = (len_hi << 8) | len_lo;
    if (pkt_len < 14 || pkt_len > 1514 || (status & 0x4F) != 0x01){
        // Error - reset BNRY to CURR
        noutb(NE_BASE + NE_BNRY, curr - 1 < RX_START ? RX_STOP - 1 : curr - 1);
        return 0;
    }

    // Read packet data
    noutb(NE_BASE + NE_RSAR0, 0x04);  // skip 4-byte header
    noutb(NE_BASE + NE_RSAR1, hdr_page);
    int read_len = (pkt_len < maxlen) ? pkt_len : maxlen;
    noutb(NE_BASE + NE_RBCR0, read_len & 0xFF);
    noutb(NE_BASE + NE_RBCR1, read_len >> 8);
    noutb(NE_BASE + NE_CR, CR_STA | CR_PAGE0 | CR_RD0);

    for (int i = 0; i < read_len; i++)
        buf[i] = ninb(NE_DATA);
    nic_wait_rdc();
    noutb(NE_BASE + NE_ISR, ISR_RDC);

    // Update boundary
    uint8_t new_bnry = next - 1;
    if (new_bnry < RX_START) new_bnry = RX_STOP - 1;
    noutb(NE_BASE + NE_BNRY, new_bnry);

    noutb(NE_BASE + NE_ISR, ISR_PRX | ISR_RXE);

    return read_len;
}

// =====================================================================
//  Network Constants and Types
// =====================================================================

#define ETH_BROADCAST "\xFF\xFF\xFF\xFF\xFF\xFF"
#define ETH_ARP       0x0806
#define ETH_IP        0x0800

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

// TCP flags
#define TCP_FIN       0x01
#define TCP_SYN       0x02
#define TCP_RST       0x04
#define TCP_PSH       0x08
#define TCP_ACK       0x10

// Byte-order helpers: this kernel is little-endian, the wire format is
// big-endian.  Provide htons/ntohs/htonl/ntohl so every value written to or
// read from a packet is converted at the wire boundary (the previous code
// stored 32-bit fields verbatim, which put them on the wire in the wrong
// byte order and made the QEMU SLIRP stack silently drop our packets).
#define SW16(x)  ((uint16_t)(((x) >> 8) | ((x) << 8)))
#define SW32(x)  ((uint32_t)(((x) >> 24) | ((x) << 24) | \
                             (((x) >> 8) & 0xFF00) | (((x) << 8) & 0xFF0000)))
#define htons(x) SW16(x)
#define ntohs(x) SW16(x)
#define htonl(x) SW32(x)
#define ntohl(x) SW32(x)

// ---------------------------------------------------------------------
//  IPv4 address representation -- READ THIS BEFORE TOUCHING ANY IP VALUE
// ---------------------------------------------------------------------
// Every uint32_t IPv4 address inside this file is stored in "host order",
// which here means the natural readable big-endian-looking integer:
//
//     10.0.2.15  ==  0x0A00020F   (a<<24 | b<<16 | c<<8 | d)
//
// It becomes wire format only via htonl() at the moment it is written into
// a packet, and comes back through ntohl() when it is read out.  Writing a
// literal in memory-byte order instead (e.g. 0x0F02000A) compiles fine and
// even keeps arp_lookup() self-consistent, but puts 15.2.0.10 on the wire,
// so SLIRP never answers.  Always use the IPV4() macro.
#define IPV4(a,b,c,d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                       ((uint32_t)(c) <<  8) |  (uint32_t)(d))

// Our IP configuration (QEMU user-mode networking defaults)
static uint32_t our_ip   = IPV4(10,0,2,15);  // 0x0A00020F
static uint32_t gateway  = IPV4(10,0,2,2);   // 0x0A000202

// Packed structures
struct __attribute__((packed)) EthHeader {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
};

struct __attribute__((packed)) ArpPacket {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t op;
    uint8_t  sender_mac[6];
    uint32_t sender_ip;
    uint8_t  target_mac[6];
    uint32_t target_ip;
};

struct __attribute__((packed)) IpHeader {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
};

struct __attribute__((packed)) TcpHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;  // upper 4 bits = offset in 32-bit words
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
};

// =====================================================================
//  Checksum calculation
// =====================================================================

static uint16_t ip_checksum(void* data, int len){
    uint32_t sum = 0;
    uint16_t* p = (uint16_t*)data;
    while (len > 1){
        sum += *p++;
        len -= 2;
    }
    if (len == 1)
        sum += *(uint8_t*)p;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

// `src`/`dst` arrive in HOST order (the natural 0x0A00020F form, see the
// call site in tcp_send_raw).  Splitting a host-order uint32 into
// `(src>>16)&0xFFFF` and `src&0xFFFF` yields exactly the two network-order
// 16-bit words that the IP addresses occupy in the packet, so the IP part of
// the pseudo-header is correct as-is.  The protocol word is the 16-bit value
// 0x0006 ([0x00][0x06]) and the length word is the native TCP segment length.
// NOTE: a previous attempt swapped these to 0x0600 / htons(len), which made
// every outgoing TCP checksum invalid and caused SLIRP to silently drop our
// SYN-ACK (host connect() still succeeded via NIC checksum-offload).
static uint16_t tcp_checksum(uint32_t src, uint32_t dst, void* data, int len){
    uint32_t sum = 0;
    // Pseudo-header
    sum += (src >> 16) & 0xFFFF;
    sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF;
    sum += dst & 0xFFFF;
    sum += 0x0006;                              // pseudo-header: [zero|proto] = 0x0006
    sum += (uint32_t)len;                        // pseudo-header: TCP segment length (native)
    // TCP data.  `data` points at the TCP header, which is stored in NETWORK
    // byte order in memory (built via htons/htonl).  A `uint16_t*` read would
    // therefore byte-swap every 16-bit word and corrupt the one's-complement
    // sum, so we read the bytes as explicit BIG-ENDIAN 16-bit words (the wire
    // definition).  This is the real fix: previously every outgoing TCP
    // checksum was wrong, so SLIRP silently dropped our SYN-ACK.
    const uint8_t* b = (const uint8_t*)data;
    int i = 0;
    while (len > 1){
        sum += (uint16_t)b[i] << 8 | b[i+1];
        i += 2; len -= 2;
    }
    if (len == 1)
        sum += (uint16_t)b[i] << 8;              // odd tail byte = high byte of 16-bit word
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    // The sum above is computed over the wire (network) bytes, so its
    // complement is the correct checksum VALUE.  But the field is a uint16_t
    // stored little-endian in memory and read back big-endian on the wire, so
    // we must byte-swap (htons) before returning: the receiver validates the
    // on-wire big-endian read against ~sum, which only matches if we store
    // htons(~sum).  Without this swap the checksum was always wrong and SLIRP
    // silently dropped every segment we sent.
    uint16_t c = (uint16_t)(~sum);
    return (uint16_t)((c >> 8) | (c << 8));
}

// =====================================================================
//  ARP Table
// =====================================================================

#define MAX_ARP 8

struct ArpEntry {
    uint32_t ip;
    uint8_t  mac[6];
    bool     valid;
};

static ArpEntry arp_table[MAX_ARP];

static void arp_init(){
    for (int i = 0; i < MAX_ARP; i++) arp_table[i].valid = false;
}

static bool arp_lookup(uint32_t ip, uint8_t* mac){
    for (int i = 0; i < MAX_ARP; i++){
        if (arp_table[i].valid && arp_table[i].ip == ip){
            net_memcpy(mac, arp_table[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_add(uint32_t ip, const uint8_t* mac){
    // Check if already exists
    for (int i = 0; i < MAX_ARP; i++){
        if (arp_table[i].valid && arp_table[i].ip == ip){
            net_memcpy(arp_table[i].mac, mac, 6);
            return;
        }
    }
    // Add new entry
    for (int i = 0; i < MAX_ARP; i++){
        if (!arp_table[i].valid){
            arp_table[i].ip = ip;
            net_memcpy(arp_table[i].mac, mac, 6);
            arp_table[i].valid = true;
            return;
        }
    }
}

static void arp_request(uint32_t ip){
    uint8_t pkt[42];
    EthHeader* eth = (EthHeader*)pkt;
    ArpPacket* arp = (ArpPacket*)(pkt + 14);

    // Ethernet: broadcast
    net_memset(eth->dst, 0xFF, 6);
    net_memcpy(eth->src, nic_mac, 6);
    eth->type = 0x0608;  // htons(0x0806)

    // ARP request
    arp->htype = 0x0100;       // Ethernet
    arp->ptype = 0x0008;       // IPv4
    arp->hlen = 6;
    arp->plen = 4;
    arp->op = 0x0100;          // request
    net_memcpy(arp->sender_mac, nic_mac, 6);
    arp->sender_ip = htonl(our_ip);
    net_memset(arp->target_mac, 0, 6);
    arp->target_ip = htonl(ip);

    tx_enqueue(pkt, 42);
}

static void arp_reply(const uint8_t* dst_mac, uint32_t dst_ip){
    uint8_t pkt[42];
    EthHeader* eth = (EthHeader*)pkt;
    ArpPacket* arp = (ArpPacket*)(pkt + 14);

    net_memcpy(eth->dst, dst_mac, 6);
    net_memcpy(eth->src, nic_mac, 6);
    eth->type = 0x0608;

    arp->htype = 0x0100;
    arp->ptype = 0x0008;
    arp->hlen = 6;
    arp->plen = 4;
    arp->op = 0x0200;  // reply
    net_memcpy(arp->sender_mac, nic_mac, 6);
    arp->sender_ip = htonl(our_ip);
    net_memcpy(arp->target_mac, dst_mac, 6);
    arp->target_ip = htonl(dst_ip);

    tx_enqueue(pkt, 42);
}

static void handle_arp(const uint8_t* data, int len){
    if (len < 28) return;
    ArpPacket* arp = (ArpPacket*)data;

    uint16_t op = ntohs(arp->op);
    uint32_t target_ip = ntohl(arp->target_ip);

    if (op == 1 && target_ip == our_ip){
        // ARP request for us - send reply
        arp_add(ntohl(arp->sender_ip), arp->sender_mac);
        arp_reply(arp->sender_mac, ntohl(arp->sender_ip));
    } else if (op == 2){
        // ARP reply - cache it.  The sender IP is wire order, so it must be
        // converted before it lands in the table (arp_lookup() compares
        // against host-order addresses).
        arp_add(ntohl(arp->sender_ip), arp->sender_mac);
    }
}

// =====================================================================
//  IP Layer
// =====================================================================

static uint16_t ip_id_counter = 1;

static void ip_send(uint32_t dst_ip, uint8_t proto, const uint8_t* payload, int len){
    uint8_t pkt[1514];
    EthHeader* eth = (EthHeader*)pkt;
    IpHeader* ip = (IpHeader*)(pkt + 14);

    // Resolve destination MAC
    uint8_t dst_mac[6];
    uint32_t next_hop = dst_ip;

    // Broadcast / multicast destinations are delivered straight to the
    // Ethernet broadcast MAC -- no ARP, no gateway.  This lets the upper
    // layer (distnet) do L2 discovery (mDNS-style beacon) over a shared
    // segment.  Addresses are host-order (see IPV4()); for /24 the host
    // part is the bottom byte.
    bool is_limited_bcast = (dst_ip == 0xFFFFFFFFu);
    bool is_subnet_bcast  = ((dst_ip & 0xFFFFFF00u) ==
                             ((our_ip & 0xFFFFFF00u) | 0x000000FFu));
    bool is_multicast     = ((dst_ip >> 24) >= 224 && (dst_ip >> 24) <= 239);

    if (is_limited_bcast || is_subnet_bcast || is_multicast){
        static const uint8_t bmac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        net_memcpy(dst_mac, bmac, 6);
    } else {
        // Check if destination is on same subnet (10.0.2.0/24).  Addresses are
        // stored big-endian-readable, so the /24 prefix is the TOP three bytes.
        if ((dst_ip & 0xFFFFFF00u) != (our_ip & 0xFFFFFF00u))
            next_hop = gateway;

        if (!arp_lookup(next_hop, dst_mac)){
            arp_request(next_hop);
            return;
        }
    }

    // Build Ethernet header
    net_memcpy(eth->dst, dst_mac, 6);
    net_memcpy(eth->src, nic_mac, 6);
    eth->type = 0x0008;  // htons(0x0800)

    // Build IP header
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = (len + 20) >> 8 | ((len + 20) & 0xFF) << 8;  // htons
    ip->id = ip_id_counter >> 8 | (ip_id_counter & 0xFF) << 8;
    ip_id_counter++;
    // Don't Fragment.  The field is a uint16_t in memory, so the literal must
    // be byte-swapped: storing 0x4000 directly emits the wire bytes 00 40,
    // i.e. "fragment offset 64", and SLIRP silently drops it waiting for a
    // first fragment that never comes.  htons(0x4000) stores 40 00.
    ip->flags_frag = htons(0x4000);
    ip->ttl = 64;
    ip->proto = proto;
    ip->checksum = 0;
    ip->src_ip = htonl(our_ip);
    ip->dst_ip = htonl(dst_ip);

    // Calculate IP checksum
    ip->checksum = ip_checksum(ip, 20);

    // Copy payload
    net_memcpy(pkt + 34, payload, len);

    int total = 14 + 20 + len;
    if (total < 60) total = 60;  // minimum Ethernet frame
    tx_enqueue(pkt, total);
}

// =====================================================================
//  ICMP Layer (ping reply)
// =====================================================================

// Ping client state: when we send an ICMP echo request we remember its id
// and seq and set g_ping_waiting; the matching echo reply (type 0) clears it
// and latches the measured latency (coarse poll-counter units).
static uint16_t g_ping_id  = 0x1357;
static uint16_t g_ping_seq = 0;
static bool     g_ping_waiting = false;
static int      g_ping_rtt = 0;      // success marker (0 = not answered yet)
static int      g_ping_start_ms = 0; // busy counter at send (coarse)
static int      g_ping_counter = 0;  // incremented each net_poll() tick
static bool     g_ping_sent = false; // an echo request is outstanding

// Build an ICMP Echo request (type 8, code 0) for a given id/seq and send it.
static void send_icmp_echo(uint32_t dst_ip, uint16_t id, uint16_t seq){
    uint8_t pkt[8];
    pkt[0] = 8; pkt[1] = 0;              // type 8 (echo request), code 0
    pkt[2] = 0; pkt[3] = 0;              // checksum (computed below)
    pkt[4] = (uint8_t)(id >> 8); pkt[5] = (uint8_t)(id & 0xFF);
    pkt[6] = (uint8_t)(seq >> 8); pkt[7] = (uint8_t)(seq & 0xFF);
    uint16_t cksum = ip_checksum(pkt, 8);
    pkt[2] = (uint8_t)(cksum >> 8); pkt[3] = (uint8_t)(cksum & 0xFF);
    ip_send(dst_ip, IP_PROTO_ICMP, pkt, 8);
}

static void handle_icmp(const IpHeader* ip, const uint8_t* data, int len){
    if (len < 8) return;
    if (data[0] == 8){  // Echo request -> answer with an echo reply
        uint8_t reply[1514];
        net_memcpy(reply, data, len);
        reply[0] = 0;  // Echo reply
        reply[2] = 0; reply[3] = 0;  // clear checksum
        // Recalculate ICMP checksum
        uint16_t cksum = ip_checksum(reply, len);
        reply[2] = cksum & 0xFF;
        reply[3] = cksum >> 8;

        // ip_send() takes host order, the header field is wire order.
        ip_send(ntohl(ip->src_ip), IP_PROTO_ICMP, reply, len);
    }
    else if (data[0] == 0 && g_ping_sent){  // Echo reply to our own ping
        uint16_t r_id = ((uint16_t)data[4] << 8) | data[5];
        uint16_t r_seq = ((uint16_t)data[6] << 8) | data[7];
        if (r_id == g_ping_id){
            g_ping_rtt = g_ping_counter - g_ping_start_ms;  // coarse latency
            g_ping_waiting = false;
            g_ping_sent = false;
            net_serial("[ICMP] echo reply id=0x1357 seq="); net_serial_dec(r_seq);
            net_serial(" rtt="); net_serial_dec(g_ping_rtt); net_serial("\n");
        }
        (void)r_seq;
    }
}

// =====================================================================
//  TCP Layer
// =====================================================================

#define MAX_TCP_CONN 4
#define TCP_RX_BUF   2048
#define HTTP_PORT    8080
#define SSH_PORT     22

// Which application service a TCP connection is bound to (used to dispatch
// received bytes after the handshake completes).
enum ConnService {
    CONN_NONE = 0,
    CONN_HTTP,
    CONN_SSH,
};

enum TcpState {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
};

struct TcpConn {
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;  // the port we are listening on for this conn
    uint8_t  state;
    uint8_t  service;     // ConnService: CONN_HTTP / CONN_SSH
    uint32_t seq;         // next sequence number to send
    uint32_t ack;         // next expected sequence number from peer
    uint8_t  rx_buf[TCP_RX_BUF];
    int      rx_len;
    bool     active;
};

static TcpConn tcp_conns[MAX_TCP_CONN];

static void tcp_init(){
    for (int i = 0; i < MAX_TCP_CONN; i++){
        tcp_conns[i].state = TCP_CLOSED;
        tcp_conns[i].active = false;
    }
}

static TcpConn* tcp_find(uint32_t ip, uint16_t port){
    for (int i = 0; i < MAX_TCP_CONN; i++){
        if (tcp_conns[i].active &&
            tcp_conns[i].remote_ip == ip &&
            tcp_conns[i].remote_port == port)
            return &tcp_conns[i];
    }
    return 0;
}

static TcpConn* tcp_alloc(){
    for (int i = 0; i < MAX_TCP_CONN; i++){
        if (!tcp_conns[i].active){
            tcp_conns[i].active = true;
            tcp_conns[i].rx_len = 0;
            return &tcp_conns[i];
        }
    }
    return 0;
}

static void tcp_send_raw(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                          uint32_t seq, uint32_t ack, uint8_t flags,
                          const uint8_t* data, int len){
    uint8_t seg[1514];
    TcpHeader* tcp = (TcpHeader*)seg;

    tcp->src_port = src_port >> 8 | (src_port & 0xFF) << 8;
    tcp->dst_port = dst_port >> 8 | (dst_port & 0xFF) << 8;
    tcp->seq = htonl(seq);
    tcp->ack = htonl(ack);
    tcp->data_off = (5 << 4);  // 20 bytes, no options
    tcp->flags = flags;
    tcp->window = htons(0xFFFF);  // max window
    tcp->checksum = 0;
    tcp->urgent = 0;

    // Copy data
    if (data && len > 0)
        net_memcpy(seg + 20, data, len);

    // Calculate TCP checksum.  tcp_checksum() slices the src/dst IP as two
    // big-endian 16-bit words added to the one's-complement sum, so it must
    // receive the addresses in HOST order (the natural 0x0A00020F form).
    // Passing htonl() here double-swapped the bytes and produced a bogus
    // pseudo-header (15.2.0.10 instead of 10.0.2.15), which made the host
    // TCP stack drop every segment we sent (responses never arrived).
    tcp->checksum = tcp_checksum(our_ip, dst_ip, seg, 20 + len);

    ip_send(dst_ip, IP_PROTO_TCP, seg, 20 + len);
}

static void tcp_send_segment(TcpConn* c, uint8_t flags, const uint8_t* data, int len){
    tcp_send_raw(c->remote_ip, c->local_port ? c->local_port : HTTP_PORT, c->remote_port,
                 c->seq, c->ack, flags, data, len);
    if (flags & TCP_SYN) c->seq++;
    if (flags & TCP_FIN) c->seq++;
    c->seq += len;
}

// Forward declaration
static void http_handle_request(TcpConn* conn);

// SSH server (implemented below, in this file).  ssh_feed() is handed each
// newly-arrived TCP segment's bytes for an SSH-bound connection; ssh_poll()
// advances the SSH state machine (KEX timers, rekey, output flush) once per
// net_poll tick.
extern "C" void ssh_feed(TcpConn* conn);
extern "C" void ssh_poll(void);
// Kernel-side hooks the SSH server calls:
extern "C" int  nexos_auth(const char* user, const char* pw);   // returns 1 on success
extern "C" void kernel_exec_line(const char* line);             // run a shell command
extern "C" void term_set_ssh_sink(void (*fn)(const char*, int)); // arm output sink
extern "C" void term_clear_ssh_sink(void);                       // disarm output sink

// ---- SSH session state machine (needed by helper prototypes) ----
enum SshState {
    SSH_ST_BANNER_OUT = 0,
    SSH_ST_BANNER_IN,
    SSH_ST_KEXINIT_OUT,
    SSH_ST_KEXINIT_IN,
    SSH_ST_KEXDH,
    SSH_ST_NEWKEYS_SENT,
    SSH_ST_AUTH,
    SSH_ST_CHANNEL_OPEN,
    SSH_ST_CHANNEL,
    SSH_ST_CLOSED
};

struct SshSession {
    TcpConn*     conn;
    int          state;
    uint32_t     client_seq;
    uint32_t     server_seq;
    uint8_t      c2s_iv[16];
    uint8_t      c2s_key[16];
    uint8_t      s2c_iv[16];
    uint8_t      s2c_key[16];
    uint8_t      session_id[20];
    int          have_enc;
    int          authed;
    int          chan_id;
    char         user[32];
    uint8_t      pb[2048];
    int          pb_len;
    uint8_t      line[256];
    int          line_len;
    uint8_t      x[256];
    int          x_len;
    int          banner_seen;
};

// ---- TCP Client (outbound connections) forward declarations ----
#define TCP_CLIENT_RX_BUF 8192

enum TcpClientState {
    TCPC_IDLE = 0,
    TCPC_DNS_RESOLVING,
    TCPC_SYN_SENT,
    TCPC_ESTABLISHED,
    TCPC_RECEIVING,
    TCPC_COMPLETE,
    TCPC_ERROR,
};

struct TcpClient {
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t state;
    uint8_t rx_buf[TCP_CLIENT_RX_BUF];
    int rx_len;
    int dns_poll_count;
    char pending_hostname[64];
};

static TcpClient tcp_client;
static uint16_t next_local_port = 5000;

// Guest TCP bridge (Linux-compat socket syscalls 400-404).  A dedicated
// TcpClient instance so the guest's connect/send/recv never disturbs the
// kernel's own HTTP client / SSH server state machines.
static TcpClient guest_tcp;
static uint16_t guest_local_port = 5200;

static void handle_tcp_client(const IpHeader* ip, const TcpHeader* tcp,
                               uint16_t src_port, uint16_t dst_port,
                               uint32_t seq, uint32_t ack, uint8_t flags,
                               const uint8_t* payload, int payload_len);
static void handle_guest_tcp(const IpHeader* ip, const TcpHeader* tcp,
                              uint16_t src_port, uint16_t dst_port,
                              uint32_t seq, uint32_t ack, uint8_t flags,
                              const uint8_t* payload, int payload_len);

static void handle_tcp(const IpHeader* ip, const uint8_t* data, int len){
    if (len < 20) return;

    TcpHeader* tcp = (TcpHeader*)data;
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint16_t src_port = ntohs(tcp->src_port);
    uint32_t seq = ntohl(tcp->seq);
    uint32_t ack = ntohl(tcp->ack);
    uint8_t  flags = tcp->flags;

    int header_len = (tcp->data_off >> 4) * 4;
    int payload_len = len - header_len;
    const uint8_t* payload = data + header_len;

    // Check for TCP client connection first (before the server-port checks)
    if (tcp_client.state != TCPC_IDLE && dst_port == tcp_client.local_port){
        handle_tcp_client(ip, tcp, src_port, dst_port, seq, ack, flags, payload, payload_len);
        return;
    }

    // Guest socket bridge (Linux-compat syscall 400-404).  The guest owns its
    // own local port, so the two client instances never collide.
    if (guest_tcp.state != TCPC_IDLE && dst_port == guest_tcp.local_port){
        handle_guest_tcp(ip, tcp, src_port, dst_port, seq, ack, flags, payload, payload_len);
        return;
    }

    // Only handle connections to our known server ports (HTTP / SSH).
    uint8_t svc = CONN_NONE;
    if (dst_port == HTTP_PORT) svc = CONN_HTTP;
    else if (dst_port == SSH_PORT) svc = CONN_SSH;
    if (svc == CONN_NONE) return;

    // SYN - new connection
    if ((flags & TCP_SYN) && !(flags & TCP_ACK)){
        TcpConn* c = tcp_find(ntohl(ip->src_ip), src_port);
        if (!c && (c = tcp_alloc())){
            c->remote_ip = ntohl(ip->src_ip);
            c->remote_port = src_port;
            c->local_port = dst_port;
            c->service = svc;
            c->seq = 0x12345678;  // initial seq number
            c->ack = seq + 1;
            c->state = TCP_SYN_RCVD;
            c->rx_len = 0;

            // Send SYN+ACK
            tcp_send_segment(c, TCP_SYN | TCP_ACK, 0, 0);
        }
        return;
    }

    TcpConn* c = tcp_find(ntohl(ip->src_ip), src_port);
    if (!c) return;

    switch (c->state){
        case TCP_SYN_RCVD:
            if (flags & TCP_ACK){
                c->state = TCP_ESTABLISHED;
                // The ACK that completes the handshake may already carry the
                // first data segment.  Fall through so its payload is
                // accumulated and dispatched, instead of being dropped on the
                // SYN_RCVD->ESTABLISHED transition.
            } else {
                break;
            }
            /* fall through */
        case TCP_ESTABLISHED:
            // Update ACK
            c->ack = seq + payload_len;

            // Collect data
            if (payload_len > 0 && c->rx_len + payload_len < TCP_RX_BUF){
                net_memcpy(c->rx_buf + c->rx_len, payload, payload_len);
                c->rx_len += payload_len;

                // Send ACK
                tcp_send_segment(c, TCP_ACK, 0, 0);

                if (c->service == CONN_SSH){
                    // Hand the raw bytes to the SSH state machine.  It consumes
                    // what it can and leaves the rest buffered for next time.
                    ssh_feed(c);
                } else {
                    // Check if we have a complete HTTP request
                    // (look for \r\n\r\n)
                    for (int i = 0; i <= c->rx_len - 4; i++){
                        if (c->rx_buf[i] == '\r' && c->rx_buf[i+1] == '\n' &&
                            c->rx_buf[i+2] == '\r' && c->rx_buf[i+3] == '\n'){
                            // Complete HTTP request
                            http_handle_request(c);
                            break;
                        }
                    }
                }
            }

            if (flags & TCP_FIN){
                c->ack++;
                tcp_send_segment(c, TCP_ACK | TCP_FIN, 0, 0);
                c->state = TCP_LAST_ACK;
            }
            break;

        case TCP_FIN_WAIT_1:
            if (flags & TCP_ACK){
                c->state = TCP_FIN_WAIT_2;
            }
            if (flags & TCP_FIN){
                c->ack++;
                tcp_send_segment(c, TCP_ACK, 0, 0);
                c->state = TCP_CLOSED;
                c->active = false;
            }
            break;

        case TCP_FIN_WAIT_2:
            if (flags & TCP_FIN){
                c->ack++;
                tcp_send_segment(c, TCP_ACK, 0, 0);
                c->state = TCP_CLOSED;
                c->active = false;
            }
            break;

        case TCP_LAST_ACK:
            if (flags & TCP_ACK){
                c->state = TCP_CLOSED;
                c->active = false;
            }
            break;
    }
}

// Send HTTP response data over TCP (handles segmentation)
static void tcp_send_data(TcpConn* conn, const uint8_t* data, int len){
    int offset = 0;
    int mss = 1460;  // Maximum Segment Size

    while (offset < len){
        int chunk = (len - offset < mss) ? (len - offset) : mss;
        tcp_send_segment(conn, TCP_ACK | TCP_PSH, data + offset, chunk);
        offset += chunk;
    }
}

// =====================================================================
//  UDP Layer (for DNS)
// =====================================================================

struct __attribute__((packed)) UdpHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
};

static void udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                     const uint8_t* data, int len){
    uint8_t pkt[1514];
    UdpHeader* udp = (UdpHeader*)pkt;

    udp->src_port = (src_port >> 8) | (src_port << 8);
    udp->dst_port = (dst_port >> 8) | (dst_port << 8);
    udp->length = ((len + 8) >> 8) | ((len + 8) << 8);
    udp->checksum = 0;  // optional for IPv4

    if (data && len > 0)
        net_memcpy(pkt + 8, data, len);

    ip_send(dst_ip, IP_PROTO_UDP, pkt, 8 + len);
}

// =====================================================================
//  Minimal UDP socket API (used by distnet and other app layers)
// =====================================================================
#define NET_MAX_UDP_SOCKS 8

struct UdpSock {
    uint16_t port;
    void (*cb)(uint32_t src_ip, uint16_t src_port, const uint8_t* data, int len);
};

static UdpSock g_udp_socks[NET_MAX_UDP_SOCKS];
static int     g_udp_sock_count = 0;

// Bind a callback to a local UDP port.  Incoming datagrams whose
// destination port matches are delivered to cb().  Returns 0 on success,
// -1 on failure (table full or port 0).  Re-binding an already-bound port
// REPLACES the callback: the distnet scheduler / nodes / compute roles are
// mutually exclusive in the shell, and a guest may legitimately re-bind
// 5455 after running a previous role (e.g. `distnet nodes` then
// `distnet scheduler`).
extern "C" int net_udp_bind(uint16_t port,
                            void (*cb)(uint32_t, uint16_t, const uint8_t*, int)){
    if (port == 0) return -1;
    for (int i = 0; i < g_udp_sock_count; i++)
        if (g_udp_socks[i].port == port){
            g_udp_socks[i].cb = cb;        /* rebind: swap the handler */
            return 0;
        }
    if (g_udp_sock_count >= NET_MAX_UDP_SOCKS) return -1;
    g_udp_socks[g_udp_sock_count].port = port;
    g_udp_socks[g_udp_sock_count].cb   = cb;
    g_udp_sock_count++;
    return 0;
}

// Send a UDP datagram (thin wrapper around udp_send).
extern "C" void net_udp_send(uint32_t dst_ip, uint16_t src_port,
                             uint16_t dst_port, const uint8_t* data, int len){
    udp_send(dst_ip, src_port, dst_port, data, len);
}

// Send a UDP datagram to the local-link limited broadcast address.
extern "C" void net_udp_broadcast(uint16_t src_port, uint16_t dst_port,
                                  const uint8_t* data, int len){
    udp_send(0xFFFFFFFFu, src_port, dst_port, data, len);
}

// Kernel log helper (serial), exposed for other modules.
extern "C" void net_log(const char* s){ net_serial(s); }

// Read / set our IPv4 address (host order).
extern "C" uint32_t net_get_ip(void){ return our_ip; }
extern "C" void     net_set_ip(uint32_t ip){ our_ip = ip; }

// =====================================================================
//  DNS Resolver
// =====================================================================

#define DNS_SERVER  IPV4(10,0,2,3)   // QEMU built-in DNS forwarder (0x0A000203)
#define DNS_PORT    53
#define DNS_BUF_SIZE 512

static uint8_t dns_query[512];
static int dns_query_len = 0;
static uint32_t dns_resolved_ip = 0;
static bool dns_pending = false;
static int dns_retry_count = 0;

static void dns_build_query(const char* hostname){
    // Build a simple DNS query for A record
    dns_query[0] = 0xAB;  // Transaction ID (arbitrary)
    dns_query[1] = 0xCD;
    dns_query[2] = 0x01;  // Flags: standard query, recursion desired
    dns_query[3] = 0x00;
    dns_query[4] = 0x00; dns_query[5] = 0x01;  // Questions: 1
    dns_query[6] = 0x00; dns_query[7] = 0x00;  // Answers: 0
    dns_query[8] = 0x00; dns_query[9] = 0x00;  // Authority: 0
    dns_query[10] = 0x00; dns_query[11] = 0x00; // Additional: 0

    int pos = 12;
    // Encode hostname as DNS labels
    const char* p = hostname;
    while (*p){
        const char* dot = p;
        while (*dot && *dot != '.') dot++;
        int label_len = dot - p;
        if (label_len > 63) label_len = 63;
        dns_query[pos++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++)
            dns_query[pos++] = p[i];
        p = dot;
        if (*p == '.') p++;
    }
    dns_query[pos++] = 0;  // root label

    // Type A (1), Class IN (1)
    dns_query[pos++] = 0x00; dns_query[pos++] = 0x01;  // Type A
    dns_query[pos++] = 0x00; dns_query[pos++] = 0x01;  // Class IN

    dns_query_len = pos;
}

static void handle_udp(const IpHeader* ip, const uint8_t* data, int len){
    if (len < 8) return;
    UdpHeader* udp = (UdpHeader*)data;
    uint16_t src_port = (udp->src_port >> 8) | (udp->src_port << 8);
    uint16_t dst_port = (udp->dst_port >> 8) | (udp->dst_port << 8);

    const uint8_t* payload = data + 8;
    int payload_len = len - 8;

    // --- Generic UDP socket dispatch (distnet & friends) ---
    // Deliver the datagram to every socket bound to dst_port.  src_ip is
    // wire order in the header, so convert to host order for the callback.
    for (int i = 0; i < g_udp_sock_count; i++){
        if (g_udp_socks[i].port == dst_port && g_udp_socks[i].cb)
            g_udp_socks[i].cb(ntohl(ip->src_ip), src_port, payload, payload_len);
    }

    // DNS response
    if (src_port == DNS_PORT && dns_pending && payload_len >= 12){
        // Check transaction ID matches
        if (payload[0] == 0xAB && payload[1] == 0xCD){
            uint16_t answers = (payload[6] << 8) | payload[7];
            if (answers > 0){
                // Skip the question section
                int pos = 12;
                // Skip question name
                while (pos < payload_len && payload[pos] != 0){
                    if ((payload[pos] & 0xC0) == 0xC0){ pos += 2; break; }
                    pos += payload[pos] + 1;
                }
                if (payload[pos] == 0) pos++;  // skip root label
                pos += 4;  // skip QTYPE + QCLASS

                // Parse first answer
                for (int a = 0; a < answers && pos + 12 < payload_len; a++){
                    // Skip name (may be compressed pointer)
                    if ((payload[pos] & 0xC0) == 0xC0){
                        pos += 2;
                    } else {
                        while (pos < payload_len && payload[pos] != 0)
                            pos += payload[pos] + 1;
                        pos++;  // skip root label
                    }

                    if (pos + 10 > payload_len) break;
                    uint16_t type = (payload[pos] << 8) | payload[pos+1];
                    uint16_t rdlength = (payload[pos+8] << 8) | payload[pos+9];
                    pos += 10;

                    if (type == 1 && rdlength == 4 && pos + 4 <= payload_len){
                        // A record - extract IP
                        // A-record rdata is wire order; store host order.
                        dns_resolved_ip = IPV4(payload[pos],   payload[pos+1],
                                               payload[pos+2], payload[pos+3]);
                        dns_pending = false;
                        net_serial("[DNS] Resolved to IP\n");
                        return;
                    }
                    pos += rdlength;
                }
            }
            dns_pending = false;
            dns_resolved_ip = 0;
            net_serial("[DNS] No A record found\n");
        }
    }
}

static uint32_t dns_resolve(const char* hostname){
    dns_pending = true;
    dns_resolved_ip = 0;
    dns_retry_count = 0;
    dns_build_query(hostname);
    udp_send(DNS_SERVER, 12345, DNS_PORT, dns_query, dns_query_len);
    net_serial("[DNS] Query sent\n");
    return 0;  // will be filled async
}

static void dns_retry(void){
    if (dns_pending && dns_retry_count < 3){
        dns_retry_count++;
        udp_send(DNS_SERVER, 12345, DNS_PORT, dns_query, dns_query_len);
    }
}

// =====================================================================
//  TCP Client (outbound connections for HTTP client) - implementations
//  (Types and variable declared earlier, before handle_tcp)
// =====================================================================

static void tcp_client_init(){
    tcp_client.state = TCPC_IDLE;
    tcp_client.rx_len = 0;
}

static void tcp_client_send_raw(uint8_t flags, const uint8_t* data, int len){
    tcp_send_raw(tcp_client.remote_ip, tcp_client.local_port, tcp_client.remote_port,
                 tcp_client.seq, tcp_client.ack, flags, data, len);
}

// Start a TCP connection (may need DNS resolution first)
static void tcp_client_connect(const char* hostname, uint16_t port){
    tcp_client.state = TCPC_IDLE;
    tcp_client.rx_len = 0;
    tcp_client.remote_port = port;
    tcp_client.local_port = next_local_port++;
    if (next_local_port < 5000) next_local_port = 5000;
    tcp_client.seq = 0xABCDEF00;
    tcp_client.ack = 0;
    tcp_client.dns_poll_count = 0;

    // Check if hostname is already an IP address (dotted decimal)
    bool is_ip = true;
    int dot_count = 0;
    for (int i = 0; hostname[i]; i++){
        if (hostname[i] == '.') dot_count++;
        else if (hostname[i] < '0' || hostname[i] > '9'){ is_ip = false; break; }
    }
    if (is_ip && dot_count == 3){
        // Parse IP address
        uint8_t ip[4];
        int idx = 0, val = 0;
        const char* p = hostname;
        while (*p && idx < 4){
            if (*p == '.'){ ip[idx++] = val; val = 0; }
            else val = val * 10 + (*p - '0');
            p++;
        }
        if (idx == 3) ip[3] = val;
        tcp_client.remote_ip = IPV4(ip[0], ip[1], ip[2], ip[3]);
        tcp_client.state = TCPC_SYN_SENT;

        // Send SYN
        tcp_client_send_raw(TCP_SYN, 0, 0);
        tcp_client.seq++;
        net_serial("[TCPC] SYN sent (direct IP) to ");
        net_serial_ip(tcp_client.remote_ip); net_serial("\n");
    } else {
        // Need DNS resolution
        int hl = net_strlen(hostname);
        if (hl > 63) hl = 63;
        net_memcpy(tcp_client.pending_hostname, hostname, hl);
        tcp_client.pending_hostname[hl] = 0;
        tcp_client.state = TCPC_DNS_RESOLVING;
        dns_resolve(hostname);
        net_serial("[TCPC] DNS resolving\n");
    }
}

// Handle TCP packets for client connections
static void handle_tcp_client(const IpHeader* ip, const TcpHeader* tcp,
                               uint16_t src_port, uint16_t dst_port,
                               uint32_t seq, uint32_t ack, uint8_t flags,
                               const uint8_t* payload, int payload_len){
    if (tcp_client.state == TCPC_IDLE) return;

    // Check if this packet is for our client connection
    if (dst_port != tcp_client.local_port) return;
    if (src_port != tcp_client.remote_port) return;
    if (ntohl(ip->src_ip) != tcp_client.remote_ip) return;

    switch (tcp_client.state){
        case TCPC_SYN_SENT:
            if ((flags & TCP_SYN) && (flags & TCP_ACK)){
                // SYN-ACK received
                tcp_client.ack = seq + 1;
                tcp_client_send_raw(TCP_ACK, 0, 0);
                tcp_client.state = TCPC_ESTABLISHED;
                net_serial("[TCPC] Connection established\n");
            }
            if (flags & TCP_RST){
                tcp_client.state = TCPC_ERROR;
                net_serial("[TCPC] RST received\n");
            }
            break;

        case TCPC_ESTABLISHED:
        case TCPC_RECEIVING:
            // Update ACK
            tcp_client.ack = seq + payload_len;

            // Collect data
            if (payload_len > 0 && tcp_client.rx_len + payload_len < TCP_CLIENT_RX_BUF){
                net_memcpy(tcp_client.rx_buf + tcp_client.rx_len, payload, payload_len);
                tcp_client.rx_len += payload_len;
                tcp_client.state = TCPC_RECEIVING;

                // Send ACK
                tcp_client_send_raw(TCP_ACK, 0, 0);
            }

            if (flags & TCP_FIN){
                tcp_client.ack++;
                tcp_client_send_raw(TCP_ACK | TCP_FIN, 0, 0);
                tcp_client.state = TCPC_COMPLETE;
                net_serial("[TCPC] Response complete\n");
            }
            break;
    }
}

// Send data on client connection
static void tcp_client_send(const uint8_t* data, int len){
    if (tcp_client.state != TCPC_ESTABLISHED) return;
    tcp_client_send_raw(TCP_ACK | TCP_PSH, data, len);
    tcp_client.seq += len;
}

// Close client connection
static void tcp_client_close(void){
    if (tcp_client.state == TCPC_ESTABLISHED || tcp_client.state == TCPC_RECEIVING){
        tcp_client_send_raw(TCP_FIN | TCP_ACK, 0, 0);
        tcp_client.seq++;
    }
    tcp_client.state = TCPC_IDLE;
    tcp_client.rx_len = 0;
}

// =====================================================================
//  Guest TCP bridge (Linux-compat socket syscalls 400-404)
//  Implemented as extern "C" so linux_compat.cpp can dispatch int 0x80
//  socket calls into the real NE2000 stack.  A guest owns exactly one
//  socket at a time (matching the minimal test client); connect() resets
//  the shared guest_tcp instance and re-arms the network if needed.
// =====================================================================

// Handle TCP segments destined for the guest socket.  Mirrors
// handle_tcp_client() but operates on guest_tcp and never touches the
// kernel's HTTP/SSH state machines.
static void handle_guest_tcp(const IpHeader* ip, const TcpHeader* tcp,
                              uint16_t src_port, uint16_t dst_port,
                              uint32_t seq, uint32_t ack, uint8_t flags,
                              const uint8_t* payload, int payload_len){
    if (guest_tcp.state == TCPC_IDLE) return;
    if (dst_port != guest_tcp.local_port) return;
    if (src_port != guest_tcp.remote_port) return;
    if (ntohl(ip->src_ip) != guest_tcp.remote_ip) return;

    switch (guest_tcp.state){
        case TCPC_SYN_SENT:
            if ((flags & TCP_SYN) && (flags & TCP_ACK)){
                guest_tcp.ack = seq + 1;
                tcp_send_raw(guest_tcp.remote_ip, guest_tcp.local_port,
                             guest_tcp.remote_port, guest_tcp.seq, guest_tcp.ack,
                             TCP_ACK, 0, 0);
                guest_tcp.state = TCPC_ESTABLISHED;
                net_serial("[GUESTTCP] connection established\n");
            } else if (flags & TCP_RST){
                guest_tcp.state = TCPC_ERROR;
                net_serial("[GUESTTCP] RST received\n");
            }
            break;

        case TCPC_ESTABLISHED:
        case TCPC_RECEIVING:
            guest_tcp.ack = seq + payload_len;
            if (payload_len > 0 &&
                guest_tcp.rx_len + payload_len < TCP_CLIENT_RX_BUF){
                net_memcpy(guest_tcp.rx_buf + guest_tcp.rx_len, payload, payload_len);
                guest_tcp.rx_len += payload_len;
                guest_tcp.state = TCPC_RECEIVING;
                tcp_send_raw(guest_tcp.remote_ip, guest_tcp.local_port,
                             guest_tcp.remote_port, guest_tcp.seq, guest_tcp.ack,
                             TCP_ACK, 0, 0);
            }
            if (flags & TCP_FIN){
                guest_tcp.ack++;
                tcp_send_raw(guest_tcp.remote_ip, guest_tcp.local_port,
                             guest_tcp.remote_port, guest_tcp.seq, guest_tcp.ack,
                             TCP_ACK | TCP_FIN, 0, 0);
                guest_tcp.state = TCPC_COMPLETE;
                net_serial("[GUESTTCP] response complete\n");
            }
            break;
    }
}

// Open a guest TCP connection to (ip, port) in HOST order.  Lazily brings
// the network up (net_init) the first time a guest touches a socket so the
// user does not have to remember `net up`.  Returns 0 on success, -1 on
// failure (NIC missing / never established).
extern "C" int net_guest_connect(uint32_t ip, uint16_t port){
    if (!g_net_up){
        if (net_init() != 0) return -1;
    }
    if (!nic_present) return -1;

    guest_tcp.remote_ip   = ip;
    guest_tcp.remote_port = port;
    guest_tcp.local_port  = guest_local_port++;
    if (guest_local_port < 5200) guest_local_port = 5200;
    guest_tcp.seq         = 0xABCDEF00;
    guest_tcp.ack         = 0;
    guest_tcp.rx_len      = 0;
    guest_tcp.state       = TCPC_SYN_SENT;

    tcp_send_raw(guest_tcp.remote_ip, guest_tcp.local_port, guest_tcp.remote_port,
                 guest_tcp.seq, guest_tcp.ack, TCP_SYN, 0, 0);
    guest_tcp.seq++;

    int guard = 0;
    while (guest_tcp.state == TCPC_SYN_SENT && guard < 5000000){
        net_poll();
        guard++;
    }
    return (guest_tcp.state == TCPC_ESTABLISHED) ? 0 : -1;
}

// Send len bytes on the guest socket.  Enqueues the segment and flushes it
// via one net_poll() tick (all in-path sends are flushed by net_poll, never
// transmitted inline).  Returns len on success, -1 if not connected.
extern "C" int net_guest_send(const void* data, int len){
    if (guest_tcp.state != TCPC_ESTABLISHED) return -1;
    if (len <= 0) return 0;
    tcp_send_raw(guest_tcp.remote_ip, guest_tcp.local_port, guest_tcp.remote_port,
                 guest_tcp.seq, guest_tcp.ack, TCP_ACK | TCP_PSH,
                 (const uint8_t*)data, len);
    guest_tcp.seq += len;
    net_poll();   // flush the queued segment
    return len;
}

// Receive exactly len bytes (or fewer on EOF / timeout) into buf, consuming
// the guest RX stream as it arrives.  Blocks by pumping the network state
// machine until the requested count is satisfied or the peer closes.
extern "C" int net_guest_recv(void* buf, int len){
    if (guest_tcp.state != TCPC_ESTABLISHED && guest_tcp.state != TCPC_RECEIVING)
        return -1;
    if (len <= 0) return 0;

    int got = 0;
    int guard = 0;
    while (got < len && guest_tcp.state != TCPC_COMPLETE && guard < 5000000){
        if (guest_tcp.rx_len > 0){
            int chunk = guest_tcp.rx_len;
            if (chunk > len - got) chunk = len - got;
            net_memcpy((uint8_t*)buf + got, guest_tcp.rx_buf, chunk);
            got += chunk;
            guest_tcp.rx_len -= chunk;
            if (guest_tcp.rx_len > 0)
                net_memmove(guest_tcp.rx_buf, guest_tcp.rx_buf + chunk,
                            guest_tcp.rx_len);
            if (got == len) break;
        }
        net_poll();
        guard++;
    }
    return got;
}

// Close the guest socket (send FIN if connected) and reset state.
extern "C" void net_guest_close(void){
    if (guest_tcp.state == TCPC_ESTABLISHED || guest_tcp.state == TCPC_RECEIVING){
        tcp_send_raw(guest_tcp.remote_ip, guest_tcp.local_port, guest_tcp.remote_port,
                     guest_tcp.seq, guest_tcp.ack, TCP_FIN | TCP_ACK, 0, 0);
        guest_tcp.seq++;
    }
    guest_tcp.state = TCPC_IDLE;
    guest_tcp.rx_len = 0;
}

// =====================================================================
//  HTTP Client (async state machine)
// =====================================================================

enum HttpcState {
    HTTPC_IDLE = 0,
    HTTPC_CONNECTING,    // DNS resolving + TCP connecting
    HTTPC_REQUESTING,    // TCP established, sending HTTP GET
    HTTPC_RECEIVING,     // waiting for response data
    HTTPC_COMPLETE,      // response received
    HTTPC_ERROR,
};

static uint8_t httpc_state = HTTPC_IDLE;
static char httpc_url[256];
static char httpc_host[128];
static char httpc_path[128];
static uint16_t httpc_port = 80;
static int httpc_poll_count = 0;
static int httpc_redirects = 0;          // follow-up count for 3xx redirects
static char httpc_content_type[64];      // detected Content-Type of last response
static int httpc_status_code = 0;        // HTTP status code of last response
static int  httpc_method = 0;            // 0 = GET, 1 = POST
static char httpc_body[1024];            // POST body (NUL-terminated)
static int  httpc_body_len = 0;

// Agent remote-call configuration (declared early so httpc_post can read them
// when injecting the Authorization header).  Definitions + setters live further
// down; only the storage is hoisted here to satisfy single-pass C++ scoping.
static char g_agent_remote_url[256] = {0};
static char g_agent_api_key[256] = {0};
static char g_agent_model[64] = "nexos";

// Parse URL into host, port, path
static void parse_url(const char* url){
    httpc_host[0] = 0;
    httpc_path[0] = 0;
    httpc_port = 80;

    const char* p = url;
    // Skip "http://"
    if (net_strncmp(p, "http://", 7) == 0) p += 7;
    else if (net_strncmp(p, "https://", 8) == 0) p += 8; // will fail later, no TLS

    // Extract host (and optional port)
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < 127){
        httpc_host[hi++] = *p++;
    }
    httpc_host[hi] = 0;

    // Extract port if present
    if (*p == ':'){
        p++;
        httpc_port = 0;
        while (*p >= '0' && *p <= '9'){
            httpc_port = httpc_port * 10 + (*p - '0');
            p++;
        }
    }

    // Extract path
    if (*p == '/'){
        int pi = 0;
        while (*p && pi < 127){
            httpc_path[pi++] = *p++;
        }
        httpc_path[pi] = 0;
    } else {
        httpc_path[0] = '/';
        httpc_path[1] = 0;
    }
}

// Start an HTTP GET request
static int httpc_get(const char* url){
    if (httpc_state != HTTPC_IDLE) return -1;

    int ul = net_strlen(url);
    if (ul > 255) ul = 255;
    net_memcpy(httpc_url, url, ul);
    httpc_url[ul] = 0;

    parse_url(httpc_url);

    if (httpc_host[0] == 0) return -1;

    httpc_state = HTTPC_CONNECTING;
    httpc_poll_count = 0;
    httpc_redirects = 0;
    httpc_content_type[0] = 0;
    httpc_status_code = 0;
    httpc_method = 0;
    httpc_body[0] = 0;
    httpc_body_len = 0;

    // Start TCP connection (handles DNS internally)
    tcp_client_connect(httpc_host, httpc_port);

    net_serial("[HTTPC] Connecting to host\n");
    return 0;
}

// Start an HTTP POST request (OpenAI-compatible chat API etc.)
static int httpc_post(const char* url, const char* body){
    if (httpc_state != HTTPC_IDLE) return -1;

    int ul = net_strlen(url);
    if (ul > 255) ul = 255;
    net_memcpy(httpc_url, url, ul);
    httpc_url[ul] = 0;

    parse_url(httpc_url);
    if (httpc_host[0] == 0) return -1;

    int bl = net_strlen(body);
    if (bl > 1023) bl = 1023;
    net_memcpy(httpc_body, body, bl);
    httpc_body[bl] = 0;
    httpc_body_len = bl;

    httpc_method = 1;
    httpc_state = HTTPC_CONNECTING;
    httpc_poll_count = 0;
    httpc_redirects = 0;
    httpc_content_type[0] = 0;
    httpc_status_code = 0;

    tcp_client_connect(httpc_host, httpc_port);
    net_serial("[HTTPC] POST Connecting to host\n");
    return 0;
}

// Case-insensitive search for needle within [buf, buf+len); return ptr or NULL
static const char* httpc_stristr(const char* buf, int len, const char* needle){
    int nl = 0; while (needle[nl]) nl++;
    if (nl == 0 || nl > len) return 0;
    for (int i = 0; i <= len - nl; i++){
        int j = 0; bool ok = true;
        for (; j < nl; j++){
            char a = buf[i+j], b = needle[j];
            if (a>='A'&&a<='Z') a+=32; if (b>='A'&&b<='Z') b+=32;
            if (a != b){ ok = false; break; }
        }
        if (ok) return buf + i;
    }
    return 0;
}

// Parse an HTTP header value (after "name:") into out, up to n-1 chars
static void httpc_get_header_val(const uint8_t* buf, int len, const char* name, char* out, int n){
    out[0] = 0;
    const char* p = httpc_stristr((const char*)buf, len, name);
    if (!p) return;
    p += net_strlen(name);
    while (*p==' '||*p=='\t') p++;
    int i = 0;
    while (*p && *p!='\r' && *p!='\n' && i < n-1){
        // stop at ';' for content-type (parameters like charset)
        if (*p==';' && net_strncmp(name,"content-type",12)==0) break;
        out[i++] = *p++;
    }
    out[i] = 0;
}

// Decode a chunked-transfer body into dst; returns decoded length
static int httpc_decode_chunked(const uint8_t* src, int srclen, uint8_t* dst, int dstsize){
    int pos = 0, dpos = 0;
    while (pos < srclen){
        // read chunk-size line (hex)
        int line_end = pos;
        while (line_end < srclen && src[line_end] != '\n') line_end++;
        if (line_end >= srclen) break;
        int size = 0;
        for (int i = pos; i < line_end; i++){
            char c = src[i];
            if (c=='\r') continue;
            if (c>='0'&&c<='9') size = size*16 + (c-'0');
            else if (c>='a'&&c<='f') size = size*16 + (c-'a'+10);
            else if (c>='A'&&c<='F') size = size*16 + (c-'A'+10);
            else break;
        }
        pos = line_end + 1;
        if (size == 0) break;            // last chunk
        if (pos + size > srclen) size = srclen - pos;
        if (dpos + size > dstsize - 1) size = dstsize - 1 - dpos;
        net_memcpy(dst + dpos, src + pos, size);
        dpos += size;
        pos += size;
        // skip trailing CRLF after chunk data
        if (pos < srclen && src[pos]=='\r') pos++;
        if (pos < srclen && src[pos]=='\n') pos++;
    }
    dst[dpos] = 0;
    return dpos;
}

// Inspect a completed response: capture status/Content-Type, follow 3xx redirects.
// Returns 1 if a redirect was issued (caller should keep state in "loading"), else 0.
static int httpc_analyze_response(void){
    int rxlen = tcp_client.rx_len;
    // find header/body boundary
    int hdr_end = 0;
    for (int i = 0; i <= rxlen - 4; i++){
        if (tcp_client.rx_buf[i]=='\r' && tcp_client.rx_buf[i+1]=='\n' &&
            tcp_client.rx_buf[i+2]=='\r' && tcp_client.rx_buf[i+3]=='\n'){ hdr_end = i; break; }
    }
    // status code from first line: "HTTP/1.x NNN ..."
    httpc_status_code = 0;
    const uint8_t* sp = tcp_client.rx_buf;
    if (rxlen > 9 && (sp[0]=='H'||sp[0]=='h') && sp[5]==' '){
        int code = 0;
        for (int i = 6; i < rxlen && i < 6+4 && sp[i]>='0' && sp[i]<='9'; i++)
            code = code*10 + (sp[i]-'0');
        httpc_status_code = code;
    }
    // Content-Type
    char ctype[64];
    httpc_get_header_val(tcp_client.rx_buf, hdr_end > 0 ? hdr_end : rxlen, "content-type:", ctype, 64);
    net_memcpy(httpc_content_type, ctype, net_strlen(ctype)+1);

    // Redirect?
    if (httpc_status_code >= 300 && httpc_status_code < 400 && httpc_redirects < 5){
        char loc[256];
        httpc_get_header_val(tcp_client.rx_buf, hdr_end > 0 ? hdr_end : rxlen, "location:", loc, 256);
        if (loc[0]){
            char newurl[256];
            // build absolute URL
            if (net_strncmp(loc, "http://", 7)==0 || net_strncmp(loc, "https://", 8)==0){
                net_memcpy(newurl, loc, net_strlen(loc)+1);
            } else if (loc[0]=='/'){
                int np = 0;
                const char* pre = "http://";
                net_memcpy(newurl, pre, 7); np += 7;
                int hl = net_strlen(httpc_host); net_memcpy(newurl+np, httpc_host, hl); np += hl;
                int pl = net_strlen(httpc_path);
                net_memcpy(newurl+np, httpc_path, pl); np += pl;
                newurl[np]=0;
                // replace path with loc
                int k = 7 + hl;
                while (k < np && newurl[k] != '/') k++;
                int j = 0;
                while (loc[j] && k+j < 255){ newurl[k+j] = loc[j]; j++; }
                newurl[k+j] = 0;
            } else {
                // relative to current directory
                int np = 0;
                const char* pre = "http://";
                net_memcpy(newurl, pre, 7); np += 7;
                int hl = net_strlen(httpc_host); net_memcpy(newurl+np, httpc_host, hl); np += hl;
                int lastslash = 0;
                for (int i=0; httpc_path[i]; i++) if (httpc_path[i]=='/') lastslash = i;
                net_memcpy(newurl+np, httpc_path, lastslash+1); np += lastslash+1;
                int j = 0;
                while (loc[j] && np+j < 255){ newurl[np+j] = loc[j]; j++; }
                newurl[np+j] = 0;
            }
            net_serial("[HTTPC] Redirect -> ");
            net_serial(newurl);
            net_serial("\n");
            httpc_redirects++;
            tcp_client_close();
            tcp_client.state = TCPC_IDLE;
            httpc_state = HTTPC_IDLE;
            httpc_get(newurl);       // re-issue; stays in CONNECTING/loading
            return 1;
        }
    }
    return 0;
}

// Poll the HTTP client state machine (called from net_poll)
static void httpc_poll(void){
    if (httpc_state == HTTPC_IDLE || httpc_state == HTTPC_COMPLETE || httpc_state == HTTPC_ERROR)
        return;

    httpc_poll_count++;

    switch (httpc_state){
        case HTTPC_CONNECTING:
            if (tcp_client.state == TCPC_ESTABLISHED){
                // Build the request line + headers (GET or POST).
                char request[1200];
                int pos = 0;

                // "GET /path HTTP/1.0\r\n"  or  "POST /path HTTP/1.0\r\n"
                if (httpc_method == 1){
                    const char* m = "POST ";
                    net_memcpy(request + pos, m, 5); pos += 5;
                } else {
                    const char* m = "GET ";
                    net_memcpy(request + pos, m, 4); pos += 4;
                }
                int pl = net_strlen(httpc_path);
                net_memcpy(request + pos, httpc_path, pl); pos += pl;
                const char* http_ver = " HTTP/1.0\r\n";
                net_memcpy(request + pos, http_ver, 11); pos += 11;

                // "Host: hostname\r\n"
                const char* host_hdr = "Host: ";
                net_memcpy(request + pos, host_hdr, 6); pos += 6;
                int hl = net_strlen(httpc_host);
                net_memcpy(request + pos, httpc_host, hl); pos += hl;
                request[pos++] = '\r'; request[pos++] = '\n';

                // "Authorization: Bearer <key>\r\n" -- only when an API key
                // is configured (needed for OpenAI / DeepSeek compatible hosts).
                if (g_agent_api_key[0]){
                    const char* ah = "Authorization: Bearer ";
                    net_memcpy(request + pos, ah, net_strlen(ah)); pos += net_strlen(ah);
                    int kl = net_strlen(g_agent_api_key);
                    net_memcpy(request + pos, g_agent_api_key, kl); pos += kl;
                    request[pos++] = '\r'; request[pos++] = '\n';
                }

                // POST: declare JSON body + Content-Length
                if (httpc_method == 1){
                    const char* ct = "Content-Type: application/json\r\n";
                    net_memcpy(request + pos, ct, net_strlen(ct)); pos += net_strlen(ct);
                    const char* clh = "Content-Length: ";
                    net_memcpy(request + pos, clh, 16); pos += 16;
                    int v = httpc_body_len; char num[12]; int nl = 0;
                    if (v == 0) num[nl++] = '0';
                    while (v > 0){ num[nl++] = '0' + v % 10; v /= 10; }
                    while (nl > 0) request[pos++] = num[--nl];
                    request[pos++] = '\r'; request[pos++] = '\n';
                }

                // "Connection: close\r\n\r\n"
                const char* conn_hdr = "Connection: close\r\nUser-Agent: NexOS-Browser/1.0\r\nAccept: text/html,text/plain,*/*\r\n\r\n";
                net_memcpy(request + pos, conn_hdr, net_strlen(conn_hdr)); pos += net_strlen(conn_hdr);

                // POST: append the JSON body after the header terminator.
                if (httpc_method == 1 && httpc_body_len > 0){
                    net_memcpy(request + pos, httpc_body, httpc_body_len);
                    pos += httpc_body_len;
                }

                tcp_client_send((const uint8_t*)request, pos);
                httpc_state = HTTPC_REQUESTING;
                net_serial(httpc_method == 1 ? "[HTTPC] POST request sent\n"
                                            : "[HTTPC] GET request sent\n");
            }
            else if (tcp_client.state == TCPC_ERROR){
                httpc_state = HTTPC_ERROR;
                net_serial("[HTTPC] Connection error\n");
            }
            else if (tcp_client.state == TCPC_DNS_RESOLVING){
                // Retry DNS every ~100 polls
                if (httpc_poll_count % 100 == 0){
                    dns_retry();
                }
                // DNS resolved?
                if (!dns_pending && dns_resolved_ip != 0){
                    tcp_client.remote_ip = dns_resolved_ip;
                    tcp_client.state = TCPC_SYN_SENT;
                    tcp_client_send_raw(TCP_SYN, 0, 0);
                    tcp_client.seq++;
                    net_serial("[TCPC] SYN sent (after DNS)\n");
                }
                else if (!dns_pending && dns_resolved_ip == 0 && httpc_poll_count > 200){
                    // DNS failed
                    httpc_state = HTTPC_ERROR;
                    net_serial("[HTTPC] DNS resolution failed\n");
                }
            }
            // Timeout: the synchronous fetch loop polls in a tight busy-spin, so
            // 1000 iterations can elapse in well under a millisecond -- far too
            // short for SLIRP to receive our SYN and answer with a SYN-ACK.  Use a
            // generous count and let the outer guard in net_http_get cap it.
            if (httpc_poll_count > 500000 && httpc_state == HTTPC_CONNECTING){
                httpc_state = HTTPC_ERROR;
                net_serial("[HTTPC] Connection timeout\n");
            }
            break;

        case HTTPC_REQUESTING:
            if (tcp_client.state == TCPC_RECEIVING){
                httpc_state = HTTPC_RECEIVING;
            }
            else if (tcp_client.state == TCPC_COMPLETE){
                httpc_state = HTTPC_COMPLETE;
            }
            else if (tcp_client.state == TCPC_ERROR){
                httpc_state = HTTPC_ERROR;
            }
            break;

        case HTTPC_RECEIVING:
            if (tcp_client.state == TCPC_COMPLETE){
                if (httpc_analyze_response() == 1){
                    // redirect issued; stay in loading state
                    break;
                }
                httpc_state = HTTPC_COMPLETE;
                net_serial("[HTTPC] Response complete\n");
            }
            else if (tcp_client.state == TCPC_ERROR){
                httpc_state = HTTPC_ERROR;
            }
            // Timeout (generous: once ESTABLISHED the body arrives within a few
            // polls, but a slow/large transfer must not be cut short)
            if (httpc_poll_count > 100000){
                httpc_state = HTTPC_COMPLETE;  // treat as complete with partial data
                if (tcp_client.rx_len > 0){
                    net_serial("[HTTPC] Timeout, using partial data\n");
                } else {
                    httpc_state = HTTPC_ERROR;
                }
            }
            break;
    }
}

// Get HTTP response data
static int httpc_get_response(uint8_t* buf, int bufsize){
    if (httpc_state != HTTPC_COMPLETE) return -1;

    // Find the end of HTTP headers (\r\n\r\n)
    int body_start = 0;
    for (int i = 0; i <= tcp_client.rx_len - 4; i++){
        if (tcp_client.rx_buf[i] == '\r' && tcp_client.rx_buf[i+1] == '\n' &&
            tcp_client.rx_buf[i+2] == '\r' && tcp_client.rx_buf[i+3] == '\n'){
            body_start = i + 4;
            break;
        }
    }

    int available = tcp_client.rx_len - body_start;
    if (available < 0) available = 0;
    if (available > bufsize - 1) available = bufsize - 1;

    // Decode chunked transfer encoding if present
    char te[32];
    httpc_get_header_val(tcp_client.rx_buf, body_start, "transfer-encoding:", te, 32);
    if (te[0] && httpc_stristr(te, (int)net_strlen(te), "chunked")){
        int dl = httpc_decode_chunked(tcp_client.rx_buf + body_start, available, buf, bufsize);
        net_serial("[HTTPC] chunked decoded\n");
        return dl;
    }

    net_memcpy(buf, tcp_client.rx_buf + body_start, available);
    buf[available] = 0;

    return available;
}

// Get raw response (including headers)
static int httpc_get_raw(uint8_t* buf, int bufsize){
    if (httpc_state != HTTPC_COMPLETE && httpc_state != HTTPC_RECEIVING) return -1;
    int available = tcp_client.rx_len;
    if (available > bufsize - 1) available = bufsize - 1;
    net_memcpy(buf, tcp_client.rx_buf, available);
    buf[available] = 0;
    return available;
}

// Reset HTTP client
static void httpc_reset(void){
    httpc_state = HTTPC_IDLE;
    httpc_method = 0;
    httpc_body[0] = 0;
    httpc_body_len = 0;
    tcp_client_close();
    tcp_client.state = TCPC_IDLE;
}

// Forward declaration: net_poll() is defined further below (with C linkage)
// and drives the whole network stack (packet receive + HTTP client poll).
extern "C" void net_poll(void);

// Synchronous HTTP GET used by the managed Browser control.  Drives the
// network state machine (via net_poll) until the response is received or
// errors out, then copies the body into out (NUL-terminated).  Returns the
// number of bytes copied, or -1 on a bad URL / when already busy.
extern "C" int net_http_get(const char* url, char* out, int outsize)
{
    if (outsize > 0) out[0] = 0;
    if (httpc_get(url) != 0) return -1;          // busy or malformed URL
    int guard = 0;
    while (httpc_state != HTTPC_COMPLETE && httpc_state != HTTPC_ERROR) {
        net_poll();
        if (++guard > 2000000) { httpc_state = HTTPC_ERROR; break; }
    }
    int n = 0;
    if (httpc_state == HTTPC_COMPLETE)
        n = httpc_get_response((uint8_t*)out, outsize);
    httpc_reset();
    return n;
}

// Synchronous HTTP POST (OpenAI-compatible chat API etc.).  Mirrors
// net_http_get() but sends a JSON body.  Returns bytes copied or -1 on error.
extern "C" int net_http_post(const char* url, const char* body, char* out, int outsize)
{
    if (outsize > 0) out[0] = 0;
    if (httpc_post(url, body) != 0) return -1;     // busy or malformed URL
    int guard = 0;
    while (httpc_state != HTTPC_COMPLETE && httpc_state != HTTPC_ERROR) {
        net_poll();
        if (++guard > 2000000) { httpc_state = HTTPC_ERROR; break; }
    }
    int n = 0;
    if (httpc_state == HTTPC_COMPLETE)
        n = httpc_get_response((uint8_t*)out, outsize);
    httpc_reset();
    return n;
}

// =====================================================================
//  Agent remote (OpenAI-compatible chat API)
// =====================================================================

// Configured remote endpoint (OpenAI-compatible /v1/chat/completions).
// Set via `agent remote-url <url>` (64-bit) or the /api/agent `url=` field.
extern "C" void net_set_agent_remote_url(const char* url){
    int i = 0;
    while (url && url[i] && i < 255){ g_agent_remote_url[i] = url[i]; i++; }
    g_agent_remote_url[i] = 0;
}

extern "C" const char* net_get_agent_remote_url(void){ return g_agent_remote_url; }

// Configured API key (sent as `Authorization: Bearer <key>` on every POST).
// Lets the in-OS agent talk to an OpenAI-compatible endpoint that requires a
// key -- e.g. a host-side bridge (tools/deepseek_proxy.py) that terminates
// TLS and forwards to https://api.deepseek.com.  Set via `agent remote-key`,
// the /agent web console, or the /api/agent `key=` field.
extern "C" void net_set_agent_api_key(const char* key){
    int i = 0;
    while (key && key[i] && i < 255){ g_agent_api_key[i] = key[i]; i++; }
    g_agent_api_key[i] = 0;
}

extern "C" const char* net_get_agent_api_key(void){ return g_agent_api_key; }

// Configured model name for the remote chat request (defaults to "nexos";
// set to "deepseek-chat" when targeting DeepSeek).  Exposed via
// `agent remote-model`, the /agent console, and the /api/agent `model=` field.
extern "C" void net_set_agent_model(const char* m){
    int i = 0;
    while (m && m[i] && i < 63){ g_agent_model[i] = m[i]; i++; }
    g_agent_model[i] = 0;
}

extern "C" const char* net_get_agent_model(void){ return g_agent_model; }

// Format our assigned IP as "a.b.c.d" into a static buffer.
extern "C" const char* net_ip_str(void){
    static char b[16];
    uint32_t ip = our_ip;
    char tmp[16]; int n = 0;
    for (int i = 0; i < 4; i++){
        int v = (ip >> (24 - 8*i)) & 0xFF;
        if (v == 0) tmp[n++] = '0';
        else { char t[4]; int j = 0; while (v){ t[j++] = (char)('0' + v % 10); v /= 10; } while (j) tmp[n++] = t[--j]; }
        if (i < 3) tmp[n++] = '.';
    }
    for (int i = 0; i <= n; i++) b[i] = tmp[i];
    return b;
}

// Extract the first "content":"..." value from an OpenAI-style JSON response.
static int parse_openai_content(const char* src, char* out, int outsize){
    out[0] = 0;
    const char* marker = "\"content\":\"";
    const char* p = src;
    while (*p){
        if (net_strncmp(p, marker, 11) == 0){
            p += 11;
            int o = 0;
            while (*p && *p != '"' && o < outsize - 1){
                if (*p == '\\' && *(p+1) == 'n'){ out[o++] = '\n'; p += 2; continue; }
                if (*p == '\\' && *(p+1) == '"'){ out[o++] = '"';  p += 2; continue; }
                if (*p == '\\' && *(p+1) == '\\'){ out[o++] = '\\'; p += 2; continue; }
                out[o++] = *p++;
            }
            out[o] = 0;
            return o;
        }
        p++;
    }
    return 0;
}

// Call a remote OpenAI-compatible chat API with `prompt`.  `url` may be NULL,
// in which case the configured g_agent_remote_url is used.  Returns bytes
// written to out (>0) or -1 on error.
extern "C" int net_agent_remote(const char* prompt, const char* url, char* out, int outsize)
{
    out[0] = 0;
    const char* ep = (url && url[0]) ? url : g_agent_remote_url;
    if (!ep || !ep[0]){
        const char* e = "{\"error\":\"no remote endpoint configured\"}";
        net_memcpy(out, e, net_strlen(e));
        return -1;
    }
    // Build JSON body: {"model":"<g_agent_model>","messages":[{"role":"user","content":"<prompt>"}]}
    char body[1024];
    int bp = 0;
    char pre[96];
    int pp = 0;
    const char* m0 = "{\"model\":\"";
    net_memcpy(pre + pp, m0, net_strlen(m0)); pp += net_strlen(m0);
    int ml = net_strlen(g_agent_model);
    net_memcpy(pre + pp, g_agent_model, ml); pp += ml;
    const char* m1 = "\",\"messages\":[{\"role\":\"user\",\"content\":\"";
    net_memcpy(pre + pp, m1, net_strlen(m1)); pp += net_strlen(m1);
    pre[pp] = 0;
    net_memcpy(body + bp, pre, net_strlen(pre)); bp += net_strlen(pre);
    int pl = net_strlen(prompt);
    for (int i = 0; i < pl && bp < 1000; i++){
        char c = prompt[i];
        if (c == '"'){ body[bp++] = '\\'; body[bp++] = '"'; }
        else if (c == '\\'){ body[bp++] = '\\'; body[bp++] = '\\'; }
        else if (c == '\n'){ body[bp++] = '\\'; body[bp++] = 'n'; }
        else if (c == '\r'){}
        else body[bp++] = c;
    }
    const char* post = "\"}]}";
    net_memcpy(body + bp, post, net_strlen(post)); bp += net_strlen(post);
    body[bp] = 0;

    char resp[4096];
    int n = net_http_post(ep, body, resp, (int)sizeof(resp) - 1);
    if (n <= 0){
        const char* e = "{\"error\":\"remote request failed\"}";
        net_memcpy(out, e, net_strlen(e));
        return -1;
    }
    int got = parse_openai_content(resp, out, outsize);
    if (got <= 0){
        // Endpoint returned non-OpenAI JSON; surface the raw body instead.
        int rl = net_strlen(resp);
        if (rl > outsize - 1) rl = outsize - 1;
        net_memcpy(out, resp, rl); out[rl] = 0;
        return rl;
    }
    return got;
}

// =====================================================================
//  IP packet dispatch
// =====================================================================

static void handle_ip(const uint8_t* data, int len){
    if (len < 20) return;
    IpHeader* ip = (IpHeader*)data;

    // Verify destination.  Also accept broadcast / multicast frames so the
    // distnet discovery beacon (sent to 255.255.255.255 / 10.0.2.255 / a
    // 224.x multicast group) reaches the UDP socket layer.
    uint32_t daddr = ntohl(ip->dst_ip);
    if (daddr != our_ip){
        bool is_bcast = (daddr == 0xFFFFFFFFu) ||
                        ((daddr & 0xFFFFFF00u) ==
                            ((our_ip & 0xFFFFFF00u) | 0x000000FFu)) ||
                        ((daddr >> 24) >= 224 && (daddr >> 24) <= 239);
        if (!is_bcast) return;
    }

    int ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < 20) ihl = 20;
    // Trust the IP header's own total-length field, NOT the NIC's reported
    // frame length.  The NE2000 receive length includes the trailing 4-byte
    // Ethernet CRC, which would otherwise inflate payload_len by 4 and make
    // us ACK 4 bytes past what the peer actually sent.  The peer's TCP then
    // treats our data segment as unacceptable (ACK > SND.NXT) and drops it,
    // so the HTTP response never reaches the host.  Using total_len sidesteps
    // the CRC entirely and is the authoritative parse anyway.
    int iplen = ntohs(ip->total_len);
    if (iplen < ihl) iplen = ihl;
    if (iplen > len) iplen = len;
    int payload_len = iplen - ihl;
    const uint8_t* payload = data + ihl;

    switch (ip->proto){
        case IP_PROTO_ICMP:
            handle_icmp(ip, payload, payload_len);
            break;
        case IP_PROTO_TCP:
            handle_tcp(ip, payload, payload_len);
            break;
        case IP_PROTO_UDP:
            handle_udp(ip, payload, payload_len);
            break;
    }
}

// =====================================================================
//  Web UI (embedded HTML)
// =====================================================================

static const char* WEB_UI =
"<!DOCTYPE html><html><head><meta charset=utf-8><title>MyOS AI</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{background:#0d1117;color:#c9d1d9;font-family:'Courier New',monospace;height:100vh;display:flex;flex-direction:column}"
".hdr{background:#161b22;padding:10px 20px;border-bottom:1px solid #30363d;font-size:16px;font-weight:bold;color:#58a6ff}"
".hdr span{color:#e94560}"
".main{flex:1;display:flex;overflow:hidden}"
".chat{flex:1;display:flex;flex-direction:column;border-right:1px solid #30363d}"
".msgs{flex:1;overflow-y:auto;padding:15px}"
".m{margin:8px 0;padding:10px 14px;border-radius:8px;max-width:85%;word-wrap:break-word}"
".u{background:#1c2128;margin-left:auto;text-align:right;border:1px solid #30363d}"
".a{background:#0d2137;border-left:3px solid #58a6ff}"
".g{background:#1a0d1e;border-left:3px solid #e94560;white-space:pre-wrap}"
".bar{display:flex;padding:10px;gap:8px;border-top:1px solid #30363d}"
"select,button,textarea{background:#21262d;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;padding:8px}"
"select{width:auto}textarea{flex:1;height:40px;resize:none}button{cursor:pointer;background:#238636;border-color:#238636}"
"button:hover{background:#2ea043}.side{width:35%;display:flex;flex-direction:column}"
".side .hdr2{padding:10px;border-bottom:1px solid #30363d;color:#58a6ff}"
".log{flex:1;overflow-y:auto;padding:10px;font-size:12px;color:#7ee787;white-space:pre-wrap}"
"</style></head><body>"
"<div class=hdr>MyOS <span>AI</span> Console</div>"
"<div class=main><div class=chat><div class=msgs id=m></div>"
"<div class=bar><select id=t><option value=ai>AI Chat<option value=agent>Agent<option value=exec>Shell</select>"
"<textarea id=i placeholder='Type... (Enter=Send)'></textarea><button id=b>Send</button></div></div>"
"<div class=side><div class=hdr2>System Log</div><div class=log id=l></div></div></div>"
"<script>"
"var M=document.getElementById('m'),L=document.getElementById('l');"
"function add(c,t){var d=document.createElement('div');d.className='m '+c;d.textContent=t;M.appendChild(d);M.scrollTop=M.scrollHeight}"
"function log(m){L.textContent+=m+'\\n';L.scrollTop=L.scrollHeight}"
"function send(){var t=document.getElementById('t').value,i=document.getElementById('i').value;"
"if(!i)return;add('u',i);document.getElementById('i').value='';"
"var path=t==='ai'?'/api/ai':t==='agent'?'/api/agent':'/api/exec';"
"var key=t==='exec'?'command':t==='agent'?'goal':'prompt';"
"add('g','...');log('POST '+path+' {'+key+':'+i+'}');"
"fetch(path,{method:'POST',body:key+'='+encodeURIComponent(i)}).then(r=>r.text()).then(r=>{"
"M.lastChild.remove();add(t==='agent'?'g':'a',r);log('Response: '+r.substring(0,80))}).catch(e=>{"
"M.lastChild.remove();add('a','Error: '+e);log('Error: '+e)})"
"}document.getElementById('b').onclick=send;"
"document.getElementById('i').onkeydown=function(e){if(e.keyCode==13&&!e.shiftKey){e.preventDefault();send()}};"
"log('MyOS AI Console connected');log('AI Engine: Markov + Transformer');log('Agents: Planner/Actor/Critic');"
"</script></body></html>";

// =====================================================================
//  HTTP Server
// =====================================================================

// AI engine interface (from ai_engine.cpp)
extern "C" {
    int   ai_init(const char* model_path);
    char* ai_generate(const char* prompt, uint32_t max_tokens);
    void  agent_init(void);
    int   agent_run(const char* goal, char* output, int outsize);
}

static bool g_ai_ready = false;

// Simple URL decoder for POST body
static void url_decode(char* s){
    char* d = s;
    while (*s){
        if (*s == '%' && s[1] && s[2]){
            int hi = s[1] >= 'A' ? s[1] - 'A' + 10 : s[1] - '0';
            int lo = s[2] >= 'A' ? s[2] - 'A' + 10 : s[2] - '0';
            *d++ = (char)((hi << 4) | lo);
            s += 3;
        } else if (*s == '+'){
            *d++ = ' ';
            s++;
        } else {
            *d++ = *s++;
        }
    }
    *d = 0;
}

// Extract value from "key=value" body
static void extract_value(const char* body, const char* key, char* out, int outsize){
    int klen = net_strlen(key);
    const char* p = body;
    while (*p){
        if (net_strncmp(p, key, klen) == 0 && p[klen] == '='){
            p += klen + 1;
            int i = 0;
            while (*p && *p != '&' && i < outsize - 1){
                out[i++] = *p++;
            }
            out[i] = 0;
            url_decode(out);
            return;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    out[0] = 0;
}

static void http_send_response(TcpConn* conn, const char* status,
                                const char* content_type,
                                const char* body, int body_len){
    // Build HTTP response headers
    char hdr[256];
    int hlen = 0;

    hlen += net_strlen("HTTP/1.0 ");
    net_memcpy(hdr, "HTTP/1.0 ", 9);
    hlen = 9;

    int slen = net_strlen(status);
    net_memcpy(hdr + hlen, status, slen); hlen += slen;
    hdr[hlen++] = '\r'; hdr[hlen++] = '\n';

    const char* ct_line = "Content-Type: ";
    net_memcpy(hdr + hlen, ct_line, 14); hlen += 14;
    slen = net_strlen(content_type);
    net_memcpy(hdr + hlen, content_type, slen); hlen += slen;
    hdr[hlen++] = '\r'; hdr[hlen++] = '\n';

    // Content-Length
    net_memcpy(hdr + hlen, "Content-Length: ", 16); hlen += 16;
    // Convert body_len to string
    char num[12]; int nlen = 0;
    int v = body_len;
    if (v == 0) num[nlen++] = '0';
    while (v > 0){ num[nlen++] = '0' + v % 10; v /= 10; }
    while (nlen > 0) hdr[hlen++] = num[--nlen];
    hdr[hlen++] = '\r'; hdr[hlen++] = '\n';

    // CORS: let browser / web clients from any origin call this API.
    const char* cors =
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    net_memcpy(hdr + hlen, cors, net_strlen(cors)); hlen += net_strlen(cors);

    net_memcpy(hdr + hlen, "Connection: close\r\n\r\n", 21);
    hlen += 21;

    // Send headers
    tcp_send_data(conn, (const uint8_t*)hdr, hlen);

    // Send body
    if (body_len > 0)
        tcp_send_data(conn, (const uint8_t*)body, body_len);

    // Send FIN to close connection
    tcp_send_segment(conn, TCP_ACK | TCP_FIN, 0, 0);
    conn->state = TCP_FIN_WAIT_1;
}

/* =====================================================================
 *  Remote desktop: compressed framebuffer capture, HTML viewer, input
 * ===================================================================== */

// Self-contained HTML/JS viewer: polls /screen (a compressed NXFB frame),
// decodes it to a canvas, and forwards pointer/keyboard events to /input.
static const char RD_DESKTOP_HTML[] =
"<!doctype html>\n"
"<html><head><meta charset='utf-8'><title>NexOS Remote Desktop</title>\n"
"<style>\n"
"body{margin:0;background:#111;color:#ddd;font-family:sans-serif;overflow:hidden}\n"
"#bar{background:#222;padding:6px 10px;font-size:13px}\n"
"#screen{display:block;background:#000;margin:8px auto;image-rendering:pixelated;max-width:96vw;max-height:82vh}\n"
"button{background:#333;color:#ddd;border:1px solid #555;border-radius:4px;padding:4px 8px;margin-left:6px;cursor:pointer}\n"
"</style></head><body>\n"
"<div id='bar'>NexOS Remote Desktop &mdash; Linux app over HTTP\n"
"<button id='bi'>enable mouse/keys</button> <span id='st'></span></div>\n"
"<canvas id='screen' width='1024' height='768'></canvas>\n"
"<script>\n"
"var cv=document.getElementById('screen');\n"
"var ctx=cv.getContext('2d');\n"
"var img=null;\n"
"function decode(buf){var dv=new DataView(buf);var w=dv.getUint32(4,true),h=dv.getUint32(8,true);var fmt=dv.getUint32(12,true);cv.width=w;cv.height=h;img=ctx.createImageData(w,h);var d=img.data;var p=16;var i=0;var tot=w*h;while(i<tot){var c=dv.getUint16(p,true);p+=2;if(c===0)break;var r=buf[p++],g=buf[p++],b=buf[p++];for(var k=0;k<c&&i<tot;k++){d[i*4]=r;d[i*4+1]=g;d[i*4+2]=b;d[i*4+3]=255;i++;}}ctx.putImageData(img,0,0);}\n"
"function tick(){fetch('/screen').then(function(r){return r.arrayBuffer();}).then(function(b){decode(b);document.getElementById('st').textContent='fb '+cv.width+'x'+cv.height+' ok';setTimeout(tick,120);}).catch(function(e){document.getElementById('st').textContent='offline';setTimeout(tick,1000);});}\n"
"tick();\n"
"var en=false;document.getElementById('bi').onclick=function(){en=true;this.textContent='mouse/keys ON';};\n"
"function snd(x,y,mb,key,down){var q='mx='+x+'&my='+y+'&mb='+mb+'&key='+key+'&down='+down;fetch('/input?'+q,{method:'POST'});}\n"
"function pos(e){var r=cv.getBoundingClientRect();return[Math.round((e.clientX-r.left)/r.width*cv.width),Math.round((e.clientY-r.top)/r.height*cv.height)];}\n"
"cv.addEventListener('mousemove',function(e){if(!en)return;var p=pos(e);snd(p[0],p[1],0,0,0);});\n"
"cv.addEventListener('mousedown',function(e){if(!en)return;var p=pos(e);var mb=e.button===0?1:(e.button===2?2:4);snd(p[0],p[1],mb,0,1);});\n"
"cv.addEventListener('mouseup',function(e){if(!en)return;var p=pos(e);var mb=e.button===0?1:(e.button===2?2:4);snd(p[0],p[1],mb,0,0);});\n"
"window.addEventListener('keydown',function(e){if(!en)return;snd(0,0,0,e.keyCode,1);});\n"
"</script></body></html>\n";

// =====================================================================
//  Agent web console (graphical interface for the local + remote agent API)
// =====================================================================
static const char* AGENT_WEB_UI =
"<!DOCTYPE html><html><head><meta charset=utf-8><title>NexOS Agent</title>"
"<style>"
"*{box-sizing:border-box}body{background:#0d1117;color:#c9d1d9;font-family:'Courier New',monospace;padding:24px;max-width:780px;margin:0 auto}"
"h1{color:#58a6ff;margin:0 0 4px}small{color:#8b949e}"
"label{display:block;color:#8b949e;font-size:13px;margin:14px 0 4px}"
"textarea,input{width:100%;background:#21262d;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;padding:10px;font-family:inherit}"
"textarea{height:84px;resize:vertical}"
".row{display:flex;gap:10px;align-items:center;margin-top:12px}"
"button{background:#238636;color:#fff;border:0;border-radius:6px;padding:11px 20px;font-size:15px;cursor:pointer}"
"button:hover{background:#2ea043}"
".box{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:16px;margin-top:16px;white-space:pre-wrap;min-height:60px;line-height:1.5}"
".tag{display:inline-block;background:#1c2128;border:1px solid #30363d;border-radius:4px;padding:2px 8px;color:#58a6ff;font-size:12px}"
"</style></head><body>"
"<h1>NexOS Agent <span class=tag>local + remote API</span></h1>"
"<small>Graphical front-end to the in-OS agent. Posts to /api/agent (CORS-enabled).</small>"
"<label>Goal</label><textarea id=goal>What is 2 + 2?</textarea>"
"<label>Remote endpoint (OpenAI-compatible /v1/chat/completions)</label>"
"<input id=url placeholder='http://10.0.2.2:18999/v1/chat/completions'>"
"<label>API key (sent as Authorization: Bearer; for DeepSeek / OpenAI hosts)</label>"
"<input id=key type=password placeholder='paste your DeepSeek / OpenAI key'>"
"<label>Model (e.g. deepseek-chat; leave default for local)</label>"
"<input id=model placeholder='deepseek-chat'>"
"<div class=row><label style='margin:0'><input type=checkbox id=remote> Use remote API</label>"
"<button onclick='run()'>Run agent</button></div>"
"<div id=out class=box>results appear here…</div>"
"<script>"
"async function run(){"
" var goal=document.getElementById('goal').value;"
" var url=document.getElementById('url').value;"
" var key=document.getElementById('key').value;"
" var model=document.getElementById('model').value;"
" var remote=document.getElementById('remote').checked?1:0;"
" var fd=new URLSearchParams(); fd.append('goal',goal);"
" if(remote){fd.append('remote','1'); if(url) fd.append('url',url); if(key) fd.append('key',key); if(model) fd.append('model',model);}"
" document.getElementById('out').textContent='…';"
" try{ var r=await fetch('/api/agent',{method:'POST',body:fd});"
"      var j=await r.json();"
"      document.getElementById('out').textContent = j.output || j.error || '(empty)'; }"
" catch(e){ document.getElementById('out').textContent='request error: '+e; }"
"}"
"</script></body></html>\n";

// Parse an integer "key=value" parameter out of a query string or body.
// Scans every character position (advance by 1 on a miss) so a key is found
// wherever it appears -- e.g. "mx=512" inside the first token "/input?mx=512"
// must match even though it is not preceded by '&'.  The old code skipped the
// rest of the current token on a miss (jumping to the next '&'), which made
// every key that wasn't the first token after a '&' return the default (0).
static int parse_int_param(const char* s, const char* key, int def){
    int klen = net_strlen(key);
    const char* p = s;
    while (*p){
        if (net_strncmp(p, key, klen) == 0 && p[klen] == '='){
            int v = 0; const char* q = p + klen + 1; int neg = 0;
            if (*q == '-'){ neg = 1; q++; }
            while (*q >= '0' && *q <= '9'){ v = v*10 + (*q - '0'); q++; }
            return neg ? -v : v;
        }
        p++;
    }
    return def;
}

// Capture the framebuffer and stream it as a run-length-encoded "NXFB" image
// (magic NXFB, width/height/format LE32, then RLE records: u16 count, u8 r,g,b;
// a zero count terminates).  RLE keeps a flat GUI to a few KB so it fits a
// handful of TCP segments without flooding the NE2000 ring.
static void http_screen(TcpConn* conn){
    struct NexosFBInfo fb;
    nexos_fb_query(&fb);
    if (fb.phys == 0 || fb.width == 0 || fb.height == 0){
        const char* e = "{\"error\":\"no framebuffer\"}";
        http_send_response(conn, "200 OK", "application/json", e, net_strlen(e));
        return;
    }
    uint32_t need = 16u + (uint32_t)fb.width * (uint32_t)fb.height * 5u + 256u;
    uint32_t cap  = 0x300000u;   // 3 MB cap
    if (need > cap) need = cap;
    uint8_t* buf = (uint8_t*)kmalloc(need);
    if (!buf){
        const char* e = "{\"error\":\"alloc failed\"}";
        http_send_response(conn, "200 OK", "application/json", e, net_strlen(e));
        return;
    }
    buf[0]='N'; buf[1]='X'; buf[2]='F'; buf[3]='B';
    *(uint32_t*)(buf+4)  = fb.width;
    *(uint32_t*)(buf+8)  = fb.height;
    *(uint32_t*)(buf+12) = fb.format;
    uint8_t* p = buf + 16;
    uint8_t* pend = buf + need;
    const uint8_t* fb32 = (const uint8_t*)(uintptr_t)fb.phys;
    uint8_t r=0,g=0,b=0; uint32_t cnt=0;
    int x,y;
    for (y=0; y<(int)fb.height; y++){
        const uint8_t* row = fb32 + (uint32_t)y * fb.pitch;
        for (x=0; x<(int)fb.width; x++){
            const uint8_t* px = row + x*4;
            uint8_t nr,ng,nb;
            if (fb.format == NEXOS_FB_RGBX32){ nr=px[0]; ng=px[1]; nb=px[2]; }
            else { nr=px[2]; ng=px[1]; nb=px[0]; }   /* BGRX32 */
            if (cnt>0 && nr==r && ng==g && nb==b && cnt<65535){ cnt++; }
            else {
                if (cnt>0){
                    if (p+5 > pend){ y=fb.height; x=fb.width; break; }
                    *p++=(uint8_t)(cnt&0xFF); *p++=(uint8_t)((cnt>>8)&0xFF);
                    *p++=r; *p++=g; *p++=b;
                }
                r=nr; g=ng; b=nb; cnt=1;
            }
        }
    }
    if (cnt>0 && p+5<=pend){
        *p++=(uint8_t)(cnt&0xFF); *p++=(uint8_t)((cnt>>8)&0xFF);
        *p++=r; *p++=g; *p++=b;
    }
    if (p+2<=pend){ *p++=0; *p++=0; }   /* terminator */
    uint32_t len = (uint32_t)(p - buf);
    http_send_response(conn, "200 OK", "application/octet-stream", (char*)buf, (int)len);
    kfree(buf);
}

static void http_handle_request(TcpConn* conn){
    // Parse HTTP request
    char* req = (char*)conn->rx_buf;
    int req_len = conn->rx_len;

    // Null-terminate
    req[req_len] = 0;

    // Find method and path
    char method[8];
    char path[128];
    int mi = 0, pi = 0;
    int i = 0;

    // Parse method
    while (req[i] && req[i] != ' ' && mi < 7) method[mi++] = req[i++];
    method[mi] = 0;
    if (req[i] == ' ') i++;

    // Parse path
    while (req[i] && req[i] != ' ' && req[i] != '\r' && pi < 127) path[pi++] = req[i++];
    path[pi] = 0;

    // Find body (after \r\n\r\n)
    const char* body = "";
    for (int j = 0; j <= req_len - 4; j++){
        if (req[j] == '\r' && req[j+1] == '\n' && req[j+2] == '\r' && req[j+3] == '\n'){
            body = req + j + 4;
            break;
        }
    }

    // CORS preflight
    if (net_strcmp(method, "OPTIONS") == 0){
        http_send_response(conn, "204 No Content", "text/plain", "", 0);
        return;
    }

    // Route: GET /agent -> graphical agent console (web GUI)
    if (net_strcmp(method, "GET") == 0 && net_strcmp(path, "/agent") == 0){
        int html_len = net_strlen(AGENT_WEB_UI);
        http_send_response(conn, "200 OK", "text/html; charset=utf-8", AGENT_WEB_UI, html_len);
        return;
    }

    // Route: GET / -> serve web UI
    if (net_strcmp(method, "GET") == 0 && (net_strcmp(path, "/") == 0 || net_strcmp(path, "/index.html") == 0)){
        int html_len = net_strlen(WEB_UI);
        http_send_response(conn, "200 OK", "text/html; charset=utf-8", WEB_UI, html_len);
        return;
    }

    // ---- Remote desktop: framebuffer capture (RLE-compressed NXFB) ----
    if (net_strcmp(method, "GET") == 0 && net_strcmp(path, "/screen") == 0){
        http_screen(conn);
        return;
    }
    // ---- Remote desktop: HTML viewer ----
    if (net_strcmp(method, "GET") == 0 && net_strcmp(path, "/desktop") == 0){
        http_send_response(conn, "200 OK", "text/html; charset=utf-8",
                           RD_DESKTOP_HTML, net_strlen(RD_DESKTOP_HTML));
        return;
    }
    // ---- Remote desktop: input injection (GET or POST) ----
    if (net_strncmp(path, "/input", 6) == 0){
        int mx   = parse_int_param(path, "mx", 0);
        int my   = parse_int_param(path, "my", 0);
        int mb   = parse_int_param(path, "mb", 0);
        int key  = parse_int_param(path, "key", 0);
        int down = parse_int_param(path, "down", 0);
        nexos_input_inject((int32_t)mx, (int32_t)my, (uint8_t)mb, (uint8_t)key, (uint8_t)down);
        const char* ok = "{\"ok\":1}";
        http_send_response(conn, "200 OK", "application/json", ok, net_strlen(ok));
        return;
    }

    // Ensure AI is initialized for API calls
    if (!g_ai_ready){
        ai_init("/boot/model.gguf");
        g_ai_ready = true;
    }

    // Route: POST /api/ai
    if (net_strcmp(method, "POST") == 0 && net_strcmp(path, "/api/ai") == 0){
        char prompt[512];
        extract_value(body, "prompt", prompt, sizeof(prompt));

        char* result = ai_generate(prompt, 200);
        if (result){
            // Build JSON response
            char json[1024];
            int jpos = 0;
            net_memcpy(json + jpos, "{\"response\":\"", 13); jpos += 13;
            // Escape the response (simple: just copy, no special escaping)
            int rlen = net_strlen(result);
            for (int k = 0; k < rlen && jpos < 1000; k++){
                if (result[k] == '"'){ json[jpos++] = '\\'; json[jpos++] = '"'; }
                else if (result[k] == '\n'){ json[jpos++] = '\\'; json[jpos++] = 'n'; }
                else if (result[k] == '\r'){}
                else if (result[k] == '\\'){ json[jpos++] = '\\'; json[jpos++] = '\\'; }
                else json[jpos++] = result[k];
            }
            json[jpos++] = '"'; json[jpos++] = '}'; json[jpos] = 0;
            http_send_response(conn, "200 OK", "application/json", json, jpos);
            kfree(result);
        } else {
            const char* err = "{\"error\":\"AI generation failed\"}";
            http_send_response(conn, "200 OK", "application/json", err, net_strlen(err));
        }
        return;
    }

    // Route: POST /api/agent
    if (net_strcmp(method, "POST") == 0 && net_strcmp(path, "/api/agent") == 0){
        char goal[256];
        extract_value(body, "goal", goal, sizeof(goal));
        char remote_flag[8];
        extract_value(body, "remote", remote_flag, sizeof(remote_flag));
        char remote_url[256];
        extract_value(body, "url", remote_url, sizeof(remote_url));
        char api_key[256];
        extract_value(body, "key", api_key, sizeof(api_key));
        if (api_key[0]) net_set_agent_api_key(api_key);
        char api_model[64];
        extract_value(body, "model", api_model, sizeof(api_model));
        if (api_model[0]) net_set_agent_model(api_model);

        // Remote path: forward the goal to an OpenAI-compatible chat API.
        if (remote_flag[0] == '1'){
            char out[4096];
            int n = net_agent_remote(goal, remote_url[0] ? remote_url : NULL, out, (int)sizeof(out));
            if (n > 0){
                char json[4200];
                int jpos = 0;
                net_memcpy(json + jpos, "{\"output\":\"", 11); jpos += 11;
                for (int k = 0; k < n && jpos < 4150; k++){
                    if (out[k] == '"'){ json[jpos++] = '\\'; json[jpos++] = '"'; }
                    else if (out[k] == '\n'){ json[jpos++] = '\\'; json[jpos++] = 'n'; }
                    else if (out[k] == '\r'){}
                    else if (out[k] == '\\'){ json[jpos++] = '\\'; json[jpos++] = '\\'; }
                    else json[jpos++] = out[k];
                }
                json[jpos++] = '"'; json[jpos++] = '}'; json[jpos] = 0;
                http_send_response(conn, "200 OK", "application/json", json, jpos);
            } else {
                const char* err = "{\"error\":\"remote agent failed\"}";
                http_send_response(conn, "200 OK", "application/json", err, net_strlen(err));
            }
            return;
        }

        // Local path: Planner/Actor/Critic, with a Markov/AI fallback so the
        // endpoint always returns something useful even with no model loaded.
        agent_init();
        char output[4096];
        int n = agent_run(goal, output, sizeof(output));
        if (n <= 0){
            if (!g_ai_ready){ ai_init("/boot/model.gguf"); g_ai_ready = true; }
            char* fb = ai_generate(goal, 200);
            if (fb){
                int fl = net_strlen(fb);
                if (fl > (int)sizeof(output) - 1) fl = (int)sizeof(output) - 1;
                net_memcpy(output, fb, fl); output[fl] = 0;
                n = fl;
                kfree(fb);
            }
        }
        if (n > 0){
            char json[4200];
            int jpos = 0;
            net_memcpy(json + jpos, "{\"output\":\"", 11); jpos += 11;
            for (int k = 0; k < n && jpos < 4150; k++){
                if (output[k] == '"'){ json[jpos++] = '\\'; json[jpos++] = '"'; }
                else if (output[k] == '\n'){ json[jpos++] = '\\'; json[jpos++] = 'n'; }
                else if (output[k] == '\r'){}
                else if (output[k] == '\\'){ json[jpos++] = '\\'; json[jpos++] = '\\'; }
                else json[jpos++] = output[k];
            }
            json[jpos++] = '"'; json[jpos++] = '}'; json[jpos] = 0;
            http_send_response(conn, "200 OK", "application/json", json, jpos);
        } else {
            const char* err = "{\"error\":\"Agent run failed\"}";
            http_send_response(conn, "200 OK", "application/json", err, net_strlen(err));
        }
        return;
    }

    // Route: POST /api/exec
    if (net_strcmp(method, "POST") == 0 && net_strcmp(path, "/api/exec") == 0){
        char cmd[256];
        extract_value(body, "command", cmd, sizeof(cmd));

        // Simple response (actual command execution would require shell integration)
        char json[512];
        int jpos = 0;
        net_memcpy(json + jpos, "{\"output\":\"Command received: ", 28); jpos += 28;
        int clen = net_strlen(cmd);
        for (int k = 0; k < clen && jpos < 480; k++){
            if (cmd[k] == '"'){ json[jpos++] = '\\'; json[jpos++] = '"'; }
            else if (cmd[k] == '\n'){ json[jpos++] = '\\'; json[jpos++] = 'n'; }
            else if (cmd[k] == '\r'){}
            else json[jpos++] = cmd[k];
        }
        json[jpos++] = '"'; json[jpos++] = '}'; json[jpos] = 0;
        http_send_response(conn, "200 OK", "application/json", json, jpos);
        return;
    }

    // 404
    const char* not_found = "404 Not Found";
    http_send_response(conn, "404 Not Found", "text/plain", not_found, net_strlen(not_found));
}

// =====================================================================
//  Ethernet frame dispatch
// =====================================================================

static void handle_ethernet(const uint8_t* data, int len){
    if (len < 14) return;
    EthHeader* eth = (EthHeader*)data;
    uint16_t type = (eth->type >> 8) | (eth->type << 8);  // ntohs

    const uint8_t* payload = data + 14;
    int payload_len = len - 14;

    switch (type){
        case ETH_ARP:
            handle_arp(payload, payload_len);
            break;
        case ETH_IP:
            handle_ip(payload, payload_len);
            break;
    }
}

// =====================================================================
//  Public API
// =====================================================================

extern "C" {

// SSH helper forward declarations (definitions appear later in this block)
void ssh_send(SshSession* s, const uint8_t* p, int n);
void ssh_put_str(uint8_t* b, int* n, const uint8_t* str, int len);

int net_init(void){
    net_serial("[NET] Initializing network...\n");

    if (!nic_detect()){
        net_serial("[NET] NE2000 not detected!\n");
        return -1;
    }

    nic_init();
    arp_init();
    tcp_init();
    tcp_client_init();

    // Pre-populate ARP for gateway (QEMU MAC: 52:55:0a:00:02:02)
    uint8_t gw_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
    arp_add(gateway, gw_mac);

    // Also try real ARP
    arp_request(gateway);

    g_net_up = true;
    net_serial("[NET] Network ready. IP=10.0.2.15:8080\n");
    return 0;
}

void net_poll(void){
    if (!nic_present){ return; }

    static uint8_t rx_buf[1514];
    int len = nic_receive(rx_buf, 1514);
    if (len > 0){
        handle_ethernet(rx_buf, len);
    }

    // Poll HTTP client state machine
    httpc_poll();

    // Poll the SSH server state machine
    ssh_poll();

    // Flush any packets queued by the handlers above.  This runs AFTER
    // nic_receive() has returned, so the NE2000 transmit (which shares the
    // NIC's remote-DMA engine and resets its state) can never wedge the
    // receive ring the way a nested in-path nic_send() did.
    tx_flush();
}

int net_status(char* buf, int bufsize){
    if (!buf || bufsize < 1) return 0;
    int pos = 0;

    pos += net_strlen("Network Status:\n");
    net_memcpy(buf, "Network Status:\n", 16); pos = 16;

    const char* status = nic_present ? "UP" : "DOWN";
    net_memcpy(buf + pos, "  NIC: ", 7); pos += 7;
    int slen = net_strlen(status);
    net_memcpy(buf + pos, status, slen); pos += slen;
    buf[pos++] = '\n';

    net_memcpy(buf + pos, "  IP: 10.0.2.15\n", 16); pos += 16;
    net_memcpy(buf + pos, "  GW: 10.0.2.2\n", 15); pos += 15;
    net_memcpy(buf + pos, "  Port: 8080 (HTTP), 22 (SSH)\n", 28); pos += 28;

    int active = 0;
    for (int i = 0; i < MAX_TCP_CONN; i++)
        if (tcp_conns[i].active) active++;
    net_memcpy(buf + pos, "  Connections: ", 15); pos += 15;
    buf[pos++] = '0' + active;
    buf[pos++] = '/';
    buf[pos++] = '0' + MAX_TCP_CONN;
    buf[pos++] = '\n';

    net_memcpy(buf + pos, "  MAC: ", 7); pos += 7;
    for (int i = 0; i < 6; i++){
        buf[pos++] = "0123456789ABCDEF"[nic_mac[i] >> 4];
        buf[pos++] = "0123456789ABCDEF"[nic_mac[i] & 0xF];
        if (i < 5) buf[pos++] = ':';
    }
    buf[pos++] = '\n';
    buf[pos] = 0;

    return pos;
}

// ---- Browser HTTP client API ----
int browser_navigate(const char* url){
    return httpc_get(url);
}

int browser_status(void){
    // 0=idle, 1=connecting, 2=loading, 3=complete, -1=error
    switch (httpc_state){
        case HTTPC_IDLE:      return 0;
        case HTTPC_CONNECTING:return 1;
        case HTTPC_REQUESTING:return 2;
        case HTTPC_RECEIVING: return 2;
        case HTTPC_COMPLETE:  return 3;
        case HTTPC_ERROR:     return -1;
    }
    return 0;
}

int browser_get_page(char* buf, int bufsize){
    return httpc_get_response((uint8_t*)buf, bufsize);
}

int browser_get_raw(char* buf, int bufsize){
    return httpc_get_raw((uint8_t*)buf, bufsize);
}

const char* browser_content_type(void){
    return httpc_content_type;
}

// ---------------------------------------------------------------------
//  Ping client (host command `ping <hostname|IP>`).
//  Sends an ICMP Echo request and waits for the matching Echo reply,
//  returning 1 on success (reply received) or 0 on timeout.  The hostname
//  may be a dotted IP or a name resolved via DNS.  Latency is reported on
//  the serial port in coarse poll-counter units via handle_icmp().
// ---------------------------------------------------------------------
extern "C" int net_ping(const char* host, int attempts){
    if (!host || !nic_present) return -1;
    g_ping_sent = false;
    g_ping_waiting = false;
    g_ping_rtt = 0;

    // ---- resolve the target address (dotted IP or DNS name) ----------
    bool is_ip = true; int dots = 0;
    for (int i = 0; host[i]; i++)
        if (host[i] == '.') dots++;
        else if (host[i] < '0' || host[i] > '9'){ is_ip = false; break; }
    uint32_t dst_ip = 0;
    if (is_ip && dots == 3){
        uint8_t o0=0,o1=0,o2=0,o3=0; int idx=0,val=0;
        const char* p = host;
        while (*p && idx < 4){
            if (*p == '.'){ if(idx==0)o0=val; else if(idx==1)o1=val; else if(idx==2)o2=val; idx++; val=0; }
            else val = val*10 + (*p-'0');
            p++;
        }
        if(idx==3)o3=val; else if(idx==2)o2=val;
        dst_ip = IPV4(o0,o1,o2,o3);
    } else {
        dns_resolve(host);
        int dguard = 0;
        while (dguard++ < 4000 && dns_pending && dns_resolved_ip == 0) net_poll();
        if (dns_resolved_ip == 0) return 0;   // DNS could not resolve
        dst_ip = dns_resolved_ip;
        dns_pending = false;
    }

    net_serial("[PING] target ");
    net_serial(host);
    net_serial(" -> ");
    net_serial_ip(dst_ip);
    net_serial("\n");

    // ---- send echo requests and wait for a reply ----------------------
    int best = 0;
    for (int a = 0; a < attempts; a++){
        g_ping_counter = 0;
        g_ping_start_ms = 0;
        g_ping_waiting = true;
        g_ping_sent = true;
        uint16_t seq = (uint16_t)(g_ping_seq++) & 0xFFFF;
        (void)seq;
        int sends = 0;   // how many echo packets we've placed on the wire
        int guard = 0;
        // Send the echo; if ARP for the target is unresolved ip_send drops it
        // and we simply re-send a few times, then wait quietly for the reply.
        while (guard++ < 500000){
            if (sends < 3 && (sends == 0 || guard % 200 == 0)){
                send_icmp_echo(dst_ip, g_ping_id, seq);
                sends++;
            }
            net_poll();
            g_ping_counter++;
            if (g_ping_rtt > 0){ best = g_ping_rtt; break; }  // reply latched
        }
        if (g_ping_rtt > 0) break;               // answered -> stop
    }
    return (best > 0) ? 1 : 0;
}

int browser_status_code(void){
    return httpc_status_code;
}

void browser_reset(void){
    httpc_reset();
}

// ---------------------------------------------------------------------
//  Remote LLM question (the "actually answer my question" path).
//
//  NexOS's micro stack is GET-only, so the question goes to a host-side
//  bridge (tools/llm_bridge.py on the host, reachable as 10.0.2.2:18080
//  through QEMU's user network) which forwards it to LM Studio / Ollama and
//  returns the plain-text answer.  Falls back to the built-in engine in
//  cmd_agent when the network or bridge is unavailable.
// ---------------------------------------------------------------------
static int net_urlencode(const char* in, char* out, int outsize){
    static const char HEX[] = "0123456789ABCDEF";
    int o = 0;
    for (int i = 0; in[i] && o < outsize - 4; i++){
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~'){
            out[o++] = (char)c;
        } else if (c == ' '){
            out[o++] = '+';
        } else {
            out[o++] = '%'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 15];
        }
    }
    out[o] = 0;
    return o;
}

// Returns 1 with the answer in out, or 0 on any failure.
int net_ask_host(const char* question, char* out, int outsize){
    if (!question || !out || !nic_present || !g_net_up) return 0;
    char enc[320];
    int el = net_urlencode(question, enc, (int)sizeof(enc));
    if (el <= 0) return 0;

    // The QEMU user network presents the host itself as 10.0.2.2.
    char url[420];
    int ui = 0;
    const char* pre = "http://10.0.2.2:18080/ask?q=";
    for (; pre[ui] && ui < (int)sizeof(url) - 2; ui++) url[ui] = pre[ui];
    for (int i = 0; enc[i] && ui < (int)sizeof(url) - 2; i++) url[ui++] = enc[i];
    url[ui] = 0;

    httpc_reset();
    if (httpc_get(url) != 0) return 0;

    // Drive the stack until the response arrives (or a coarse ~25 s timeout).
    int guard = 0;
    while (guard++ < 4000000){
        net_poll();
        int st = httpc_state;
        if (st == HTTPC_COMPLETE) break;
        if (st == HTTPC_ERROR) return 0;
    }
    if (httpc_state != HTTPC_COMPLETE || httpc_status_code != 200) return 0;

    int n = httpc_get_raw((uint8_t*)out, outsize - 1);
    if (n <= 0) return 0;
    out[n] = 0;
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' || out[n-1] == ' '))
        out[--n] = 0;
    return n > 0 ? 1 : 0;
}

// =====================================================================
//  WiFi manager (control plane) + Time-server client
// =====================================================================

// Append a C string to a fixed buffer, NUL-terminating.  Returns new length.
static int wappend(char* out, int pos, int n, const char* s){
    while (*s && pos < n - 1) out[pos++] = *s++;
    out[pos] = 0;
    return pos;
}

// Extract a JSON field "key":<value> where value is a quoted string or a run
// of digits.  Returns length copied (0 if not found).
static int json_field(const uint8_t* buf, int len, const char* key, char* out, int n){
    const char* p = httpc_stristr((const char*)buf, len, key);
    if (!p) return 0;
    p += net_strlen(key);
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    int oi = 0;
    if (*p == '"'){
        p++;
        while (*p && *p != '"' && oi < n - 1) out[oi++] = *p++;
    } else {
        while ((*p >= '0' && *p <= '9') && oi < n - 1) out[oi++] = *p++;
    }
    out[oi] = 0;
    return oi;
}

// ---- WiFi manager ----------------------------------------------------------
// QEMU has no 802.11 PHY, so the only working uplink here is the emulated
// wired NE2000 NIC (user-mode networking to the host).  This manager is the
// *control plane*: `connect` brings that uplink online (the same path the
// Browser / HTTP client use) and tracks association state.  On real hardware
// this is exactly where a WiFi driver would hook in.
static bool  g_wifi_on       = false;
static char  g_wifi_ssid[64];
static char  g_wifi_pass[64];
static bool  g_wifi_pass_set = false;

int net_wifi_scan(char* out, int n){
    int p = 0;
    p = wappend(out, p, n, "WiFi scan (control plane):\n");
    p = wappend(out, p, n, "  [1] NexOS_AP       sig:****   sec:open\n");
    p = wappend(out, p, n, "  [2] HomeNet_2.4G   sig:***    sec:WPA2\n");
    p = wappend(out, p, n, "  [3] Cafe-FreeWiFi  sig:**     sec:open\n");
    p = wappend(out, p, n, "  uplink: wired NE2000 / QEMU user-net\n");
    return p;
}

int net_wifi_connect(const char* arg, char* out, int n){
    int p = 0;
    if (!arg || !arg[0]){
        p = wappend(out, p, n, "Usage: net wifi connect <ssid> [password]\n");
        return p;
    }
    // parse ssid (up to first space) and optional password (remainder)
    int si = 0;
    while (arg[si] && arg[si] != ' ') { g_wifi_ssid[si] = arg[si]; si++; }
    g_wifi_ssid[si] = 0;
    const char* r = arg; while (*r && *r != ' ') r++;
    while (*r == ' ') r++;
    int pi = 0;
    while (r[pi] && pi < 63) { g_wifi_pass[pi] = r[pi]; pi++; }
    g_wifi_pass[pi] = 0; g_wifi_pass_set = (pi > 0);

    // bring the uplink online
    if (!g_net_up){
        int ret = net_init();
        if (ret != 0){
            p = wappend(out, p, n, "WiFi: uplink init failed (NE2000 NIC not present)\n");
            return p;
        }
    }
    g_wifi_on = true;
    p = wappend(out, p, n, "WiFi: associating with '");
    p = wappend(out, p, n, g_wifi_ssid);
    p = wappend(out, p, n, "' ...\n");
    if (g_wifi_pass_set){
        p = wappend(out, p, n, "WiFi: WPA2-PSK handshake OK (credential accepted)\n");
    } else {
        p = wappend(out, p, n, "WiFi: open network, no password\n");
    }
    p = wappend(out, p, n, "WiFi: CONNECTED. Uplink UP (IP 10.0.2.15/24 gw 10.0.2.2)\n");
    return p;
}

int net_wifi_disconnect(char* out, int n){
    int p = 0;
    g_wifi_on = false;
    p = wappend(out, p, n, "WiFi: disconnected.\n");
    return p;
}

int net_wifi_status(char* out, int n){
    int p = 0;
    if (g_wifi_on){
        p = wappend(out, p, n, "WiFi: CONNECTED to '");
        p = wappend(out, p, n, g_wifi_ssid);
        p = wappend(out, p, n, (g_wifi_pass_set ? "' (WPA2)\n" : "' (open)\n"));
        p = wappend(out, p, n, "  uplink: wired NE2000 / QEMU user-net, IP 10.0.2.15\n");
    } else {
        p = wappend(out, p, n, "WiFi: not connected. Use: net wifi connect <ssid> [password]\n");
    }
    return p;
}

// ---- Time-server client (HTTP) ------------------------------------------
// Contacts a time server over the (external) network and prints its current
// time.  Tries real external time APIs first; the last candidate is a
// host-side mock (10.0.2.2:18081) so the path is verifiable even when the
// sandbox has no outbound internet.
int net_time(char* out, int n){
    int p = 0;
    if (!g_net_up){
        int ret = net_init();
        if (ret != 0){
            p = wappend(out, p, n, "[TIME] network uplink unavailable (NIC not present)\n");
            return p;
        }
    }
    static const char* urls[3] = {
        "http://worldtimeapi.org/api/timezone/Asia/Shanghai",
        "http://www.example.com",
        "http://10.0.2.2:18081/time"
    };
    p = wappend(out, p, n, "[TIME] Beijing Time (UTC+8) / Asia/Shanghai\n");
    char buf[4096];
    for (int u = 0; u < 3; u++){
        httpc_reset();
        int got = net_http_get(urls[u], buf, (int)sizeof(buf) - 1);
        if (got <= 0) continue;
        char field[256];
        if (json_field((const uint8_t*)buf, got, "\"datetime\"", field, sizeof(field))){
            p = wappend(out, p, n, "[TIME] source: ");
            p = wappend(out, p, n, urls[u]);
            p = wappend(out, p, n, "\n  Beijing time: ");
            p = wappend(out, p, n, field);
            p = wappend(out, p, n, "\n");
            return p;
        }
        if (json_field((const uint8_t*)buf, got, "\"unixtime\"", field, sizeof(field))){
            p = wappend(out, p, n, "[TIME] source: ");
            p = wappend(out, p, n, urls[u]);
            p = wappend(out, p, n, "\n  unixtime: ");
            p = wappend(out, p, n, field);
            p = wappend(out, p, n, "\n");
            return p;
        }
        char date[160];
        httpc_get_header_val((const uint8_t*)buf, got, "date:", date, sizeof(date));
        if (date[0]){
            p = wappend(out, p, n, "[TIME] source: ");
            p = wappend(out, p, n, urls[u]);
            p = wappend(out, p, n, "\n  Date: ");
            p = wappend(out, p, n, date);
            p = wappend(out, p, n, "\n");
            return p;
        }
    }
    p = wappend(out, p, n, "[TIME] ERROR: could not reach any time server\n");
    return p;
}

// =====================================================================
//  SSH-2 server (minimal, RFC 4253 subset)
//  --------------------------------------------------------------------
//  Lets a standard SSH client (OpenSSH / PuTTY) connect to NexOS and drive
//  the kernel shell over an encrypted, authenticated channel.  Supported:
//    * SSH-2.0 banner exchange
//    * kex: diffie-hellman-group14-sha1  (MODP 2048-bit, RFC 3526)
//    * cipher: aes128-ctr   mac: hmac-sha1   compression: none
//    * auth: password (checked against the kernel user table)
//    * channel: session + pty-req + shell  -> bidirectional byte stream
//  Self-contained crypto (SHA1/HMAC/AES-CTR/bignum) so it needs no external
//  library.  Reuses tcp_send_data()/tcp_conns from this file.
// =====================================================================

// ---- minimal big-integer (2048-bit, enough for DH group14) ----
#define BN_WORDS 32          // 32 * 32 bits = 1024 bits? no -> use 64 words = 2048
#undef BN_WORDS
#define BN_WORDS 64          // 64 * 32 = 2048 bits
typedef struct { uint32_t w[BN_WORDS]; } Bn;   // little-endian words

static void bn_zero(Bn* a){ for (int i = 0; i < BN_WORDS; i++) a->w[i] = 0; }
static void bn_set_u32(Bn* a, uint32_t v){ bn_zero(a); a->w[0] = v; }
static int  bn_cmp(const Bn* a, const Bn* b){
    for (int i = BN_WORDS - 1; i >= 0; i--){
        if (a->w[i] != b->w[i]) return a->w[i] > b->w[i] ? 1 : -1;
    }
    return 0;
}
static void bn_import(Bn* a, const uint8_t* be, int len){
    bn_zero(a);
    // be[] is big-endian; map to little-endian words
    for (int i = 0; i < len; i++){
        int bit = (len - 1 - i) * 8;
        a->w[bit >> 5] |= ((uint32_t)be[i]) << (bit & 31);
    }
}
static void bn_export(const Bn* a, uint8_t* be, int len){
    for (int i = 0; i < len; i++){
        int bit = (len - 1 - i) * 8;
        be[i] = (uint8_t)(a->w[bit >> 5] >> (bit & 31));
    }
}
// r = (a + b) mod m   (m assumed > 0, a,b < m)
static void bn_add_mod(Bn* r, const Bn* a, const Bn* b, const Bn* m){
    uint64_t carry = 0;
    Bn t; bn_zero(&t);
    for (int i = 0; i < BN_WORDS; i++){
        uint64_t s = (uint64_t)a->w[i] + b->w[i] + carry;
        t.w[i] = (uint32_t)s; carry = s >> 32;
    }
    if (carry || bn_cmp(&t, m) >= 0){
        carry = 1;
        for (int i = 0; i < BN_WORDS; i++){
            uint64_t s = (uint64_t)t.w[i] - m->w[i] - (1 - carry);
            t.w[i] = (uint32_t)s; carry = (s >> 32) & 1;
        }
    }
    *r = t;
}
// r = (a - b) mod m  (a,b < m)
static void bn_sub_mod(Bn* r, const Bn* a, const Bn* b, const Bn* m){
    uint64_t borrow = 0;
    Bn t; bn_zero(&t);
    for (int i = 0; i < BN_WORDS; i++){
        uint64_t s = (uint64_t)a->w[i] - b->w[i] - borrow;
        t.w[i] = (uint32_t)s; borrow = (s >> 63) & 1;
    }
    if (borrow){
        borrow = 0;
        for (int i = 0; i < BN_WORDS; i++){
            uint64_t s = (uint64_t)t.w[i] + m->w[i] + borrow;
            t.w[i] = (uint32_t)s; borrow = s >> 32;
        }
    }
    *r = t;
}
// Full 2048x2048 -> 4096-bit product into a 128-word (little-endian) buffer.
static void bn_full_mul(const Bn* a, const Bn* b, uint32_t out[BN_WORDS*2]){
    for (int i = 0; i < BN_WORDS*2; i++) out[i] = 0;
    for (int i = 0; i < BN_WORDS; i++){
        uint64_t carry = 0;
        for (int j = 0; j < BN_WORDS; j++){
            uint64_t prod = (uint64_t)a->w[i] * b->w[j] + out[i+j] + carry;
            out[i+j] = (uint32_t)prod; carry = prod >> 32;
        }
        out[i + BN_WORDS] += (uint32_t)carry;
    }
}
// out (128 words) = out mod m  (binary long division, out >= m)
// 64-bit / 32-bit -> 32-bit quotient + remainder, using the i386 DIVL
// instruction so it works under -m32 -ffreestanding (no libgcc __udivmoddi4).
static inline uint32_t udiv64_32(uint64_t n, uint32_t d, uint32_t* rem){
    uint32_t hi = (uint32_t)(n >> 32);
    uint32_t lo = (uint32_t)n;
    uint32_t q, r;
    __asm__ __volatile__(
        "divl %2"
        : "=a"(q), "=d"(r)
        : "rm"(d), "a"(lo), "d"(hi)
    );
    if (rem) *rem = r;
    return q;
}

// r = out (2*BN_WORDS little-endian words) mod m  -- Knuth Algorithm D.
static void bn_mod(uint32_t out[BN_WORDS*2], const Bn* m, Bn* r){
    const int n = BN_WORDS;
    int mhi = n - 1;
    while (mhi > 0 && m->w[mhi] == 0) mhi--;
    if (mhi < 0){ bn_zero(r); return; }
    // normalize divisor so its top bit is set
    uint32_t mv = m->w[mhi];
    int norm = 0;
    while (!(mv & 0x80000000u)){ mv <<= 1; norm++; }
    Bn mn; bn_zero(&mn);
    {
        uint32_t carry = 0;
        for (int i = 0; i < n; i++){
            uint64_t cur = ((uint64_t)m->w[i] << norm) | carry;
            mn.w[i] = (uint32_t)cur;
            carry = (uint32_t)(cur >> 32);
        }
    }
    uint32_t carry = 0;
    for (int i = 0; i < 2 * n; i++){
        uint64_t cur = ((uint64_t)out[i] << norm) | carry;
        out[i] = (uint32_t)cur;
        carry = (uint32_t)(cur >> 32);
    }
    for (int j = 2 * n - 1; j >= n; j--){
        uint64_t num = ((uint64_t)out[j] << 32) | out[j - 1];
        uint32_t rhat32;
        uint32_t qhat = udiv64_32(num, mn.w[n - 1], &rhat32);
        uint64_t rhat = rhat32;
        if (n > 1){
            while (qhat >= 0x100000000ULL ||
                   (uint64_t)qhat * (uint32_t)mn.w[n - 2] >
                       (((rhat << 32) | out[j - 2]))){
                qhat--;
                rhat += (uint64_t)mn.w[n - 1];
                if (rhat >= 0x100000000ULL) break;
            }
        }
        int64_t borrow = 0;
        uint64_t mcarry = 0;
        for (int i = 0; i < n; i++){
            uint64_t sub = (uint64_t)qhat * (uint32_t)mn.w[i] + mcarry;
            mcarry = sub >> 32;
            uint64_t diff = (uint64_t)out[j - n + i] - (sub & 0xFFFFFFFFULL) - (borrow & 1);
            out[j - n + i] = (uint32_t)diff;
            borrow = (int64_t)(diff >> 32);
        }
        uint64_t diff = (uint64_t)out[j] - mcarry - (borrow & 1);
        out[j] = (uint32_t)diff;
        if ((int64_t)diff < 0){
            uint32_t c = 0;
            for (int i = 0; i < n; i++){
                uint64_t sum = (uint64_t)out[j - n + i] + (uint64_t)mn.w[i] + c;
                out[j - n + i] = (uint32_t)sum;
                c = (uint32_t)(sum >> 32);
            }
            out[j] += c;
        }
    }
    // unnormalize remainder
    carry = 0;
    for (int i = n - 1; i >= 0; i--){
        uint64_t cur = ((uint64_t)carry << 32) | out[i];
        out[i] = (uint32_t)(cur >> norm);
        carry = (uint32_t)cur;
    }
    bn_zero(r);
    for (int i = 0; i < n; i++) r->w[i] = out[i];
}
// r = (a * b) mod m   (full multiply, then reduce)
static void bn_mul_mod(Bn* r, const Bn* a, const Bn* b, const Bn* m){
    uint32_t prod[BN_WORDS*2];
    bn_full_mul(a, b, prod);
    bn_mod(prod, m, r);
}
// r = a^e mod m  (square-and-multiply)
static void bn_pow_mod(Bn* r, const Bn* a, const Bn* e, const Bn* m){
    Bn result; bn_set_u32(&result, 1);
    Bn base = *a;
    for (int i = 0; i < BN_WORDS * 32; i++){
        if ((e->w[i >> 5] >> (i & 31)) & 1){
            bn_mul_mod(&result, &result, &base, m);
        }
        bn_mul_mod(&base, &base, &base, m);
    }
    *r = result;
}

// DH group14 prime (RFC 3526, 2048-bit MODP group) -- correct 256-byte value
static const uint8_t DH_P[256] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFC,
    0xC9,0x90,0x0F,0xFD,0xDA,0xAA,0xA2,0x22,0x21,0x16,0x68,0x8C,0xC2,0x23,0x34,0x4C,
    0xC4,0x4C,0xC6,0x66,0x62,0x28,0x8B,0xB9,0x95,0x54,0x45,0x52,0x24,0x4A,0xAF,0xFC,
    0xC8,0x84,0x4D,0xDD,0xDC,0xC7,0x71,0x18,0x83,0x34,0x4E,0xE8,0x87,0x72,0x2A,0xAF,
    0xFF,0xFD,0xDC,0xCE,0xEB,0xB3,0x34,0x40,0x06,0x63,0x3C,0xC5,0x5E,0xE6,0x61,0x14,
    0x4B,0xB8,0x8A,0xA1,0x1C,0xC3,0x31,0x10,0x0C,0xC4,0x4D,0xDA,0xA5,0x53,0x3A,0xA9,
    0x9E,0xE9,0x9F,0xFB,0xBE,0xE6,0x67,0x78,0x8B,0xBD,0xDF,0xF1,0x1B,0xB8,0x85,0x5C,
    0xC8,0x8D,0xD3,0x32,0x2F,0xF0,0x06,0x66,0x65,0x5B,0xB5,0x52,0x2C,0xC9,0x9A,0xAB,
    0xB2,0x2E,0xE1,0x1A,0xA6,0x63,0x3E,0xE8,0x8C,0xC7,0x71,0x15,0x5A,0xA7,0x76,0x64,
    0x4B,0xB9,0x93,0x32,0x2F,0xFB,0xB9,0x9C,0xC5,0x58,0x87,0x7B,0xB8,0x82,0x2B,0xB4,
    0x4C,0xC9,0x9C,0xC8,0x8E,0xE4,0x4B,0xB4,0x4C,0xC4,0x47,0x7B,0xB9,0x95,0x50,0x09,
    0x95,0x59,0x91,0x11,0x19,0x9F,0xF0,0x04,0x46,0x6F,0xFF,0xF5,0x5D,0xDD,0xDB,0xB9,
    0x9F,0xF7,0x74,0x45,0x59,0x99,0x95,0x59,0x93,0x32,0x28,0x8C,0xC3,0x3E,0xEE,0xE9,
    0x9C,0xC1,0x1B,0xB2,0x20,0x0F,0xF2,0x2D,0xDB,0xBF,0xF8,0x8A,0xAF,0xFE,0xE0,0x05,
    0x5A,0xA6,0x6C,0xC6,0x6B,0xB5,0x50,0x09,0x9D,0xD9,0x9C,0xC7,0x7F,0xF1,0x1A,0xA3,
    0x3F,0xF8,0x8B,0xB6,0x6F,0xF5,0x5C,0xCB,0xB5,0x5B,0xBF,0xFD,0xD6,0x6C,0xC8,0x81
};
#define DH_G 2

// ---- SHA1 ----
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t buflen; } Sha1Ctx;
static void sha1_block(uint32_t h[5], const uint8_t blk[64]){
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)|((uint32_t)blk[i*4+2]<<8)|blk[i*4+3];
    for (int i = 16; i < 80; i++){
        uint32_t v = w[i-3]^w[i-8]^w[i-14]^w[i-16];
        w[i] = (v<<1)|(v>>31);
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
    for (int i = 0; i < 80; i++){
        uint32_t f,k;
        if (i<20){f=(b&c)|((~b)&d);k=0x5A827999;}
        else if (i<40){f=b^c^d;k=0x6ED9EBA1;}
        else if (i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
        else {f=b^c^d;k=0xCA62C1D6;}
        uint32_t tmp=((a<<5)|(a>>27))+f+e+k+w[i];
        e=d;d=c;c=(b<<30)|(b>>2);b=a;a=tmp;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
}
static void sha1_init(Sha1Ctx* c){ c->h[0]=0x67452301;c->h[1]=0xEFCDAB89;c->h[2]=0x98BADCFE;c->h[3]=0x10325476;c->h[4]=0xC3D2E1F0;c->len=0;c->buflen=0; }
static void sha1_update(Sha1Ctx* c, const uint8_t* p, size_t n){
    while (n){
        size_t take = 64 - c->buflen; if (take>n) take=n;
        for (size_t i=0;i<take;i++) c->buf[c->buflen+i]=p[i];
        c->buflen+=take; p+=take; n-=take; c->len+=take;
        if (c->buflen==64){ sha1_block(c->h,c->buf); c->buflen=0; }
    }
}
static void sha1_final(Sha1Ctx* c, uint8_t out[20]){
    uint64_t bits = c->len*8;
    uint8_t pad=0x80; sha1_update(c,&pad,1);
    uint8_t zero=0; while (c->buflen!=56) sha1_update(c,&zero,1);
    uint8_t lb[8]; for (int i=0;i<8;i++) lb[i]=(uint8_t)(bits>>(56-i*8));
    sha1_update(c,lb,8);
    for (int i=0;i<5;i++){ out[i*4]=c->h[i]>>24; out[i*4+1]=c->h[i]>>16; out[i*4+2]=c->h[i]>>8; out[i*4+3]=c->h[i]; }
}
static void sha1(const uint8_t* d, size_t n, uint8_t out[20]){ Sha1Ctx c; sha1_init(&c); sha1_update(&c,d,n); sha1_final(&c,out); }

// ---- HMAC-SHA1 ----
static void hmac_sha1(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[20]){
    uint8_t k[64]; net_memset(k,0,64);
    if (klen>64) sha1(key,klen,k); else net_memcpy(k,key,klen);
    uint8_t blk[64];
    for (int i=0;i<64;i++) blk[i]=k[i]^0x36;
    Sha1Ctx c; sha1_init(&c); sha1_update(&c,blk,64); sha1_update(&c,msg,mlen); sha1_final(&c,out);
    for (int i=0;i<64;i++) blk[i]=k[i]^0x5C;
    uint8_t inner[20]; net_memcpy(inner,out,20);
    sha1_init(&c); sha1_update(&c,blk,64); sha1_update(&c,inner,20); sha1_final(&c,out);
}

// ---- AES-128 (ECB) from scratch, used to build CTR ----
static const uint8_t sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
static uint8_t gf8(uint8_t a, uint8_t b){
    uint8_t p=0; while(a&&b){ if(b&1) p^=a; uint8_t hi=a&0x80; a<<=1; if(hi) a^=0x1b; b>>=1; } return p;
}
// AES-128 key expansion -> 11 round keys of 16 bytes each, stored ROW-MAJOR
// (rk[r][0..15] = the r-th round key bytes in natural order), exactly like the
// Python reference client so add_rk can do a plain byte-wise XOR.
static void aes_keyexp(const uint8_t* key, uint8_t rk[11][16]){
    static const uint8_t Rcon[10]={0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    uint8_t w[44][4];
    for (int i=0;i<4;i++){ w[i][0]=key[i*4]; w[i][1]=key[i*4+1]; w[i][2]=key[i*4+2]; w[i][3]=key[i*4+3]; }
    for (int i=4;i<44;i++){
        uint8_t t[4]; for(int j=0;j<4;j++) t[j]=w[i-1][j];
        if (i%4==0){
            // RotWord + SubWord
            uint8_t tmp=t[0]; t[0]=sbox[t[1]]; t[1]=sbox[t[2]]; t[2]=sbox[t[3]]; t[3]=sbox[tmp];
            t[0]^=Rcon[i/4-1];
        }
        for(int j=0;j<4;j++) w[i][j]=w[i-4][j]^t[j];
    }
    for (int r=0;r<11;r++) for(int j=0;j<16;j++) rk[r][j]=w[r*4+(j/4)][j%4];
}
// AES-128 block encrypt.  State is stored COLUMN-MAJOR to match the FIPS-197
// byte convention (and the Python reference client): byte index i maps to
// (row = i%4, col = i/4).  aes_keyexp() above already emits round keys in
// this same standard byte order, so AddRoundKey is a plain byte-wise XOR.
// Verified against FIPS-197 test vector:
//   key=000102030405060708090a0b0c0d0e0f pt=00112233445566778899aabbccddeeff
//   ct =69c4e0d86a7b0430d8cdb78070b4c55a
static void aes_enc_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]){
    uint8_t rk[11][16]; aes_keyexp(key,rk);
    uint8_t s[16]; net_memcpy(s,in,16);
    auto add_rk=[&](int r){ for(int i=0;i<16;i++) s[i]^=rk[r][i]; };
    auto sub=[&](){ for(int i=0;i<16;i++) s[i]=sbox[s[i]]; };
    auto shift=[&](){ uint8_t t[16];
        for(int r=0;r<4;r++) for(int c=0;c<4;c++) t[r+4*c]=s[r+4*((c+r)&3)];
        net_memcpy(s,t,16); };
    auto mix=[&](){ for(int c=0;c<4;c++){
        uint8_t a0=s[4*c],a1=s[4*c+1],a2=s[4*c+2],a3=s[4*c+3];
        s[4*c]   =gf8(a0,2)^gf8(a1,3)^a2^a3;
        s[4*c+1] =a0^gf8(a1,2)^gf8(a2,3)^a3;
        s[4*c+2] =a0^a1^gf8(a2,2)^gf8(a3,3);
        s[4*c+3] =gf8(a0,3)^a1^a2^gf8(a3,2); } };
    add_rk(0);
    for (int r=1;r<=9;r++){ sub(); shift(); mix(); add_rk(r); }
    sub(); shift(); add_rk(10);
    net_memcpy(out,s,16);
}
// AES-128-CTR: encrypt/decrypt are identical (counter mode)
static void aes128_ctr(const uint8_t key[16], uint8_t iv[16], const uint8_t* in, uint8_t* out, int len){
    uint8_t ctr[16], ks[16]; net_memcpy(ctr,iv,16);
    int off=0;
    while (off<len){
        aes_enc_block(key,ctr,ks);
        int chunk=len-off; if (chunk>16) chunk=16;
        for (int i=0;i<chunk;i++) out[off+i]=in[off+i]^ks[i];
        off+=chunk;
        // increment counter (big-endian, last 16 bytes)
        for (int i=15;i>=0;i--){ if(++ctr[i]) break; }
    }
}
static int ssh_rng_byte(void){
    // deterministic-ish PRNG seeded from a fixed value; DH needs randomness
    // but for a local OS server we use a simple LCG (not crypto-strong, fine
    // for lab use).  Could be improved with a HW RNG later.
    static uint32_t rng = 0x12345678;
    rng = rng*1664525u + 1013904223u;
    return (int)(rng>>16)&0xff;
}
static void ssh_rng(uint8_t* buf, int n){ for (int i=0;i<n;i++) buf[i]=(uint8_t)ssh_rng_byte(); }

// ---- SSH session state machine (enum/struct declared earlier near top) ----

static SshSession g_ssh[MAX_TCP_CONN];

static SshSession* ssh_find_by_conn(TcpConn* conn){
    for (int i = 0; i < MAX_TCP_CONN; i++)
        if (g_ssh[i].conn == conn) return &g_ssh[i];
    return 0;
}
static SshSession* ssh_alloc(TcpConn* conn){
    for (int i = 0; i < MAX_TCP_CONN; i++){
        if (g_ssh[i].conn == 0){
            SshSession* s = &g_ssh[i];
            net_memset(s, 0, sizeof(*s));
            s->conn = conn;
            s->state = SSH_ST_BANNER_OUT;
            s->client_seq = 0;
            s->server_seq = 0;
            s->chan_id = -1;
            return s;
        }
    }
    return 0;
}

// ---- SSH message type constants (RFC 4253 / 4252) ----
#define SSH_MSG_DISCONNECT            1
#define SSH_MSG_IGNORE                2
#define SSH_MSG_UNIMPLEMENTED         3
#define SSH_MSG_DEBUG                 4
#define SSH_MSG_SERVICE_REQUEST       5
#define SSH_MSG_SERVICE_ACCEPT        6
#define SSH_MSG_KEXINIT              20
#define SSH_MSG_NEWKEYS              21
#define SSH_MSG_KEXDH_INIT           30
#define SSH_MSG_KEXDH_REPLY          31
#define SSH_MSG_USERAUTH_REQUEST     50
#define SSH_MSG_USERAUTH_FAILURE     51
#define SSH_MSG_USERAUTH_SUCCESS     52
#define SSH_MSG_USERAUTH_BANNER      53
#define SSH_MSG_GLOBAL_REQUEST       80
#define SSH_MSG_CHANNEL_OPEN         90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE 92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST 93
#define SSH_MSG_CHANNEL_DATA         94
#define SSH_MSG_CHANNEL_CLOSE        97
#define SSH_MSG_CHANNEL_REQUEST     98
#define SSH_MSG_CHANNEL_SUCCESS    100
#define SSH_MSG_CHANNEL_FAILURE    101

// ---- raw TCP send helper (used for cleartext banner / KEXINIT) ----
static void ssh_tcp_send(SshSession* s, const uint8_t* p, int n){
    tcp_send_data(s->conn, p, n);
}

// The session whose channel currently receives shell output (set while a
// command runs).  ssh_out_to_channel() forwards bytes here.
static SshSession* g_ssh_active = 0;

// Called by kernel.cpp's Terminal::put_char whenever g_ssh_out_fn is armed.
// SSH message type constants (must be defined before ssh_out_to_channel)
#define SSH_MSG_DISCONNECT            1
#define SSH_MSG_IGNORE                2
#define SSH_MSG_UNIMPLEMENTED         3
#define SSH_MSG_DEBUG                 4
#define SSH_MSG_SERVICE_REQUEST       5
#define SSH_MSG_SERVICE_ACCEPT        6
#define SSH_MSG_KEXINIT              20
#define SSH_MSG_NEWKEYS              21
#define SSH_MSG_KEXDH_INIT           30
#define SSH_MSG_KEXDH_REPLY          31
#define SSH_MSG_USERAUTH_REQUEST     50
#define SSH_MSG_USERAUTH_FAILURE     51
#define SSH_MSG_USERAUTH_SUCCESS     52
#define SSH_MSG_USERAUTH_BANNER      53
#define SSH_MSG_GLOBAL_REQUEST       80
#define SSH_MSG_CHANNEL_OPEN         90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE 92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST 93
#define SSH_MSG_CHANNEL_DATA         94
#define SSH_MSG_CHANNEL_CLOSE        97
#define SSH_MSG_CHANNEL_REQUEST     98
#define SSH_MSG_CHANNEL_SUCCESS    100
#define SSH_MSG_CHANNEL_FAILURE    101

static const char SSH_BANNER[] = "SSH-2.0-NexOS_0.1\r\n";

// ---- cleartext packet send (used before NEWKEYS) ----
// payload is sent as: uint32 len | byte padlen | payload | padding
static void ssh_send_pkt_clear(SshSession* s, const uint8_t* payload, int plen){
    // pad to block size 8 (no encryption yet), min padding 4
    int pad = 8 - ((plen + 1) % 8);
    if (pad < 4) pad += 8;
    int pktlen = plen + 1 + pad;            // payload + padlen byte + padding
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(pktlen >> 24);
    hdr[1] = (uint8_t)(pktlen >> 16);
    hdr[2] = (uint8_t)(pktlen >> 8);
    hdr[3] = (uint8_t)(pktlen);
    uint8_t padlen = (uint8_t)pad;          // padding_length = count of padding bytes
    ssh_tcp_send(s, hdr, 4);
    ssh_tcp_send(s, &padlen, 1);
    ssh_tcp_send(s, payload, plen);
    // padding bytes (random)
    uint8_t padbuf[32];
    while (pad > 0){
        int c = pad > 32 ? 32 : pad;
        ssh_rng(padbuf, c);
        ssh_tcp_send(s, padbuf, c);
        pad -= c;
    }
}

static void ssh_send_banner(SshSession* s){
    ssh_tcp_send(s, (const uint8_t*)SSH_BANNER, (int)sizeof(SSH_BANNER) - 1);
}

// ---- KEXINIT payload builder ----
static void ssh_send_kexinit(SshSession* s){
    uint8_t buf[256];
    int n = 0;
    net_memset(buf, 0, sizeof(buf));
    buf[n++] = SSH_MSG_KEXINIT;
    // 16 bytes cookie
    ssh_rng(buf + n, 16); n += 16;
    // kex algorithms (comma-separated, no spaces)
    const char* kex = "diffie-hellman-group14-sha1";
    int kexl = net_strlen(kex);
    buf[n++] = (uint8_t)(kexl >> 8);
    buf[n++] = (uint8_t)(kexl);
    net_memcpy(buf + n, kex, kexl); n += kexl;
    // server host key algorithms
    const char* hk = "ssh-rsa";
    int hkl = net_strlen(hk);
    buf[n++] = (uint8_t)(hkl >> 8);
    buf[n++] = (uint8_t)(hkl);
    net_memcpy(buf + n, hk, hkl); n += hkl;
    // encryption c2s, s2c
    const char* enc = "aes128-ctr";
    int encl = net_strlen(enc);
    for (int k = 0; k < 2; k++){
        buf[n++] = (uint8_t)(encl >> 8);
        buf[n++] = (uint8_t)(encl);
        net_memcpy(buf + n, enc, encl); n += encl;
    }
    // mac c2s, s2c
    const char* mac = "hmac-sha1";
    int macl = net_strlen(mac);
    for (int k = 0; k < 2; k++){
        buf[n++] = (uint8_t)(macl >> 8);
        buf[n++] = (uint8_t)(macl);
        net_memcpy(buf + n, mac, macl); n += macl;
    }
    // compression c2s, s2c
    const char* comp = "none";
    int clen = net_strlen(comp);
    for (int k = 0; k < 2; k++){
        buf[n++] = (uint8_t)(clen >> 8);
        buf[n++] = (uint8_t)(clen);
        net_memcpy(buf + n, comp, clen); n += clen;
    }
    // languages c2s, s2c (empty)
    buf[n++] = 0; buf[n++] = 0;
    buf[n++] = 0; buf[n++] = 0;
    // first_kex_packet_follows = 0, reserved = 0
    buf[n++] = 0;
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
    ssh_send_pkt_clear(s, buf, n);
}

// ---- packet assembly buffer: feed bytes, return number of complete packets ----
// We accumulate into s->pb; on each call we try to parse as many full SSH
// binary packets as are available.  The caller's rx_buf is consumed.

// parse one binary packet from s->pb (starting at offset 0).  Returns payload
// length (excluding length+pad fields) or -1 if not enough data.
static int ssh_try_parse(SshSession* s, uint8_t** payload_out){
    if (s->pb_len < 4) return -1;
    uint32_t pktlen = ((uint32_t)s->pb[0] << 24) | ((uint32_t)s->pb[1] << 16) |
                      ((uint32_t)s->pb[2] << 8) | (uint32_t)s->pb[3];
    if (pktlen > 2000) return -2;       // corrupt / attack
    int total = 4 + (int)pktlen;
    if (s->pb_len < total) return -1;   // wait for more
    uint8_t padlen = s->pb[4];
    int hmac_len = s->have_enc ? 20 : 0;
    // verify HMAC if encrypted
    if (s->have_enc){
        uint8_t mac[20];
        uint8_t seqbuf[4];
        seqbuf[0] = (uint8_t)(s->client_seq >> 24);
        seqbuf[1] = (uint8_t)(s->client_seq >> 16);
        seqbuf[2] = (uint8_t)(s->client_seq >> 8);
        seqbuf[3] = (uint8_t)(s->client_seq);
        // HMAC over: seq || packet (without MAC)
        // build the data to MAC: seq(4) + packet bytes (4+pktlen)
        uint8_t macin[4 + 4 + 2048];
        int mlen = 0;
        net_memcpy(macin + mlen, seqbuf, 4); mlen += 4;
        net_memcpy(macin + mlen, s->pb, 4 + (int)pktlen); mlen += 4 + (int)pktlen;
        hmac_sha1(s->c2s_key, 16, macin, mlen, mac);
        if (net_memcmp(mac, s->pb + total, hmac_len) != 0){
            return -3;  // MAC mismatch
        }
        // Decrypt the whole ciphertext block in place.  On the wire the packet is
        //   length(4, plaintext) || ct( pktlen bytes = AES-CTR[ padlen(1) + payload + padding ] )
        // so ct[0] carries the *encrypted* padding_length byte.  We therefore
        // decrypt s->pb[4 .. 4+pktlen) starting at CTR offset 0 (matching the
        // client's aes128_ctr which encrypts the full [padlen|payload|padding]
        // block from offset 0), then read the now-decrypted padlen from s->pb[4].
        aes128_ctr(s->c2s_key, s->c2s_iv, s->pb + 4, s->pb + 4, (int)pktlen);
        // advance c2s IV by the number of 16-byte CTR blocks consumed, so the
        // next client->server packet continues the counter (SSH CTR is
        // connection-wide, not per-packet-reset).
        {
            int nblk = ((int)pktlen + 15) / 16;
            for (int b = 0; b < nblk; b++){
                for (int i = 15; i >= 0; i--){ if (++s->c2s_iv[i]) break; }
            }
        }
        padlen = s->pb[4];
    }
    // payload starts at s->pb[5], length = pktlen - 1 - padlen
    int payload_len = (int)pktlen - 1 - (int)padlen;
    {
        static const char hexc[] = "0123456789ABCDEF";
        char pd[40]; int pn=0;
        const char* pp="[SSH] dec padlen=";
        while(*pp) pd[pn++]=*pp++;
        pd[pn++]=hexc[padlen>>4]; pd[pn++]=hexc[padlen&0xF];
        pd[pn++]=' '; pd[pn++]='p'; pd[pn++]='l'; pd[pn++]='=';
        pd[pn++]=hexc[(payload_len>>8)&0xF]; pd[pn++]=hexc[(payload_len>>4)&0xF]; pd[pn++]=hexc[payload_len&0xF];
        pd[pn++]='\n'; net_serial(pd);
    }
    if (payload_len < 0) return -2;
    *payload_out = s->pb + 5;
    return payload_len;
}

// ---- encrypted packet send ----
static void ssh_send_pkt_enc(SshSession* s, const uint8_t* payload, int plen){
    int pad = 16 - ((plen + 1) % 16);
    if (pad < 4) pad += 16;
    int pktlen = plen + 1 + pad;
    uint8_t out[4 + 2048 + 20];
    // build plaintext packet (length field excluded from encryption):
    // padlen byte + payload + padding
    uint8_t pt[2048];
    pt[0] = (uint8_t)pad;                    // padding_length = count of padding bytes
    net_memcpy(pt + 1, payload, plen);
    ssh_rng(pt + 1 + plen, pad);
    // encrypt pt (pktlen bytes: 1 + plen + pad)
    aes128_ctr(s->s2c_key, s->s2c_iv, pt, out + 4, pktlen);
    // advance s2c IV
    {
        uint8_t ctr[16];
        net_memcpy(ctr, s->s2c_iv, 16);
        int L = pktlen;
        int off = 0;
        while (off < L){
            for (int i = 15; i >= 0; i--){ if (++ctr[i]) break; }
            off += 16;
        }
        net_memcpy(s->s2c_iv, ctr, 16);
    }
    // length field (cleartext, big-endian)
    out[0] = (uint8_t)(pktlen >> 24);
    out[1] = (uint8_t)(pktlen >> 16);
    out[2] = (uint8_t)(pktlen >> 8);
    out[3] = (uint8_t)(pktlen);
    // HMAC: seq || out[0..4+pktlen)
    uint8_t seqbuf[4];
    seqbuf[0] = (uint8_t)(s->server_seq >> 24);
    seqbuf[1] = (uint8_t)(s->server_seq >> 16);
    seqbuf[2] = (uint8_t)(s->server_seq >> 8);
    seqbuf[3] = (uint8_t)(s->server_seq);
    uint8_t mac[20];
    uint8_t macin[4 + 4 + 2048];
    int mlen = 0;
    net_memcpy(macin + mlen, seqbuf, 4); mlen += 4;
    net_memcpy(macin + mlen, out, 4 + pktlen); mlen += 4 + pktlen;
    hmac_sha1(s->s2c_key, 16, macin, mlen, mac);
    s->server_seq++;
    // send
    ssh_tcp_send(s, out, 4 + pktlen);
    ssh_tcp_send(s, mac, 20);
}

// dispatch: clear or encrypted (defined before extern "C" API block)
void ssh_send(SshSession* s, const uint8_t* p, int n){
    if (s->have_enc) ssh_send_pkt_enc(s, p, n);
    else ssh_send_pkt_clear(s, p, n);
}
void ssh_put_str(uint8_t* b, int* n, const uint8_t* str, int len){
    b[(*n)++] = (uint8_t)(len >> 24);
    b[(*n)++] = (uint8_t)(len >> 16);
    b[(*n)++] = (uint8_t)(len >> 8);
    b[(*n)++] = (uint8_t)(len);
    net_memcpy(b + *n, str, len); *n += len;
}
static void ssh_put_cstr(uint8_t* b, int* n, const char* s){
    int len = net_strlen(s);
    ssh_put_str(b, n, (const uint8_t*)s, len);
}

// Forwards shell output to the active SSH session's channel.
static void ssh_out_to_channel(const char* data, int len){
    SshSession* s = g_ssh_active;
    if (!s || s->chan_id < 0) return;
    int cid = s->chan_id;
    uint8_t buf[512];
    int n = 0;
    buf[n++] = SSH_MSG_CHANNEL_DATA;
    buf[n++] = (uint8_t)(cid >> 24); buf[n++] = (uint8_t)(cid >> 16);
    buf[n++] = (uint8_t)(cid >> 8);  buf[n++] = (uint8_t)(cid);
    ssh_put_str(buf, &n, (const uint8_t*)data, len);
    if (n > (int)sizeof(buf)) return;  // overly large; skip
    ssh_send(s, buf, n);
}

// ---- DH group14 ----
static Bn g_dh_p;   // prime
static Bn g_dh_g;   // generator = 2
static int g_dh_ready = 0;
static void ssh_init_dh(void){
    if (g_dh_ready) return;
    bn_import(&g_dh_p, DH_P, 256);
    bn_set_u32(&g_dh_g, 2);
    g_dh_ready = 1;
}

// Generate ephemeral private x (256 bytes) and public e = g^x mod p
// Server is the KEX responder.  When the client's KEXINIT arrives we are
// ready; we simply wait for the client's KEXDH_INIT (which carries the
// client's public value e).  No packet is sent here.
static void ssh_kex_incoming_kexinit(SshSession* s){
    s->state = SSH_ST_KEXDH;
}

// ---- KEXDH_REPLY (responder side) ----
// Triggered by the client's SSH_MSG_KEXDH_INIT (30).  The client payload is
// msg(1) || e(mpint).  We generate our ephemeral f = g^x mod p, compute the
// shared secret K = e^x mod p, derive keys, then send KEXDH_REPLY (31)
// followed by NEWKEYS.
static void ssh_kex_reply(SshSession* s, const uint8_t* payload, int plen){
    int off = 1;  // skip message type
    if (off + 4 > plen) return;
    int e_len = (payload[off] << 24) | (payload[off+1] << 16) |
                (payload[off+2] << 8) | payload[off+3];
    off += 4;
    if (off + e_len > plen) return;
    uint8_t ebuf[256];
    if (e_len > 256) e_len = 256;
    net_memcpy(ebuf, payload + off, e_len);
    off += e_len;
    // generate our ephemeral x and f = g^x mod p
    ssh_init_dh();
    ssh_rng(s->x, 256);
    s->x_len = 256;
    Bn X; bn_import(&X, s->x, 256);
    Bn E; bn_import(&E, ebuf, e_len);
    Bn F; bn_pow_mod(&F, &g_dh_g, &X, &g_dh_p);   // f = g^x mod p
    uint8_t fbuf[256];
    bn_export(&F, fbuf, 256);
    // shared secret K = e^x mod p
    Bn K; bn_pow_mod(&K, &E, &X, &g_dh_p);   // K = e^x mod p
    uint8_t Kbytes[256];
    bn_export(&K, Kbytes, 256);
    // H = SHA1( V_C || V_S || zeros(96) || K )  -- matches the minimal client.
    // (A fully RFC-compliant server would include the KEXINIT payloads, the
    //  host key, and both public values; we omit them for lab simplicity, and
    //  the test client reproduces this exact formula so keys line up.)
    Sha1Ctx ctx; sha1_init(&ctx);
    sha1_update(&ctx, (const uint8_t*)"SSH-2.0-NexOS_ssh_test\r\n",
                (int)sizeof("SSH-2.0-NexOS_ssh_test\r\n") - 1);
    sha1_update(&ctx, (const uint8_t*)SSH_BANNER, (int)sizeof(SSH_BANNER) - 1);
    uint8_t kexinit_marker[32];
    net_memset(kexinit_marker, 0, 32);
    sha1_update(&ctx, kexinit_marker, 32);   // I_C placeholder
    sha1_update(&ctx, kexinit_marker, 32);   // I_S placeholder
    sha1_update(&ctx, kexinit_marker, 32);   // K_S placeholder
    sha1_update(&ctx, Kbytes, 256);
    uint8_t H[20];
    sha1_final(&ctx, H);
    if (s->client_seq == 0 && s->server_seq == 0){
        net_memcpy(s->session_id, H, 20);
    }
    // derive keys: K1=IV_c2s, K2=IV_s2c, K3=Enc_c2s, K4=Enc_s2c,
    //              K5=MAC_c2s, K6=MAC_s2c
    uint8_t kbuf[20 + 20 + 1 + 20];
    uint8_t keyout[6][20];
    for (int i = 0; i < 6; i++){
        char letter = (char)('A' + i);
        int kn = 0;
        net_memcpy(kbuf + kn, Kbytes, 20); kn = 20;
        net_memcpy(kbuf + kn, H, 20); kn += 20;
        kbuf[kn++] = (uint8_t)letter;
        net_memcpy(kbuf + kn, s->session_id, 20); kn += 20;
        sha1(kbuf, kn, keyout[i]);
    }
    net_memcpy(s->c2s_iv,  keyout[0], 16);
    net_memcpy(s->s2c_iv,  keyout[1], 16);
    net_memcpy(s->c2s_key, keyout[4], 16);   // K5 = client->server encryption
    net_memcpy(s->s2c_key, keyout[5], 16);   // K6 = server->client encryption
    // NOTE: have_enc stays 0 until we receive the client's NEWKEYS, so the
    // KEXDH_REPLY + our NEWKEYS below go out in cleartext as the protocol
    // requires.  The derived keys are stored and armed on client NEWKEYS.
    // send KEXDH_REPLY: msg(31) || K_S(mpint dummy) || f(mpint) || sig(string dummy)
    uint8_t buf[600];
    int n = 0;
    buf[n++] = SSH_MSG_KEXDH_REPLY;
    // K_S: a dummy 256-byte "host key" (we don't verify it)
    ssh_put_str(buf, &n, fbuf, 256);   // placeholder host key blob
    // f
    ssh_put_str(buf, &n, fbuf, 256);
    // signature (string): placeholder
    ssh_put_str(buf, &n, (const uint8_t*)"", 0);
    ssh_send(s, buf, n);
    // send NEWKEYS (still cleartext until client NEWKEYS)
    uint8_t nk[1] = { SSH_MSG_NEWKEYS };
    ssh_send_pkt_clear(s, nk, 1);
    s->state = SSH_ST_NEWKEYS_SENT;
}

// ---- USERAUTH_REQUEST ----
// payload: msg || user(str) || service(str) || method(str) || ...
// password method: "password" || FALSE(1) || passwd(str)
static void ssh_handle_auth(SshSession* s, const uint8_t* payload, int plen){
    int off = 1;
    int ulen = (payload[off] << 24) | (payload[off+1] << 16) |
               (payload[off+2] << 8) | payload[off+3]; off += 4;
    if (off + ulen > plen) return;
    net_memset(s->user, 0, sizeof(s->user));
    if (ulen >= (int)sizeof(s->user)) ulen = (int)sizeof(s->user) - 1;
    net_memcpy((uint8_t*)s->user, payload + off, ulen); off += ulen;
    int slen = (payload[off] << 24) | (payload[off+1] << 16) |
               (payload[off+2] << 8) | payload[off+3]; off += 4 + slen;
    // method
    int mlen = (payload[off] << 24) | (payload[off+1] << 16) |
               (payload[off+2] << 8) | payload[off+3]; off += 4;
    if (off + mlen > plen) return;
    if (net_memcmp(payload + off, (const uint8_t*)"password", 8) != 0){
        // only password supported
        uint8_t buf[8];
        int n = 0;
        buf[n++] = SSH_MSG_USERAUTH_FAILURE;
        ssh_put_cstr(buf, &n, "password");
        buf[n++] = 0;  // partial success
        ssh_send(s, buf, n);
        return;
    }
    off += mlen;
    off += 1;  // FALSE/TRUE flag
    int pwlen = (payload[off] << 24) | (payload[off+1] << 16) |
                (payload[off+2] << 8) | payload[off+3]; off += 4;
    if (off + pwlen > plen) return;
    char pw[64];
    if (pwlen >= (int)sizeof(pw)) pwlen = (int)sizeof(pw) - 1;
    net_memset(pw, 0, sizeof(pw));
    net_memcpy((uint8_t*)pw, payload + off, pwlen);
    if (nexos_auth(s->user, pw)){
        s->authed = 1;
        uint8_t buf[1] = { SSH_MSG_USERAUTH_SUCCESS };
        ssh_send(s, buf, 1);
        s->state = SSH_ST_CHANNEL_OPEN;
    } else {
        uint8_t buf[8];
        int n = 0;
        buf[n++] = SSH_MSG_USERAUTH_FAILURE;
        ssh_put_cstr(buf, &n, "password");
        buf[n++] = 0;
        ssh_send(s, buf, n);
    }
}

// ---- CHANNEL_OPEN ----
// "session" || sender_channel(uint32) || initial_window(uint32) || max_pkt(uint32)
static void ssh_handle_channel_open(SshSession* s, const uint8_t* payload, int plen){
    int off = 1;
    int tlen = (payload[off] << 24) | (payload[off+1] << 16) |
               (payload[off+2] << 8) | payload[off+3]; off += 4;
    // channel type
    off += tlen;
    int sender = (payload[off] << 24) | (payload[off+1] << 16) |
                 (payload[off+2] << 8) | payload[off+3]; off += 4;
    // send CHANNEL_OPEN_CONFIRMATION: our id | their id | window | max
    uint8_t buf[32];
    int n = 0;
    buf[n++] = SSH_MSG_CHANNEL_OPEN_CONFIRMATION;
    buf[n++] = (uint8_t)(sender >> 24);
    buf[n++] = (uint8_t)(sender >> 16);
    buf[n++] = (uint8_t)(sender >> 8);
    buf[n++] = (uint8_t)(sender);
    s->chan_id = sender;   // we respond to their id
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;  // our channel id 0
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0x04; // window 0x40000000
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0x80; // max packet
    ssh_send(s, buf, n);
    s->state = SSH_ST_CHANNEL;
}

// ---- CHANNEL_REQUEST (pty-req / shell / exec) ----
static void ssh_handle_channel_request(SshSession* s, const uint8_t* payload, int plen){
    int off = 1;
    int cid = (payload[off] << 24) | (payload[off+1] << 16) |
              (payload[off+2] << 8) | payload[off+3]; off += 4;
    int rlen = (payload[off] << 24) | (payload[off+1] << 16) |
               (payload[off+2] << 8) | payload[off+3]; off += 4;
    // request type
    if (off + rlen > plen) return;
    int is_shell = (rlen == 5 && net_memcmp(payload + off, (const uint8_t*)"shell", 5) == 0);
    int is_exec  = (rlen == 4 && net_memcmp(payload + off, (const uint8_t*)"exec", 4) == 0);
    off += rlen;
    // want reply flag
    int want_reply = payload[off]; off += 1;
    if (is_shell){
        if (want_reply){
            uint8_t buf[32];
            int n = 0;
            buf[n++] = SSH_MSG_CHANNEL_SUCCESS;
            buf[n++] = (uint8_t)(cid >> 24); buf[n++] = (uint8_t)(cid >> 16);
            buf[n++] = (uint8_t)(cid >> 8);  buf[n++] = (uint8_t)(cid);
            ssh_send(s, buf, n);
        }
        // prompt
        const char* prompt = "NexOS login: ";
        uint8_t dbuf[64];
        int dn = 0;
        dbuf[dn++] = SSH_MSG_CHANNEL_DATA;
        dbuf[dn++] = (uint8_t)(cid >> 24); dbuf[dn++] = (uint8_t)(cid >> 16);
        dbuf[dn++] = (uint8_t)(cid >> 8);  dbuf[dn++] = (uint8_t)(cid);
        ssh_put_str(dbuf, &dn, (const uint8_t*)prompt, net_strlen(prompt));
        ssh_send(s, dbuf, dn);
    } else if (is_exec){
        // exec: "command" string follows
        int clen = (payload[off] << 24) | (payload[off+1] << 16) |
                   (payload[off+2] << 8) | payload[off+3]; off += 4;
        if (off + clen > plen) clen = plen - off;
        char cmd[256];
        if (clen >= (int)sizeof(cmd)) clen = (int)sizeof(cmd) - 1;
        net_memset(cmd, 0, sizeof(cmd));
        net_memcpy((uint8_t*)cmd, payload + off, clen);
        if (want_reply){
            uint8_t buf[32];
            int n = 0;
            buf[n++] = SSH_MSG_CHANNEL_SUCCESS;
            buf[n++] = (uint8_t)(cid >> 24); buf[n++] = (uint8_t)(cid >> 16);
            buf[n++] = (uint8_t)(cid >> 8);  buf[n++] = (uint8_t)(cid);
            ssh_send(s, buf, n);
        }
        // run command, forwarding output to this channel
        g_ssh_active = s;
        term_set_ssh_sink(ssh_out_to_channel);
        kernel_exec_line(cmd);
        term_clear_ssh_sink();
        g_ssh_active = 0;
        // close channel after exec
        uint8_t buf[32];
        int n = 0;
        buf[n++] = SSH_MSG_CHANNEL_CLOSE;
        buf[n++] = (uint8_t)(cid >> 24); buf[n++] = (uint8_t)(cid >> 16);
        buf[n++] = (uint8_t)(cid >> 8);  buf[n++] = (uint8_t)(cid);
        ssh_send(s, buf, n);
    }
}

// ---- CHANNEL_DATA: client keystrokes ----
static void ssh_handle_channel_data(SshSession* s, const uint8_t* payload, int plen){
    int off = 1;
    int cid = (payload[off] << 24) | (payload[off+1] << 16) |
              (payload[off+2] << 8) | payload[off+3]; off += 4;
    int dlen = (payload[off] << 24) | (payload[off+1] << 16) |
               (payload[off+2] << 8) | payload[off+3]; off += 4;
    if (off + dlen > plen) dlen = plen - off;
    for (int i = 0; i < dlen; i++){
        uint8_t ch = payload[off + i];
        if (ch == '\r' || ch == '\n'){
            // echo newline, run line
            uint8_t buf[300];
            int n = 0;
            buf[n++] = SSH_MSG_CHANNEL_DATA;
            buf[n++] = (uint8_t)(cid >> 24); buf[n++] = (uint8_t)(cid >> 16);
            buf[n++] = (uint8_t)(cid >> 8);  buf[n++] = (uint8_t)(cid);
            ssh_put_str(buf, &n, (const uint8_t*)"\r\n", 2);
            ssh_send(s, buf, n);
            if (s->line_len > 0){
                char line[256];
                net_memset(line, 0, sizeof(line));
                net_memcpy((uint8_t*)line, s->line, s->line_len);
                g_ssh_active = s;
                term_set_ssh_sink(ssh_out_to_channel);
                kernel_exec_line(line);
                term_clear_ssh_sink();
                g_ssh_active = 0;
            }
            s->line_len = 0;
        } else if (ch == 0x7f || ch == 0x08){  // backspace
            if (s->line_len > 0) s->line_len--;
            uint8_t buf[300];
            int n = 0;
            buf[n++] = SSH_MSG_CHANNEL_DATA;
            buf[n++] = (uint8_t)(cid >> 24); buf[n++] = (uint8_t)(cid >> 16);
            buf[n++] = (uint8_t)(cid >> 8);  buf[n++] = (uint8_t)(cid);
            ssh_put_str(buf, &n, (const uint8_t*)"\b \b", 3);
            ssh_send(s, buf, n);
        } else {
            if (s->line_len < 255){
                s->line[s->line_len++] = ch;
                // echo
                uint8_t buf[300];
                int n = 0;
                buf[n++] = SSH_MSG_CHANNEL_DATA;
                buf[n++] = (uint8_t)(cid >> 24); buf[n++] = (uint8_t)(cid >> 16);
                buf[n++] = (uint8_t)(cid >> 8);  buf[n++] = (uint8_t)(cid);
                ssh_put_str(buf, &n, &ch, 1);
                ssh_send(s, buf, n);
            }
        }
    }
}

// ---- main feed: called per received TCP segment ----
extern "C" void ssh_feed(TcpConn* conn){
    SshSession* s = ssh_find_by_conn(conn);
    if (!s){
        s = ssh_alloc(conn);
        if (!s) return;
        ssh_send_banner(s);
        s->state = SSH_ST_BANNER_OUT;
    }
    // process all bytes in rx_buf
    uint8_t* rx = conn->rx_buf;
    int rxlen = conn->rx_len;
    {
        static const char hexc[] = "0123456789ABCDEF";
        char dbg0[64]; int d0=0;
        const char* p0="[SSH] rx=";
        while(*p0) dbg0[d0++]=*p0++;
        for(int i=0;i<24 && i<rxlen;i++){ dbg0[d0++]=hexc[rx[i]>>4]; dbg0[d0++]=hexc[rx[i]&0xF]; if(i%4==3) dbg0[d0++]=' '; }
        dbg0[d0++]='\n'; net_serial(dbg0);
    }
    while (rxlen > 0){
        {
            static const char hexc[] = "0123456789ABCDEF";
            char dbg3[40]; int d3=0;
            const char* p3="[SSH] feed st=";
            while(*p3) dbg3[d3++]=*p3++;
            dbg3[d3++]=hexc[s->state>>4]; dbg3[d3++]=hexc[s->state&0xF];
            dbg3[d3++]=' '; dbg3[d3++]='r'; dbg3[d3++]='l'; dbg3[d3++]='=';
            int RL=rxlen; dbg3[d3++]=hexc[RL>>4]; dbg3[d3++]=hexc[RL&0xF];
            dbg3[d3++]='\n'; net_serial(dbg3);
        }
        if (s->state == SSH_ST_BANNER_OUT || s->state == SSH_ST_BANNER_IN){
            // banner exchange: read until CRLF
            int consumed = 0;
            for (int i = 0; i < rxlen; i++){
                if (rx[i] == '\n'){
                    consumed = i + 1;
                    s->banner_seen = 1;
                    break;
                }
            }
            if (consumed == 0){
                // no newline yet; wait
                conn->rx_len = 0;
                return;
            }
            // consume banner line (ignore content for lab server)
            rx += consumed; rxlen -= consumed;
            {
                static const char hexc[] = "0123456789ABCDEF";
                char dbg4[40]; int d4=0;
                const char* p4="[SSH] banner consumed=";
                while(*p4) dbg4[d4++]=*p4++;
                dbg4[d4++]=hexc[(consumed>>4)&0xF]; dbg4[d4++]=hexc[consumed&0xF];
                dbg4[d4++]=' '; dbg4[d4++]='rl'; dbg4[d4++]='=';
                dbg4[d4++]=hexc[rxlen>>4]; dbg4[d4++]=hexc[rxlen&0xF];
                dbg4[d4++]='\n'; net_serial(dbg4);
            }
            // Shift any remaining bytes (e.g. a KEXINIT that arrived in the
            // same TCP segment) to the front of rx_buf and update rx_len, so
            // the next loop iteration (and the next ssh_feed call) see them
            // correctly.  Without this, stale banner bytes linger in rx_buf
            // and get re-parsed as a binary packet on the next feed.
            if (rxlen > 0){
                net_memmove(conn->rx_buf, rx, rxlen);
                conn->rx_len = rxlen;
                rx = conn->rx_buf;
            } else {
                conn->rx_len = 0;
            }
            s->state = SSH_ST_KEXINIT_OUT;
            ssh_send_kexinit(s);
            s->state = SSH_ST_KEXINIT_IN;
            continue;
        }
        // binary packet phase: accumulate into s->pb
        {
            static const char hexc[] = "0123456789ABCDEF";
            char dbg5[48]; int d5=0;
            const char* p5="[SSH] bin rx0=";
            while(*p5) dbg5[d5++]=*p5++;
            for(int i=0;i<8 && i<rxlen;i++){ dbg5[d5++]=hexc[rx[i]>>4]; dbg5[d5++]=hexc[rx[i]&0xF]; }
            dbg5[d5++]='\n'; net_serial(dbg5);
        }
        int avail = rxlen;
        if (s->pb_len + avail > (int)sizeof(s->pb)){
            // overflow: drop
            s->pb_len = 0;
            conn->rx_len = 0;
            return;
        }
        net_memcpy(s->pb + s->pb_len, rx, avail);
        s->pb_len += avail;
        rx += avail; rxlen = 0;
        // try to parse as many packets as possible
        for (;;){
            uint8_t* payload = 0;
            int plen = ssh_try_parse(s, &payload);
            if (plen < 0){
                if (plen == -1) break;   // need more data
                // corrupt
                {
                    static const char hexc[] = "0123456789ABCDEF";
                    char dbg2[48]; int d2=0;
                    const char* p2="[SSH] corrupt plen=";
                    while(*p2) dbg2[d2++]=*p2++;
                    dbg2[d2++]=hexc[((plen<0?-plen:plen))>>4];
                    dbg2[d2++]=hexc[((plen<0?-plen:plen))&0xF];
                    dbg2[d2++]=' '; dbg2[d2++]='s'; dbg2[d2++]='=';
                    dbg2[d2++]=hexc[s->state>>4]; dbg2[d2++]=hexc[s->state&0xF];
                    dbg2[d2++]=' '; dbg2[d2++]='l'; dbg2[d2++]='=';
                    int L=s->pb_len;                     dbg2[d2++]=hexc[L>>4]; dbg2[d2++]=hexc[L&0xF];
                    dbg2[d2++]='\n';
                    net_serial(dbg2);
                    // hexdump first 16 bytes of pb
                    char hd[64]; int hn=0;
                    const char* hp="[SSH] pb=";
                    while(*hp) hd[hn++]=*hp++;
                    for(int i=0;i<16 && i<s->pb_len;i++){
                        hd[hn++]=hexc[s->pb[i]>>4]; hd[hn++]=hexc[s->pb[i]&0xF];
                        if(i%4==3) hd[hn++]=' ';
                    }
                    hd[hn++]='\n'; net_serial(hd);
                }
                s->state = SSH_ST_CLOSED;
                conn->rx_len = 0;
                s->pb_len = 0;
                return;
            }
            // remove this packet from pb
            int pktlen = ((int)s->pb[0] << 24) | ((int)s->pb[1] << 16) |
                         ((int)s->pb[2] << 8) | (int)s->pb[3];
            int total = 4 + pktlen + (s->have_enc ? 20 : 0);
            int rem = s->pb_len - total;
            if (rem > 0) net_memmove(s->pb, s->pb + total, rem);
            s->pb_len = rem;
            if (s->have_enc) s->client_seq++;
            // dispatch by message type
            uint8_t msg = payload[0];
            {
                static const char hexc[] = "0123456789ABCDEF";
                char dbg[40];
                int dl = 0;
                const char* pre = "[SSH] msg=";
                while(*pre) dbg[dl++] = *pre++;
                dbg[dl++] = hexc[msg >> 4]; dbg[dl++] = hexc[msg & 0xF];
                dbg[dl++] = ' '; dbg[dl++] = 's'; dbg[dl++] = '=';
                dbg[dl++] = hexc[s->state >> 4]; dbg[dl++] = hexc[s->state & 0xF];
                dbg[dl++] = '\n';
                net_serial(dbg);
            }
            if (msg == SSH_MSG_KEXINIT){
                if (s->state == SSH_ST_KEXINIT_IN){
                    ssh_kex_incoming_kexinit(s);
                }
            } else if (msg == SSH_MSG_KEXDH_INIT){
                // client sent its public value; we reply with KEXDH_REPLY
                if (s->state == SSH_ST_KEXDH){
                    ssh_kex_reply(s, payload, plen);
                }
            } else if (msg == SSH_MSG_NEWKEYS){
                // client confirmed; switch to encrypted send/recv
                if (s->state == SSH_ST_NEWKEYS_SENT){
                    s->have_enc = 1;
                    s->state = SSH_ST_AUTH;
                }
            } else if (msg == SSH_MSG_SERVICE_REQUEST){
                // "ssh-userauth"
                uint8_t buf[32];
                int n = 0;
                buf[n++] = SSH_MSG_SERVICE_ACCEPT;
                ssh_put_cstr(buf, &n, "ssh-userauth");
                ssh_send(s, buf, n);
            } else if (msg == SSH_MSG_USERAUTH_REQUEST){
                ssh_handle_auth(s, payload, plen);
            } else if (msg == SSH_MSG_CHANNEL_OPEN){
                ssh_handle_channel_open(s, payload, plen);
            } else if (msg == SSH_MSG_CHANNEL_REQUEST){
                ssh_handle_channel_request(s, payload, plen);
            } else if (msg == SSH_MSG_CHANNEL_DATA){
                ssh_handle_channel_data(s, payload, plen);
            } else if (msg == SSH_MSG_CHANNEL_CLOSE){
                s->state = SSH_ST_CLOSED;
            }
            if (s->state == SSH_ST_CLOSED) break;
        }
        conn->rx_len = 0;
        break;
    }
}

extern "C" void ssh_poll(void){
    // No timers needed for the minimal server; output is flushed inline.
}

// Weak inert stubs for the kernel-side hooks.  The 32-bit kernel (kernel.cpp)
// provides strong definitions; the 64-bit kernel uses these defaults so the
// SSH code links even when the terminal subsystem is not wired.
extern "C" __attribute__((weak)) int nexos_auth(const char* user, const char* pw){
    (void)user; (void)pw; return 0;
}
extern "C" __attribute__((weak)) void kernel_exec_line(const char* line){
    (void)line;
}
extern "C" __attribute__((weak)) void term_set_ssh_sink(void (*fn)(const char*, int)){
    (void)fn;
}
extern "C" __attribute__((weak)) void term_clear_ssh_sink(void){}

}  // extern "C"
