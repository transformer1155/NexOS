//  hda64.cpp  -  Intel HD Audio (HDA) output driver, 64-bit kernel only.
//
//  WHY 64-BIT ONLY
//  ---------------
//  The 32-bit kernel's load window is 588,800 bytes and the current build sits
//  at 588,732 -- 68 bytes of headroom.  A sound driver is several KiB, so it
//  physically cannot go there without first deleting a compatibility layer.
//  The 64-bit kernel has ~20 KB of .bss slack and is the kernel that actually
//  runs the Win11 desktop, so the driver lives here and only here.
//
//  THE DEVICE
//  ----------
//  VirtualBox 7.x presents Intel ICH6 HD Audio at 8086:2668 (class 0403),
//  visible on the guest PCI bus as 00:05.0.  QEMU's -soundhw hda presents the
//  same controller, so one driver serves both.
//
//  WHAT THIS DRIVER DOES (and deliberately does NOT do)
//  ---------------------------------------------------
//  * finds the controller on the PCI bus, enables MMIO + bus mastering,
//  * maps BAR0 (identity-mapped in long mode: 0..8 GiB of physical memory is
//    covered by the 1 GiB pages switch32to64.asm installs, so a BAR at
//    0xF0000000 is directly dereferenceable -- no page-table work needed),
//  * resets the codec, enumerates the codec's audio widget chain, finds an
//    output converter, and un-mutes / sets the volume on the path to the pin,
//  * allocates a CORB/RIRB pair and issues verbs through them,
//  * sets up one output stream descriptor with a buffer descriptor list,
//  * and exposes hda_play_pcm_chunk() so the Audio Player can feed PCM.
//
//  Interrupts are NOT used.  The kernel has no IRQ dispatch framework (there is
//  no irq_install in kernel64.cpp), and polling the stream's LPIB register is
//  sufficient: hda_write_frame() waits for the stream to drain a buffer, which
//  is the same flow a demanding real application would use anyway.  When an
//  interrupt framework appears, the wait can become an IRQ sleep with no other
//  change to the driver.
//
//  This is written against the Intel HDA specification's register map, not
//  guessed: every offset below is from the spec's section 3.3 (controller
//  registers) or 4.3 (stream descriptors).

#include <stdint.h>

// C++ LINKAGE, NOT extern "C".  kernel64.cpp defines these as plain C++
// functions (serial_puts / serial_puthex / serial_putdec with name mangling,
// around line 90-121), so declaring them extern "C" here made the linker look
// for unmangled names, find nothing, and bind them to address 0 -- which
// check-k64 caught with:
//   "kernel64 binds these symbols to address 0 ... Calling one jumps to 0
//    -> #UD -> #GP -> #DF -> triple fault."
// Declared FIRST because hda_log (below) calls serial_puts.
void serial_puts(const char* s);
void serial_putdec(uint32_t v);
void serial_puthex(uint8_t b);

// ---- Diagnostic log buffer -------------------------------------------
// WHY NOT JUST serial_puts ITSELF:
// the HDA probe runs while other boot code is also writing to the UART, and
// the port is not locked.  The result was interleaved garbage on the serial
// line ("[Apb cngCf eorl", "[H]RecC", "[H]pb:rstgotle") that made a failed
// register read indistinguishable from a broken log -- diagnosis was blocked
// on a logging bug, twice.
//
// So the driver keeps its OWN append-only buffer instead of racing for the
// port.  serial_puts is still called on a best-effort basis so a human tailing
// the log sees something, but the AUTHORITATIVE record is this buffer, which
// hda_report() hands to the managed side where no other writer can interleave.
#define HDA_LOG_MAX 1024
static char hda_logbuf[HDA_LOG_MAX];
static int  hda_loglen = 0;

static void hda_log(const char* s){
    if (!s) return;
    for (int i = 0; s[i]; i++){
        if (hda_loglen < HDA_LOG_MAX - 1) hda_logbuf[hda_loglen++] = s[i];
    }
    hda_logbuf[hda_loglen] = 0;
    serial_puts(s);                       // best effort; may interleave
}

// Numbers must go through hda_log too.  serial_puthex writes straight to the
// UART and never reaches hda_logbuf, which is why the first trace held labels
// with empty values ("GCAP=" then nothing) and told us nothing.
static void hda_lognum(uint32_t v, int nibbles){
    char h[] = "0123456789ABCDEF";
    char t[16]; int p = 0;
    t[p++] = '0'; t[p++] = 'x';
    for (int i = (nibbles - 1) * 4; i >= 0 && p < 15; i -= 4)
        t[p++] = h[(v >> i) & 0xF];
    t[p] = 0;
    hda_log(t);
}
static void hda_logdec(int v){
    char t[16]; int p = 0;
    if (v < 0){ t[p++] = '-'; v = -v; }
    if (v == 0) t[p++] = '0';
    else {
        char b[12]; int n = 0;
        while (v > 0 && n < 11){ b[n++] = (char)('0' + (v % 10)); v /= 10; }
        while (n > 0 && p < 15) t[p++] = b[--n];
    }
    t[p] = 0;
    hda_log(t);
}

// (serial_* declarations moved above hda_log)

static inline void outl(uint16_t p, uint32_t v){ __asm__ __volatile__("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t inl(uint16_t p){ uint32_t v; __asm__ __volatile__("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outw(uint16_t p, uint16_t v){ __asm__ __volatile__("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void outb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }

// ---- PCI config access (mirrors kernel64's vpci_* helpers) --------------
static uint32_t pci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg){
    outl(0xCF8, 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) |
                 ((uint32_t)fn<<8) | (reg & 0xFC));
    return inl(0xCFC);
}
static void pci_wr32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val){
    outl(0xCF8, 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) |
                 ((uint32_t)fn<<8) | (reg & 0xFC));
    outl(0xCFC, val);
}

// ---- Controller register offsets (HDA spec 3.3) ------------------------
#define HDA_GCAP        0x00    // global capabilities
#define HDA_VMIN        0x02    // minor version
#define HDA_VMAJ        0x03    // major version
#define HDA_OUTPAY      0x04
#define HDA_INPAY       0x06
#define HDA_GCTL        0x08    // global control (bit0 = CRST)
#define HDA_STATESTS    0x0E    // state change (codec presence)
#define HDA_INTCTL      0x20
#define HDA_INTSTS      0x24
#define HDA_WALCLK      0x30
#define HDA_SSYNC       0x38
#define HDA_CORBLBASE   0x40
#define HDA_CORBUBASE   0x44
#define HDA_CORBWP      0x48
#define HDA_CORBRP      0x4A
#define HDA_CORBCTL     0x4C
#define HDA_CORBSTS     0x4D
#define HDA_CORBSIZE    0x4E
#define HDA_RIRBLBASE   0x50
#define HDA_RIRBUBASE   0x54
#define HDA_RIRBWP       0x58
#define HDA_RINTCNT      0x5A
#define HDA_RIRBCTL      0x5C
#define HDA_RIRBSTS      0x5D
#define HDA_RIRBSIZE     0x5E
#define HDA_DPLBASE     0x70
#define HDA_DPUBASE     0x74
#define HDA_SD_BASE     0x80    // first stream descriptor
#define HDA_SD_STRIDE   0x20    // each descriptor is 0x20 bytes

// Stream descriptor registers (offsets within the 0x20 block)
#define SD_CTL          0x00
#define SD_STS          0x03
#define SD_LPIB         0x04
#define SD_CBL          0x08    // cyclic buffer length (bytes)
#define SD_LVI          0x0C    // last valid index
#define SD_FIFOW        0x0E
#define SD_FMT          0x12    // stream format
#define SD_BDPL         0x18    // buffer descriptor list base (low)
#define SD_BDPU         0x1C

// SD_CTL bits
#define SDCTL_RUN       (1u<<1)
#define SDCTL_RESET     (1u<<0)
#define SDCTL_STRIPE(v) (((v)&3)<<16)
#define SDCTL_TAG(v)    (((v)&0xF)<<20)     // stream tag, bits 23:20 (spec 3.3.30)
#define SDCTL_TAG_MASK  (0xFu<<20)
#define SDCTL_DEIE      (1u<<4)

// ---- Codec (widget) access through CORB/RIRB ---------------------------
// A verb is one 32-bit command: [codec addr:4][node id:8][verb:12][payload:8]
static uint32_t hda_mmio_base = 0;      // BAR0 VA (identity == phys)
static uint32_t hda_corb = 0;           // VA of CORB ring (identity mapped)
static uint32_t hda_rirb = 0;           // VA of RIRB ring
static uint32_t hda_bdl  = 0;           // VA of buffer descriptor list
static uint32_t hda_buf  = 0;           // VA of the PCM ring buffer
static int      hda_ok   = 0;
static int      hda_codec = -1;
static int      hda_dac   = -1;         // output converter node id
static int      hda_pin   = -1;         // output pin widget node id
static int      hda_sd    = -1;         // stream descriptor index used
static int      hda_buf_len = 0;
static int      hda_corb_entries = 0;

// ---- Register access ---------------------------------------------------
// The controller can be driven through EITHER a memory BAR (MMIO) or an I/O
// BAR.  Measured on VirtualBox 7.2.20: PCI command went 0x0002 -> 0x0007
// (memory decode ON, verified by readback), BAR0 was 0xF0404000, and GCAP
// still read back 0xFFFFFFFF -- so on that host the memory window does not
// answer even though the device claims it.  The controller also exposes an
// I/O BAR (0x4401 -> base 0x4400), so this driver must be able to use either.
//
// Everything below accesses registers through these helpers, which pick the
// live interface.  Doing it with macros rather than direct pointer casts is
// what makes the I/O path possible without touching 40 call sites.
static uint32_t hda_io_base = 0;
static int      hda_use_io  = 0;

static inline uint32_t hda_rd32(uint32_t off){
    if (hda_use_io) return inl((uint16_t)(hda_io_base + off));
    return *(volatile uint32_t*)((uint64_t)hda_mmio_base + off);
}
static inline uint16_t hda_rd16(uint32_t off){
    if (hda_use_io){
        uint32_t v = inl((uint16_t)(hda_io_base + (off & ~3u)));
        return (uint16_t)(v >> ((off & 3u) * 8));
    }
    return *(volatile uint16_t*)((uint64_t)hda_mmio_base + off);
}
static inline uint8_t hda_rd8(uint32_t off){
    if (hda_use_io){
        uint32_t v = inl((uint16_t)(hda_io_base + (off & ~3u)));
        return (uint8_t)(v >> ((off & 3u) * 8));
    }
    return *(volatile uint8_t*)((uint64_t)hda_mmio_base + off);
}
static inline void hda_wr32(uint32_t off, uint32_t v){
    if (hda_use_io){ outl((uint16_t)(hda_io_base + off), v); return; }
    *(volatile uint32_t*)((uint64_t)hda_mmio_base + off) = v;
}
static inline void hda_wr16(uint32_t off, uint16_t v){
    if (hda_use_io){
        uint32_t sh = (off & 3u) * 8;
        uint32_t cur = inl((uint16_t)(hda_io_base + (off & ~3u)));
        cur = (cur & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
        outl((uint16_t)(hda_io_base + (off & ~3u)), cur);
        return;
    }
    *(volatile uint16_t*)((uint64_t)hda_mmio_base + off) = v;
}
static inline void hda_wr8(uint32_t off, uint8_t v){
    if (hda_use_io){
        uint32_t sh = (off & 3u) * 8;
        uint32_t cur = inl((uint16_t)(hda_io_base + (off & ~3u)));
        cur = (cur & ~(0xFFu << sh)) | ((uint32_t)v << sh);
        outl((uint16_t)(hda_io_base + (off & ~3u)), cur);
        return;
    }
    *(volatile uint8_t*)((uint64_t)hda_mmio_base + off) = v;
}

// Ask the kernel's page allocator for n CONTIGUOUS identity-mapped pages and
// return their physical (== virtual) base.  Returns 0 on failure.
extern "C" uint32_t pmm_alloc_page();

static uint32_t alloc_pages(int n){
    uint32_t first = 0, prev = 0;
    for (int i = 0; i < n; i++){
        uint32_t p = pmm_alloc_page();
        if (!p) return 0;                        // caller frees nothing: boot-time only
        if (i == 0) first = p;
        else if (p != prev + 4096u){
            // Not contiguous.  The HDA rings do not strictly need physical
            // contiguity (the BDL can point anywhere), but the CORB/RIRB
            // pointers are single addresses, so give up honestly rather than
            // hand the device a broken run.
            hda_log("[HDA] page allocator returned a non-contiguous run\n");
            return 0;
        }
        prev = p;
    }
    return first;
}

// A BOUNDED short spin.  The first version used 200000 iterations here AND
// called it inside hda_verb's 20000-iteration wait, so the worst case was
// ~4 billion iterations -- which is not a timeout, it is a hang, and that is
// exactly what the VirtualBox boot showed (serial stopped dead after
// "[K64-A] Initializing HD Audio...").  The codec answers a verb within a few
// hundred frames of the 44.1 kHz clock (~10 ms worst case), so 20000 iterations
// of this inner spin per verb attempt and 500 attempts is ample.
static void hda_delay(void){
    for (volatile uint32_t i = 0; i < 2000; i++) { }
}

// Issue a verb and return the response, or 0xFFFFFFFF on timeout.
//
// Two ordering bugs were fixed here, and either one alone makes EVERY verb
// fail with 0xFFFFFFFF -- which reads exactly like "the codec is dead":
//
//  1. The old code read RIRBWP *after* writing CORBWP.  A codec can answer
//     within microseconds, so rp0 could already include the reply and the
//     "did the pointer move?" test then waits forever for a SECOND reply that
//     never comes.  The read pointer must be sampled BEFORE the command is
//     posted.
//
//  2. On entry the read pointer is reset to the value the controller wrote,
//     so a stale, never-cleared RIRB read index can point outside the ring.
//     Snapshotting first and then waiting for a *specific* advance is the only
//     ordering that works on every implementation.
static uint32_t hda_verb(uint32_t verb){
    if (hda_corb_entries <= 0) return 0xFFFFFFFFu;

    // --- sample the response pointer BEFORE posting the command ----------
    uint16_t rp0 = (uint16_t)(hda_rd16(HDA_RIRBWP) & 0xFF);

    // --- append the verb to the CORB ------------------------------------
    uint16_t wp = (uint16_t)(hda_rd16(HDA_CORBWP) & 0xFF);
    uint16_t next = (uint16_t)((wp + 1) % (uint32_t)hda_corb_entries);
    ((volatile uint32_t*)(uint64_t)hda_corb)[next] = verb;
    hda_wr16(HDA_CORBWP, next);

    // --- wait for the controller to publish a NEW response --------------
    for (int spin = 0; spin < 500; spin++){
        uint16_t rp = (uint16_t)(hda_rd16(HDA_RIRBWP) & 0xFF);
        if (rp != rp0){
            // RIRB entries are 8 bytes: [response:4][extended:4].
            return ((volatile uint32_t*)(uint64_t)hda_rirb)[(uint32_t)rp * 2];
        }
        hda_delay();
    }
    return 0xFFFFFFFFu;
}

// Issue a verb whose payload is the FULL 16 bits (SET_CONVERTER_FORMAT needs
// this; hda_verb's 8-bit payload field cannot express it).
//
// The HDA command word for the 12-bit verbs is
//   [codec addr:4][node id:8][verb:12][payload:16]
// and the low 16 bits are the payload, so a 32-bit command cannot hold both a
// 12-bit verb and 16 payload bits -- the spec handles this by using the
// 4-bit-verb form with the payload in bits 19:4.  SET_CONVERTER_FORMAT (0x200)
// is a 12-bit verb, so it is issued as: verb<<8 | (payload >> 8), then the
// low half as a separate 4-bit-verb command with payload bits 19:4.
static uint32_t hda_verb_raw_real(uint8_t codec, uint8_t node, uint16_t v, uint16_t payload){
    // High byte of the format goes into the 8-bit payload of the 12-bit verb.
    uint32_t hi = ((uint32_t)codec << 28) | ((uint32_t)node << 20) |
                  ((uint32_t)v << 8) | (uint32_t)(payload >> 8);
    hda_verb(hi);
    // Low byte goes through the 4-bit-verb form (payload in bits 19:4).
    uint32_t lo = ((uint32_t)codec << 28) | ((uint32_t)node << 20) |
                  ((uint32_t)v << 16) | ((uint32_t)(payload & 0xFF) << 4);
    return hda_verb(lo);
}

// ---- Verb constructors -------------------------------------------------
#define VERB_GET_PARAMETER      0xF00
#define VERB_SET_CONV_FMT       0x200   // SET_CONVERTER_FORMAT (16-bit payload)
#define VERB_SET_CONV_STREAM    0x706   // SET_CONVERTER_STREAM_CHANNEL
#define VERB_SET_CONV_STREAMID  0x706
#define VERB_SET_PIN_SENSE      0xF09   // (read) pin presence detect
#define VERB_SET_STREAM_CHANNEL 0x706   // SET_STREAM_FORMAT is 0x200 on a DAC;
                                        // channel select is 0x706
#define VERB_SET_PIN_WIDGET_CTL 0x707
#define VERB_SET_AMP_MUTE       0x3     // SET_AMP_GAIN_MUTE (payload: mute bit)
#define VERB_GET_AMP_GAIN       0xB
#define VERB_GET_CONN_LIST      0xF02
#define VERB_GET_SUBNODE_COUNT  0xF04
#define VERB_SET_POWER_STATE    0x705
#define VERB_GET_CONFIG_DEFAULT 0xF1C

static uint32_t verb(uint8_t codec, uint8_t node, uint16_t v, uint8_t payload){
    return ((uint32_t)codec << 28) | ((uint32_t)node << 20) |
           ((uint32_t)v << 8) | (uint32_t)payload;
}

// Widget type lives in bits 20..23 of the audio widget capabilities parameter.
#define WIDGET_TYPE(caps) (((caps) >> 20) & 0xF)
#define WTYPE_AUDIO_OUT  0x0
#define WTYPE_AUDIO_IN   0x1
#define WTYPE_MIXER      0x2
#define WTYPE_SELECTOR   0x3
#define WTYPE_PIN        0x4
#define WTYPE_POWER      0x5
#define WTYPE_VOLUME     0x6

static const char* widget_type_name(int t){
    switch (t){
        case WTYPE_AUDIO_OUT: return "audio output";
        case WTYPE_AUDIO_IN:  return "audio input";
        case WTYPE_MIXER:     return "mixer";
        case WTYPE_SELECTOR:  return "selector";
        case WTYPE_PIN:       return "pin";
        case WTYPE_POWER:     return "power";
        case WTYPE_VOLUME:    return "volume";
    }
    return "other";
}

// ---------------------------------------------------------------------
//  Probe
// ---------------------------------------------------------------------
extern "C" int hda_init(void){
    if (hda_ok) return 1;

    hda_log("[HDA] probe: scanning PCI for the controller\n");

    // ---- 1) locate the controller on the PCI bus ---------------------
    int found = 0; uint8_t fdev = 0;
    for (uint8_t dev = 0; dev < 32 && !found; dev++){
        uint32_t id = pci_rd32(0, dev, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        uint16_t ven = (uint16_t)(id & 0xFFFF);
        uint16_t did = (uint16_t)(id >> 16);
        // 8086:2668 = ICH6 HDA.  Also accept the generic ICH HDA ids so this
        // keeps working if VirtualBox/QEMU report a cousin part.
        if (ven == 0x8086 && (did == 0x2668 || did == 0x27D8 || did == 0x269A ||
                              did == 0x2841 || did == 0x293E || did == 0x8118)){
            found = 1; fdev = dev;
        }
    }
    if (!found){
        hda_log("[HDA] no Intel HD Audio controller on the PCI bus\n");
        return 0;
    }

    // ---- 2) enable MMIO decode + bus mastering -----------------------
    uint32_t cmd_before = pci_rd32(0, fdev, 0, 0x04);
    hda_log("[HDA] PCI command before=");
    hda_lognum((uint32_t)cmd_before, 4);
    hda_log("\n");
    uint32_t cmd = cmd_before | 0x01u | 0x02u | 0x04u;
    pci_wr32(0, fdev, 0, 0x04, cmd);
    // READ IT BACK.  Writing the command register and assuming it stuck is the
    // single most common way an MMIO driver "mysteriously" reads 0xFF: if
    // memory-space decode is still off, every BAR read floats to all ones.
    uint32_t cmd_after = pci_rd32(0, fdev, 0, 0x04);
    hda_log("[HDA] PCI command after =");
    hda_lognum((uint32_t)cmd_after, 4);
    hda_log("\n");
    if (!(cmd_after & 0x02u)){
        hda_log("[HDA] memory-space decode did NOT stick; MMIO is unusable\n");
        return 0;
    }

    // ---- 2b) dump every BAR so a bad address is visible, not guessed ----
    hda_log("[HDA] BARs:");
    for (int b = 0; b < 6; b++){
        uint32_t v = pci_rd32(0, fdev, 0, (uint8_t)(0x10 + b * 4));
        hda_log(" ");
        hda_lognum((uint32_t)v, 8);
    }
    hda_log("\n");

    uint32_t bar0 = pci_rd32(0, fdev, 0, 0x10) & 0xFFFFFFF0u;

    // VALIDATE BEFORE DEREFERENCING.  A BAR is only usable when it is a real
    // assigned window.  Dereferencing an unassigned one is not a soft failure:
    // 0xFFFFFFFF / 0xFFFFFFF0 puts every "register write" into the PCI/BIOS
    // region and the machine hangs on the first store -- which is precisely
    // what the VirtualBox boot did (serial went to garbled bytes right after
    // "[K64-A] Initializing HD Audio..." and the guest stopped executing).
    //
    // The masks below are the ones the PCI spec guarantees: bit0 must be 0 for
    // a memory BAR, the low 4 bits are reserved, and an all-ones read means the
    // device did not implement (or was never assigned) the BAR.
    if (bar0 == 0 || bar0 == 0xFFFFFFF0u || bar0 == 0xFFFFFF00u ||
        (bar0 & 0xF) != 0){
        hda_log("[HDA] BAR0 is not a usable assigned window; refusing to touch it\n");
        return 0;
    }
    // The controller lives below 4 GiB, and long mode identity-maps the low
    // 8 GiB, so any legal BAR0 is directly dereferenceable.
    if (bar0 >= 0x100000000u || bar0 < 0x1000u){
        hda_log("[HDA] BAR0 out of the identity-mapped window; skipping\n");
        return 0;
    }
    // Read back to confirm something actually answers there.
    //
    // GCAP is a 16-BIT register at 0x00; the two bytes after it are VMIN and
    // VMAJ (8-bit each).  A 32-bit read therefore returns
    // gcap | (vmin<<16) | (vmaj<<24), which is not a clean capability word and
    // was the wrong thing to validate against.  Read the 16-bit register.
    uint16_t gcap_test = *(volatile uint16_t*)((uint64_t)bar0 + HDA_GCAP);
    hda_log("[HDA] BAR0 readback: GCAP=");
    hda_lognum((uint32_t)gcap_test, 4);
    hda_log("\n");
    if (gcap_test == 0xFFFFu || gcap_test == 0){
        // MMIO did not answer.  VirtualBox exposes ICH6 HDA with BOTH a memory
        // BAR and an I/O BAR (the 0x4401 in the BAR dump is an I/O BAR at
        // 0x4400).  If the memory window is not decoding -- which is exactly
        // what an all-ones GCAP read means -- the I/O window is a second,
        // independent way in.  Trying it turns "MMIO is broken" from a guess
        // into a measurement.
        hda_log("[HDA] MMIO dead (GCAP=FFFF); trying the I/O BAR\n");
        uint32_t io_bar = 0;
        for (int b = 0; b < 6; b++){
            uint32_t v = pci_rd32(0, fdev, 0, (uint8_t)(0x10 + b * 4));
            if ((v & 1u) && (v & 0xFFFCu)){ io_bar = v & 0xFFFCu; break; }
        }
        if (!io_bar){
            hda_log("[HDA] no I/O BAR either; controller unusable\n");
            return 0;
        }
        hda_log("[HDA] I/O BAR base=0x");
        hda_lognum((uint32_t)io_bar, 4);
        hda_log("\n");
        hda_io_base = io_bar;
        hda_use_io = 1;
        // Read GCAP through the I/O window to prove it answers.
        uint32_t gcap_io = inl((uint16_t)(io_bar + HDA_GCAP));
        hda_log("[HDA] I/O GCAP=");
        hda_lognum((uint32_t)gcap_io, 8);
        hda_log("\n");
        if ((gcap_io & 0xFFFFu) == 0xFFFFu){
            hda_log("[HDA] I/O window dead too; no usable register interface\n");
            return 0;
        }
        hda_log("[HDA] register access via the I/O BAR\n");
        hda_mmio_base = 0;                  // all access goes through HDA_RD/HDA_WR
    } else {
        hda_mmio_base = bar0;
    }
    hda_log("[HDA] controller at 00:");
    hda_logdec((int)(fdev));
    hda_log(".0  BAR0=");
    hda_lognum((uint32_t)bar0, 8);
    hda_log("\n");

    hda_log("[HDA] probe: resetting controller\n");
    // ---- 3) reset the controller -------------------------------------
    hda_wr32(HDA_GCTL, 0);                       // clear CRST
    hda_delay();
    for (int i = 0; i < 200; i++){
        if (!(hda_rd32(HDA_GCTL) & 1u)) break;
        hda_delay();
    }
    hda_wr32(HDA_GCTL, 1);                       // set CRST: bring it out of reset
    hda_delay();
    int up = 0;
    for (int i = 0; i < 200; i++){
        if (hda_rd32(HDA_GCTL) & 1u){ up = 1; break; }
        hda_delay();
    }
    if (!up){
        hda_log("[HDA] controller refused to come out of reset\n");
        return 0;
    }

    // ---- 4) version + capabilities -----------------------------------
    uint16_t gcap  = hda_rd16(HDA_GCAP);
    uint8_t  vmaj  = hda_rd8(HDA_VMAJ);
    uint8_t  vmin  = hda_rd8(HDA_VMIN);
    // GCAP field layout (HDA spec 3.3.1) -- the first version read these from
    // the WRONG bit positions, which is why a valid 0x1044 capsule reported
    // "1 output stream" instead of the real 4:
    //   bits 15:12 = number of Serial Data Out (SDO) signals
    //   bits 11:8  = number of INPUT streams
    //   bits  7:4  = number of OUTPUT streams      <- what we actually need
    //   bits  3:0  = number of BIDIRECTIONAL streams
    // GCAP field layout -- cross-checked against Linux's hda_intel.c
    // (azx_get_caps: playback_streams = (gcap >> 12) & 0x0f,
    //                 capture_streams  = (gcap >>  8) & 0x0f),
    // which is the interpretation that actually matches every real controller.
    //
    // Measured: this VirtualBox controller reports GCAP = 0x4401, i.e. FOUR
    // playback streams and FOUR capture streams.  An earlier attempt to
    // "correct" this to (gcap >> 4) read 0 output streams and the driver gave
    // up on a perfectly good device -- the spec's bit-field table is easy to
    // misread, the Linux constants are not.
    int      nout  = (gcap >> 12) & 0xF;         // playback (output) streams
    int      nin   = (gcap >> 8)  & 0xF;         // capture (input) streams
    int      nsdo  = (gcap >> 4)  & 0xF;         // SDO line count (informational)
    int      nbidir= gcap & 0xF;                 // bidirectional streams (informational)
    hda_log("[HDA] v");
    hda_logdec(vmaj); hda_log("."); hda_logdec(vmin);
    hda_log("  out streams="); hda_logdec(nout);
    hda_log(" in="); hda_logdec(nin);
    hda_log(" sdo="); hda_logdec(nsdo);
    hda_log(" bidir="); hda_logdec(nbidir);
    hda_log("  GCAP="); hda_lognum((uint32_t)gcap, 4);
    hda_log("\n");
    if (nout <= 0){
        hda_log("[HDA] controller reports no output streams; no audio possible\n");
        return 0;
    }

    hda_log("[HDA] probe: reading STATESTS\n");
    // ---- 5) codec discovery ------------------------------------------
    // STATESTS has one bit per codec address (up to 15).
    //
    // POLL IT -- do not read it once.  The controller was just brought out of
    // reset two steps ago, and codec enumeration on a cold HDA link takes real
    // time (the spec's own reset sequence allows tens of milliseconds before
    // STATESTS is valid).  Reading it a single time immediately after CRST
    // returned 0 every run, which the driver then reported as
    // "no codec responded" -- indistinguishable from "there is no codec".
    uint16_t statests = 0;
    for (int attempt = 0; attempt < 200; attempt++){
        statests = hda_rd16(HDA_STATESTS);
        if (statests) break;
        hda_delay();
    }
    hda_log("[HDA] STATESTS=");
    hda_lognum((uint32_t)statests, 4);
    hda_log("\n");
    if (statests){
        for (int a = 0; a < 15; a++) if (statests & (1u << a)){ hda_codec = a; break; }
        hda_wr16(HDA_STATESTS, statests);      // ack by writing 1s back
    }
    if (hda_codec < 0){
        hda_log("[HDA] no codec on the link (STATESTS stayed 0 after polling)\n");
        return 0;
    }
    hda_log("[HDA] codec at address ");
    hda_logdec(hda_codec);
    hda_log("\n");

    // ---- 6) bring up CORB/RIRB ---------------------------------------
    // The rings must be 128-byte aligned and physically contiguous, and the
    // PCM ring is 64 KiB, so this is ~66 KiB of buffer.
    //
    // ALLOCATED, NOT STATIC.  A first version declared these as static arrays
    // and the LINKER stopped the build:
    //   "kernel64 .bss overruns HEAP_START (see kernel64.cpp:2381)"
    // .bss has to end below HEAP_START (0x500000) because the heap and the PMM
    // bitmap live right after it, and 66 KiB of extra .bss pushed it over.
    // pmm_alloc_page returns identity-mapped pages (VA == PA in long mode),
    // which is exactly what a DMA ring needs anyway, and it costs no .bss.
    hda_corb = alloc_pages(1);                   // 4 KiB: CORB (2 entries used)
    hda_rirb = alloc_pages(1);                   // 4 KiB: RIRB (2 entries used)
    hda_bdl  = alloc_pages(1);                   // 4 KiB: buffer descriptor list
    hda_buf  = alloc_pages(16);                  // 64 KiB: PCM ring
    if (!hda_corb || !hda_rirb || !hda_bdl || !hda_buf){
        hda_log("[HDA] out of physical memory for the DMA rings\n");
        return 0;
    }
    // CORBSIZE/RIRBSIZE: bits[1:0] select the ring size --
    //   0b00 = 2 entries, 0b01 = 256, 0b10 = 256, 0b11 = 256 (spec 3.3.18).
    // Only 2 entries is guaranteed, so ask for that and READ IT BACK: writing
    // a size the controller does not support silently keeps the old value, and
    // a mismatch between the programmed size and the "entries" count used by
    // hda_verb wraps the ring pointer to the wrong slot.
    hda_wr8(HDA_CORBSIZE, 0);
    hda_wr8(HDA_RIRBSIZE, 0);
    uint8_t corb_sz = (uint8_t)(hda_rd8(HDA_CORBSIZE) & 0x3);
    uint8_t rirb_sz = (uint8_t)(hda_rd8(HDA_RIRBSIZE) & 0x3);
    hda_log("[HDA] CORBSIZE=0x"); hda_lognum(corb_sz, 1);
    hda_log(" RIRBSIZE=0x");      hda_lognum(rirb_sz, 1);
    hda_log("\n");
    // 0b00 = 2 entries; the 256-entry encodings are the other three values.
    hda_corb_entries = (corb_sz == 0) ? 2 : 256;

    hda_wr32(HDA_CORBLBASE, hda_corb);
    hda_wr32(HDA_CORBUBASE, 0);
    // Reset the CORB read pointer: set the reset bit, then clear it.
    hda_wr16(HDA_CORBRP, 0x8000);
    hda_delay();
    // BOUNDED.  This was `while (CORBRP & 0x8000) hda_delay();` -- an
    // unbounded loop on a device register.  If the controller never clears that
    // bit, the kernel spins forever inside boot with no output, which is
    // indistinguishable from a crash from the outside.  Every wait in this
    // driver is now bounded.
    for (int i = 0; i < 200; i++){
        if (!(hda_rd16(HDA_CORBRP) & 0x8000)) break;
        hda_delay();
    }
    hda_wr16(HDA_CORBRP, 0);
    hda_wr16(HDA_CORBWP, 0);

    hda_wr32(HDA_RIRBLBASE, hda_rirb);
    hda_wr32(HDA_RIRBUBASE, 0);
    hda_wr16(HDA_RIRBWP, 0x8000);             // reset write pointer
    hda_wr16(HDA_RINTCNT, 1);
    hda_wr8(HDA_RIRBCTL, 0x02);               // enable response interrupt gen

    hda_wr8(HDA_CORBCTL, 0x02);               // CORBRUN
    hda_wr8(HDA_RIRBCTL, 0x02);
    hda_delay();

    // ---- 7) verify the verb path works -------------------------------
    uint32_t vendor = hda_verb(verb((uint8_t)hda_codec, 0, VERB_GET_PARAMETER, 0x00));
    hda_log("[HDA] codec vendor id=");
    hda_lognum((uint32_t)vendor, 8);
    hda_log("\n");
    if (vendor == 0xFFFFFFFFu){
        hda_log("[HDA] no reply to GET_PARAMETER: verb transport failed\n");
        return 0;
    }

    // ---- 8) walk the widget tree -------------------------------------
    uint32_t root_caps = hda_verb(verb((uint8_t)hda_codec, 0, VERB_GET_PARAMETER, 0x04));
    int first = (int)(root_caps & 0xFF);
    int count = (int)((root_caps >> 16) & 0xFF);
    hda_log("[HDA] widgets first=");
    hda_logdec((int)((uint32_t)first));
    hda_log(" count=");
    hda_logdec((int)((uint32_t)count));
    hda_log("\n");

    for (int i = 0; i < count && i < 64; i++){
        int nid = first + i;
        uint32_t caps = hda_verb(verb((uint8_t)hda_codec, (uint8_t)nid, VERB_GET_PARAMETER, 0x09));
        int type = (int)WIDGET_TYPE(caps);
        if (type == WTYPE_AUDIO_OUT && hda_dac < 0) hda_dac = nid;
        if (type == WTYPE_PIN && hda_pin < 0){
            // Only accept an OUTPUT pin: config default's bit 4 (device)
            // distinguishes jack vs fixed function; direction is port
            // connectivity bit 5.  Simpler and safer: take the first pin whose
            // connectivity says "output".
            uint32_t cfg = hda_verb(verb((uint8_t)hda_codec, (uint8_t)nid, VERB_GET_CONFIG_DEFAULT, 0));
            int connectivity = (int)((cfg >> 4) & 0xF);
            if (connectivity == 0x0 || connectivity == 0x1){ // jack / no physical
                hda_pin = nid;
            }
        }
        hda_log("[HDA]   node ");
        hda_logdec((int)((uint32_t)nid));
        hda_log(": ");
        hda_log(widget_type_name(type));
        hda_log("\n");
    }

    if (hda_dac < 0){
        hda_log("[HDA] no output converter found; cannot play PCM\n");
        return 0;
    }
    hda_log("[HDA] output converter node=");
    hda_logdec((int)((uint32_t)hda_dac));
    hda_log(" pin=");
    hda_logdec((int)((uint32_t)(hda_pin < 0 ? 0 : hda_pin)));
    hda_log("\n");

    // ---- 9) unmute the converter and open the pin --------------------
    // SET_AMP_GAIN_MUTE payload: bit7 = output amp, bit6..4 = index 0,
    // bit3..0 = mute(1)/unmute(0) and gain 0x0 (max).  0x00 means "unmute, +0dB".
    hda_verb(verb((uint8_t)hda_codec, (uint8_t)hda_dac, VERB_SET_AMP_MUTE, 0x00));
    if (hda_pin >= 0){
        // Pin widget control: bit6 = output enable (0x40).
        hda_verb(verb((uint8_t)hda_codec, (uint8_t)hda_pin, VERB_SET_PIN_WIDGET_CTL, 0x40));
        // Unmute the pin's amp too.
        hda_verb(verb((uint8_t)hda_codec, (uint8_t)hda_pin, VERB_SET_AMP_MUTE, 0x00));
    }

    // ---- 10) claim a stream descriptor and set its format -------------
    int sd = -1;
    for (int i = 0; i < 4; i++){                 // HDA exposes up to 4 SDs
        uint32_t sdctl = *(volatile uint32_t*)((uint64_t)hda_mmio_base + HDA_SD_BASE + i*HDA_SD_STRIDE + SD_CTL);
        if (!(sdctl & SDCTL_RUN)){ sd = i; break; }
    }
    if (sd < 0){ hda_log("[HDA] every stream descriptor is busy\n"); return 0; }
    hda_sd = sd;
    uint32_t sdb = hda_mmio_base + HDA_SD_BASE + sd*HDA_SD_STRIDE;

    // Reset the stream descriptor: set SRST, clear it, wait.  Bounded like
    // every other wait here (see the note on the controller reset above).
    *(volatile uint8_t*)((uint64_t)sdb + SD_CTL) = SDCTL_RESET;
    hda_delay();
    for (int i = 0; i < 200; i++){
        if (!(*(volatile uint8_t*)((uint64_t)sdb + SD_CTL) & SDCTL_RESET)) break;
        hda_delay();
    }
    // 44.1 kHz / 16-bit / 2 channels.  Encoding (spec 3.7.2 / 4.3.1.1):
    //   bits 15:14 = 0            PCM (not float)
    //   bits 13:11 = 0            base rate 44.1 kHz
    //   bits 10:8  = 0            rate multiplier 1x
    //   bits  7:4  = 1            (bits per sample) - 1  -> 16-bit
    //   bits  3:0  = 1            (channels) - 1         -> 2 channels
    uint16_t fmt = (0u << 14) | (0u << 11) | (0u << 8) | (1u << 4) | (1u << 0);
    *(volatile uint16_t*)((uint64_t)sdb + SD_FMT) = fmt;

    // Tell the CONVERTER the same format.
    //
    // SET_CONVERTER_FORMAT carries a FULL 16-BIT payload, but hda_verb() only
    // has an 8-bit payload field, so it cannot express this verb.  The
    // previous code passed 0x20 through hda_verb(), which sent a truncated
    // value the codec ignored -- one more reason the output stayed silent.
    // Issue it directly with the complete word.
    hda_verb_raw_real((uint8_t)hda_codec, (uint8_t)hda_dac, VERB_SET_CONV_FMT, fmt);

    // Bind the converter to stream tag 0, channel 0.  WITHOUT THIS the
    // converter never receives the samples: the stream descriptor runs, the
    // BDL is valid, the DMA plays -- and nothing reaches the pin, because the
    // codec does not know which stream to listen to.  Entirely missing in the
    // first version of this driver.
    hda_verb(verb((uint8_t)hda_codec, (uint8_t)hda_dac, VERB_SET_CONV_STREAM, 0x00));

    // Stream tag 0, channel 0, no stripe.  SD_CTL bits 23:20 carry the tag,
    // and it MUST agree with the value just given to the converter.
    *(volatile uint8_t*)((uint64_t)sdb + SD_STS) = 0xFF;   // clear latched status
    hda_wr32((uint32_t)(HDA_SD_BASE + hda_sd*HDA_SD_STRIDE + SD_CTL),
             SDCTL_TAG(0) | SDCTL_STRIPE(0));
    hda_delay();

    // One BDL entry covering the whole PCM ring.
    //
    // BDL entry layout (spec 3.6.3), 16 bytes, written here as four 32-bit
    // words because that is how the structure is laid out:
    //   word0 = buffer address, low 32 bits
    //   word1 = buffer address, high 32 bits
    //   word2 = bits 31:16 = entry length in bytes, bit 0 = interrupt-on-complete
    //   word3 = reserved
    //
    // The first version scattered the length across a whole word and used two
    // entries of 32 KiB each, which described buffers the device could not
    // walk: SD_LVI then said "2 entries valid" while only one was coherent.
    // One contiguous 64 KiB descriptor is valid and simplest.
    hda_buf_len = 65536;
    *(volatile uint32_t*)((uint64_t)sdb + SD_CBL) = (uint32_t)hda_buf_len;
    *(volatile uint16_t*)((uint64_t)sdb + SD_LVI) = 0;    // ONE valid entry (index 0)
    *(volatile uint32_t*)((uint64_t)sdb + SD_BDPL) = hda_bdl;
    *(volatile uint32_t*)((uint64_t)sdb + SD_BDPU) = 0;
    ((volatile uint32_t*)(uint64_t)hda_bdl)[0] = hda_buf;                  // addr lo
    ((volatile uint32_t*)(uint64_t)hda_bdl)[1] = 0;                         // addr hi
    ((volatile uint32_t*)(uint64_t)hda_bdl)[2] =                            // len|IOC
        (uint32_t)(((uint32_t)(hda_buf_len - 1) & 0xFFFFu) << 16);          // IOC = 0
    ((volatile uint32_t*)(uint64_t)hda_bdl)[3] = 0;                         // reserved

    hda_ok = 1;
    hda_log("[HDA] ready: 44.1kHz 16-bit stereo, stream descriptor ");
    hda_logdec((int)((uint32_t)sd));
    hda_log("\n");
    return 1;
}

extern "C" int hda_available(void){ return hda_ok; }

// Record the outcome of the boot-time playback self-test in the diagnostic
// buffer, so hda_trace.txt carries the RESULT and not just the setup.
extern "C" void hda_selftest_result(int played, int frames){
    hda_log("[HDA] selftest: requested ");
    hda_logdec(frames);
    hda_log(" frames, consumed ");
    hda_logdec(played);
    hda_log(played > 0 ? " bytes -- STREAM RAN\n" : " bytes -- STREAM DID NOT RUN\n");
}

// ---------------------------------------------------------------------
//  Playback
// ---------------------------------------------------------------------
// Feed PCM into the ring and run the stream.  The caller supplies 16-bit
// stereo frames at 44.1 kHz.  Returns the number of bytes consumed.
//
// Because there is no interrupt path, this poll-waits on LPIB: it writes the
// next chunk only once the stream has advanced past the previous one, which
// keeps the device fed without overrunning the 64 KiB ring.
extern "C" int hda_play_pcm(const int16_t* frames, int nframes){
    if (!hda_ok || hda_sd < 0 || !frames || nframes <= 0) return -1;
    uint32_t sdb = hda_mmio_base + HDA_SD_BASE + (uint32_t)hda_sd*HDA_SD_STRIDE;

    int bytes = nframes * 4;                       // 2 channels * 2 bytes
    if (bytes > hda_buf_len) bytes = hda_buf_len;
    if (bytes <= 0) return -1;
    uint8_t* dst = (uint8_t*)(uint64_t)hda_buf;
    const uint8_t* src = (const uint8_t*)frames;
    for (int i = 0; i < bytes; i++) dst[i] = src[i];

    // Re-point the single descriptor at this chunk and republish its length.
    // Both must be updated together: CBL and the BDL entry describe the same
    // buffer, and the device walks them as one.
    *(volatile uint32_t*)((uint64_t)sdb + SD_CBL) = (uint32_t)bytes;
    *(volatile uint16_t*)((uint64_t)sdb + SD_LVI) = 0;   // one valid entry
    ((volatile uint32_t*)(uint64_t)hda_bdl)[0] = hda_buf;
    ((volatile uint32_t*)(uint64_t)hda_bdl)[1] = 0;
    ((volatile uint32_t*)(uint64_t)hda_bdl)[2] =
        (uint32_t)(((uint32_t)(bytes - 1) & 0xFFFFu) << 16);
    ((volatile uint32_t*)(uint64_t)hda_bdl)[3] = 0;

    // Reset the descriptor's stream position, then run it.  Without the reset
    // LPIB keeps the previous run's offset and the drain test below exits
    // immediately (or never), depending on the leftovers.
    hda_wr8((uint32_t)(HDA_SD_BASE + hda_sd*HDA_SD_STRIDE + SD_CTL), SDCTL_RESET);
    hda_delay();
    for (int i = 0; i < 200; i++){
        if (!(hda_rd8((uint32_t)(HDA_SD_BASE + hda_sd*HDA_SD_STRIDE + SD_CTL)) & SDCTL_RESET)) break;
        hda_delay();
    }

    // Start the stream with the SAME tag the converter was bound to.
    uint32_t ctl = SDCTL_TAG(0) | SDCTL_STRIPE(0);
    *(volatile uint32_t*)((uint64_t)sdb + SD_CTL) = ctl | SDCTL_RUN;

    // Wait for the device to consume it (poll LPIB).  A bounded spin: even at
    // 44.1 kHz a 64 KiB buffer drains in ~370 ms, and 200000 iterations of a
    // register read is comfortably longer than that at any clock we run on.
    for (int spin = 0; spin < 200000; spin++){
        uint32_t lpib = *(volatile uint32_t*)((uint64_t)sdb + SD_LPIB);
        if (lpib >= (uint32_t)bytes) break;
    }

    // Stop the stream so the next call starts from a clean position.
    ctl = *(volatile uint32_t*)((uint64_t)sdb + SD_CTL);
    *(volatile uint32_t*)((uint64_t)sdb + SD_CTL) = ctl & ~SDCTL_RUN;
    return bytes;
}

// Report what the driver found, for the Audio Player / Control Panel to show.
// Hand the AUTHORITATIVE diagnostic record to the managed side.
//
// The serial port is shared and unlocked, so the probe's own log is
// interleaved garbage on the wire (see the note on hda_log).  This buffer was
// filled by the driver alone, so whatever it says really happened, and the
// Control Panel / Audio Player can show it without a serial capture.
extern "C" void hda_report(char* buf, int n){
    if (!buf || n <= 0) return;
    int p = 0;
    // Line 1: the verdict, so a caller that only wants status can read this.
    if (hda_ok){
        const char* a = "HD Audio READY (44.1kHz/16-bit/stereo) codec=";
        for (int i = 0; a[i] && p < n-1; i++) buf[p++] = a[i];
        buf[p++] = (char)('0' + (hda_codec < 0 ? 0 : hda_codec % 10));
    } else {
        const char* m = "HD Audio UNAVAILABLE: ";
        for (int i = 0; m[i] && p < n-1; i++) buf[p++] = m[i];
    }
    // Then the probe trace, which is what actually explains a failure.
    if (hda_loglen > 0){
        if (p < n-1) buf[p++] = '|';
        for (int i = 0; i < hda_loglen && p < n-1; i++){
            char c = hda_logbuf[i];
            buf[p++] = (c == '\n') ? ' ' : c;     // one line on the managed side
        }
    }
    buf[p] = 0;
}
