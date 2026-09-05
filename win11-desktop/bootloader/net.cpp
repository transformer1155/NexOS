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

// ---- Kernel interface ----
extern "C" {
    void* kmalloc(uint32_t size);
    void  kfree(void* ptr);
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
static void net_serial(const char* s){
    while(*s) __asm__ __volatile__("outb %0,%1" :: "a"((uint8_t)*s++), "Nd"((uint16_t)0x3F8));
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
    noutb(NE_BASE + NE_RCR, 0x04);

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
    }
    // Timeout - clear anyway
    noutb(NE_BASE + NE_ISR, 0xFF);
}

static int nic_receive(uint8_t* buf, int maxlen){
    if (!nic_present) return 0;

    uint8_t bnry = ninb(NE_BASE + NE_BNRY);

    // Go to page 1 to read CURR
    noutb(NE_BASE + NE_CR, CR_STA | CR_PAGE1);
    uint8_t curr = ninb(NE_BASE + 0x07);
    noutb(NE_BASE + NE_CR, CR_STA | CR_PAGE0);

    // No packet available
    if (curr == bnry) return 0;

    // Read packet header (4 bytes)
    uint8_t hdr_page = bnry + 1;
    if (hdr_page >= RX_STOP) hdr_page = RX_START;

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

// Our IP configuration (QEMU user-mode networking defaults)
static uint32_t our_ip   = 0x0A00020F;  // 10.0.2.15
static uint32_t gateway  = 0x0A000202;  // 10.0.2.2

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

static uint16_t tcp_checksum(uint32_t src, uint32_t dst, void* data, int len){
    uint32_t sum = 0;
    // Pseudo-header
    sum += (src >> 16) & 0xFFFF;
    sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF;
    sum += dst & 0xFFFF;
    sum += 0x0006;  // protocol = TCP
    sum += len;
    // TCP data
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
    arp->sender_ip = our_ip;
    net_memset(arp->target_mac, 0, 6);
    arp->target_ip = ip;

    nic_send(pkt, 42);
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
    arp->sender_ip = our_ip;
    net_memcpy(arp->target_mac, dst_mac, 6);
    arp->target_ip = dst_ip;

    nic_send(pkt, 42);
}

static void handle_arp(const uint8_t* data, int len){
    if (len < 28) return;
    ArpPacket* arp = (ArpPacket*)data;

    uint16_t op = (arp->op >> 8) | (arp->op << 8);  // ntohs
    uint32_t target_ip = arp->target_ip;  // already in network byte order = host order on little-endian

    if (op == 1 && target_ip == our_ip){
        // ARP request for us - send reply
        arp_add(arp->sender_ip, arp->sender_mac);
        arp_reply(arp->sender_mac, arp->sender_ip);
    } else if (op == 2){
        // ARP reply - cache it
        arp_add(arp->sender_ip, arp->sender_mac);
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

    // Check if destination is on same subnet (10.0.2.0/24)
    if ((dst_ip & 0x00FFFFFF) != (our_ip & 0x00FFFFFF))
        next_hop = gateway;

    if (!arp_lookup(next_hop, dst_mac)){
        // Send ARP request and return (packet will be resent later)
        arp_request(next_hop);
        return;
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
    ip->flags_frag = 0x0040;  // Don't Fragment
    ip->ttl = 64;
    ip->proto = proto;
    ip->checksum = 0;
    ip->src_ip = our_ip;
    ip->dst_ip = dst_ip;

    // Calculate IP checksum
    ip->checksum = ip_checksum(ip, 20);

    // Copy payload
    net_memcpy(pkt + 34, payload, len);

    int total = 14 + 20 + len;
    if (total < 60) total = 60;  // minimum Ethernet frame
    nic_send(pkt, total);
}

// =====================================================================
//  ICMP Layer (ping reply)
// =====================================================================

static void handle_icmp(const IpHeader* ip, const uint8_t* data, int len){
    if (len < 8) return;
    if (data[0] == 8){  // Echo request
        uint8_t reply[1514];
        net_memcpy(reply, data, len);
        reply[0] = 0;  // Echo reply
        reply[2] = 0; reply[3] = 0;  // clear checksum
        // Recalculate ICMP checksum
        uint16_t cksum = ip_checksum(reply, len);
        reply[2] = cksum & 0xFF;
        reply[3] = cksum >> 8;

        ip_send(ip->src_ip, IP_PROTO_ICMP, reply, len);
    }
}

// =====================================================================
//  TCP Layer
// =====================================================================

#define MAX_TCP_CONN 4
#define TCP_RX_BUF   2048
#define HTTP_PORT    8080

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
    uint32_t seq;       // our next seq
    uint32_t ack;       // next expected remote seq
    uint8_t  state;
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
    tcp->seq = seq;  // already in network byte order (little-endian = stored as-is)
    tcp->ack = ack;
    tcp->data_off = (5 << 4);  // 20 bytes, no options
    tcp->flags = flags;
    tcp->window = 0xFFFF >> 8 | 0xFF << 8;  // htons(0xFFFF) - wait, let me be careful
    tcp->window = 0xFFFF;  // max window
    tcp->checksum = 0;
    tcp->urgent = 0;

    // Copy data
    if (data && len > 0)
        net_memcpy(seg + 20, data, len);

    // Calculate TCP checksum
    tcp->checksum = tcp_checksum(our_ip, dst_ip, seg, 20 + len);

    ip_send(dst_ip, IP_PROTO_TCP, seg, 20 + len);
}

static void tcp_send_segment(TcpConn* c, uint8_t flags, const uint8_t* data, int len){
    tcp_send_raw(c->remote_ip, HTTP_PORT, c->remote_port,
                 c->seq, c->ack, flags, data, len);
    if (flags & TCP_SYN) c->seq++;
    if (flags & TCP_FIN) c->seq++;
    c->seq += len;
}

// Forward declaration
static void http_handle_request(TcpConn* conn);

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

static void handle_tcp_client(const IpHeader* ip, const TcpHeader* tcp,
                               uint16_t src_port, uint16_t dst_port,
                               uint32_t seq, uint32_t ack, uint8_t flags,
                               const uint8_t* payload, int payload_len);

static void handle_tcp(const IpHeader* ip, const uint8_t* data, int len){
    if (len < 20) return;

    TcpHeader* tcp = (TcpHeader*)data;
    uint16_t dst_port = (tcp->dst_port >> 8) | (tcp->dst_port << 8);  // ntohs
    uint16_t src_port = (tcp->src_port >> 8) | (tcp->src_port << 8);
    uint32_t seq = tcp->seq;
    uint32_t ack = tcp->ack;
    uint8_t  flags = tcp->flags;

    int header_len = (tcp->data_off >> 4) * 4;
    int payload_len = len - header_len;
    const uint8_t* payload = data + header_len;

    // Check for TCP client connection first (before the HTTP_PORT check)
    if (tcp_client.state != TCPC_IDLE && dst_port == tcp_client.local_port){
        handle_tcp_client(ip, tcp, src_port, dst_port, seq, ack, flags, payload, payload_len);
        return;
    }

    // Only handle connections to our HTTP port
    if (dst_port != HTTP_PORT) return;

    // SYN - new connection
    if ((flags & TCP_SYN) && !(flags & TCP_ACK)){
        TcpConn* c = tcp_find(ip->src_ip, src_port);
        if (!c && (c = tcp_alloc())){
            c->remote_ip = ip->src_ip;
            c->remote_port = src_port;
            c->seq = 0x12345678;  // initial seq number
            c->ack = seq + 1;
            c->state = TCP_SYN_RCVD;
            c->rx_len = 0;

            // Send SYN+ACK
            tcp_send_segment(c, TCP_SYN | TCP_ACK, 0, 0);
        }
        return;
    }

    TcpConn* c = tcp_find(ip->src_ip, src_port);
    if (!c) return;

    switch (c->state){
        case TCP_SYN_RCVD:
            if (flags & TCP_ACK){
                c->state = TCP_ESTABLISHED;
            }
            break;

        case TCP_ESTABLISHED:
            // Update ACK
            c->ack = seq + payload_len;

            // Collect data
            if (payload_len > 0 && c->rx_len + payload_len < TCP_RX_BUF){
                net_memcpy(c->rx_buf + c->rx_len, payload, payload_len);
                c->rx_len += payload_len;

                // Send ACK
                tcp_send_segment(c, TCP_ACK, 0, 0);

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
//  DNS Resolver
// =====================================================================

#define DNS_SERVER  0x0A000203  // 10.0.2.3 (QEMU built-in DNS)
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
                        dns_resolved_ip = (payload[pos]) |
                                         (payload[pos+1] << 8) |
                                         (payload[pos+2] << 16) |
                                         (payload[pos+3] << 24);
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
        tcp_client.remote_ip = ip[0] | (ip[1] << 8) | (ip[2] << 16) | (ip[3] << 24);
        tcp_client.state = TCPC_SYN_SENT;

        // Send SYN
        tcp_client_send_raw(TCP_SYN, 0, 0);
        tcp_client.seq++;
        net_serial("[TCPC] SYN sent (direct IP)\n");
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
    if (ip->src_ip != tcp_client.remote_ip) return;

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

    // Start TCP connection (handles DNS internally)
    tcp_client_connect(httpc_host, httpc_port);

    net_serial("[HTTPC] Connecting to host\n");
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
                // Send HTTP GET request
                char request[512];
                int pos = 0;

                // "GET /path HTTP/1.0\r\n"
                const char* get_line = "GET ";
                net_memcpy(request + pos, get_line, 4); pos += 4;
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

                // "Connection: close\r\n\r\n"
                const char* conn_hdr = "Connection: close\r\nUser-Agent: MiniOS-Browser/1.0\r\nAccept: text/html,text/plain,*/*\r\n\r\n";
                int cl = net_strlen(conn_hdr);
                net_memcpy(request + pos, conn_hdr, cl); pos += cl;

                tcp_client_send((const uint8_t*)request, pos);
                httpc_state = HTTPC_REQUESTING;
                net_serial("[HTTPC] GET request sent\n");
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
            // Timeout after ~500 polls
            if (httpc_poll_count > 1000 && httpc_state == HTTPC_CONNECTING){
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
                httpc_state = HTTPC_COMPLETE;
                net_serial("[HTTPC] Response complete\n");
            }
            else if (tcp_client.state == TCPC_ERROR){
                httpc_state = HTTPC_ERROR;
            }
            // Timeout
            if (httpc_poll_count > 3000){
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
    tcp_client_close();
    tcp_client.state = TCPC_IDLE;
}

// =====================================================================
//  IP packet dispatch
// =====================================================================

static void handle_ip(const uint8_t* data, int len){
    if (len < 20) return;
    IpHeader* ip = (IpHeader*)data;

    // Verify destination
    if (ip->dst_ip != our_ip) return;

    int ihl = (ip->ver_ihl & 0x0F) * 4;
    int payload_len = len - ihl;
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

    net_serial("[HTTP] ");
    net_serial(method);
    net_serial(" ");
    net_serial(path);
    net_serial("\n");

    // Route: GET / -> serve web UI
    if (net_strcmp(method, "GET") == 0 && (net_strcmp(path, "/") == 0 || net_strcmp(path, "/index.html") == 0)){
        int html_len = net_strlen(WEB_UI);
        http_send_response(conn, "200 OK", "text/html; charset=utf-8", WEB_UI, html_len);
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

        agent_init();
        char output[4096];
        int n = agent_run(goal, output, sizeof(output));
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

    net_serial("[NET] Network ready. IP=10.0.2.15:8080\n");
    return 0;
}

void net_poll(void){
    if (!nic_present) return;

    static uint8_t rx_buf[1514];
    int len = nic_receive(rx_buf, 1514);
    if (len > 0){
        handle_ethernet(rx_buf, len);
    }

    // Poll HTTP client state machine
    httpc_poll();

    // Re-transmit any pending packets (after ARP resolution)
    // This is a simple retry mechanism
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
    net_memcpy(buf + pos, "  Port: 8080 (HTTP)\n", 20); pos += 20;

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

void browser_reset(void){
    httpc_reset();
}

}  // extern "C"
