#!/usr/bin/env python3
"""One-shot patcher: safe secondary-channel model read + crc32 integrity."""
import io, sys, os

PATH = os.path.join(os.path.dirname(__file__), '..', '.attic64', 'kernel64.cpp')
src = io.open(PATH, 'r', encoding='utf-8', newline='').read()

def sub(old, new, tag):
    global src
    o = old.replace('\n', '\r\n')
    n = new.replace('\n', '\r\n')
    if o not in src:
        print('MISS  %s' % tag); sys.exit(2)
    if src.count(o) != 1:
        print('DUP(%d) %s' % (src.count(o), tag)); sys.exit(3)
    src = src.replace(o, n)
    print('OK    %s' % tag)

# ------------------------------------------------ channel-aware soft reset --
sub(
"""static void ata_soft_reset(){""",
"""// Soft-reset an arbitrary ATA channel.  The device-control register sits at
// base + ATA_CTL (0x376 for the secondary channel, 0x3F6 for the primary).
static void ata_soft_reset_base(uint16_t base){
    uint16_t ctl = (uint16_t)(base + 0x06 + 0x20);   // 0x3F6 / 0x376
    outb(ctl, 0x04);                 // SRST assert
    for (volatile int i = 0; i < 200000; i++) {}
    outb(ctl, 0x00);                 // SRST release
    for (volatile int i = 0; i < 200000; i++) {}
    ata_nobsy_base(base, 2000000);
}

static void ata_soft_reset(){""",
'ata_soft_reset_base')

# --------------------------------------------- replace bulk PIO with safe --
sub(
"""// Bulk PIO over an arbitrary channel/drive.  The secondary channel (0x170)
// has no usable BM-DMA or virtio fast path, so pulling the weights off the
// data disk has to go through here: one READ SECTORS command per 128
// sectors instead of one per sector.
static bool model_read_bulk_pio(uint8_t* dst, uint16_t io_base, uint8_t drive,
                                uint64_t lba, uint64_t size){
    uint64_t sectors = (size + 511) / 512;
    uint64_t i = 0;
    while (i < sectors){
        uint32_t n = (uint32_t)(sectors - i);
        if (n > 128) n = 128;
        if (!ata_read_multi_drive(io_base, drive, (uint32_t)(lba + i),
                                  (uint8_t)n, (uint16_t*)(dst + (i << 9)))){
            serial_puts("[MDL] PIO error at sector ");
            serial_dec((uint32_t)i);
            serial_puts(" (stage ");
            serial_dec(ata_last_err);
            serial_puts(")\\n");
            return false;
        }
        i += n;
        if ((i & 0x3FFFu) == 0u){
            serial_puts("[MDL] ");
            serial_dec((uint32_t)(i >> 11));
            serial_puts(" MB\\n");
        }
    }
    return true;
}""",
"""// Standard CRC-32 (reflected, poly 0xEDB88320) -- the same function the host
// tool uses, so the descriptor checksum can be verified in-kernel.
static uint32_t crc32_bytes(const uint8_t* p, uint32_t n){
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++){
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

// Read the payload off the DATA disk.
//
// The secondary channel has no usable BM-DMA or virtio fast path (both are
// hard-bound to the primary device), and VirtualBox's PIIX3 has been observed
// to wedge on a count>1 READ SECTORS command -- BSY/DRQ never clear.  So the
// load starts with ONE small multi-sector PROBE: if it completes and the data
// carries the GGUF magic, bulk mode is used for the rest; otherwise the
// channel is soft-reset and the load falls back to the single-sector PIO path
// that SFS uses (proven on QEMU / VirtualBox / VMware).
static bool model_read_data_disk(uint8_t* dst, const struct ModelDesc* d){
    uint64_t sectors = (d->size + 511) / 512;
    uint64_t i = 0;
    bool bulk = false;

    if (sectors >= 2){
        uint8_t probe[8 * 512];
        uint32_t want = (uint32_t)(sectors < 8 ? sectors : 8);
        if (ata_read_multi_drive(d->io_base, d->drive, (uint32_t)d->start_lba,
                                 (uint8_t)want, (uint16_t*)probe)
            && probe[0]=='G' && probe[1]=='G' && probe[2]=='U' && probe[3]=='F'){
            bulk = true;
            memcpy_(dst, probe, (int)(want * 512));
            i = want;
            serial_puts("[MDL] multi-sector PIO works -> bulk mode\\n");
        } else {
            serial_puts("[MDL] multi-sector probe failed -> single-sector PIO\\n");
            ata_soft_reset_base(d->io_base);
        }
    }

    while (i < sectors){
        if (bulk){
            uint32_t n = (uint32_t)(sectors - i);
            if (n > 128) n = 128;
            if (!ata_read_multi_drive(d->io_base, d->drive, (uint32_t)(d->start_lba + i),
                                      (uint8_t)n, (uint16_t*)(dst + (i << 9)))){
                serial_puts("[MDL] bulk PIO failed at sector ");
                serial_dec((uint32_t)i);
                serial_puts(" -> single-sector PIO\\n");
                ata_soft_reset_base(d->io_base);
                bulk = false;
                continue;                       // retry this sector, slowly
            }
            i += n;
        } else {
            ata_read_sector_drive(d->io_base, d->drive, (uint32_t)(d->start_lba + i),
                                  (uint16_t*)(dst + (i << 9)));
            if (ata_last_err != 0){
                serial_puts("[MDL] PIO err lba=");
                serial_dec((uint32_t)(d->start_lba + i));
                serial_puts(" err=");
                serial_dec(ata_last_err);
                serial_puts(" (reset+retry)\\n");
                ata_soft_reset_base(d->io_base);
                ata_read_sector_drive(d->io_base, d->drive, (uint32_t)(d->start_lba + i),
                                      (uint16_t*)(dst + (i << 9)));
                if (ata_last_err != 0){
                    serial_puts("[MDL] FATAL: sector unreadable\\n");
                    return false;
                }
            }
            i++;
        }
        if ((i & 0x3FFFu) == 0u){
            serial_puts("[MDL] ");
            serial_dec((uint32_t)(i >> 11));
            serial_puts(" MB\\n");
        }
    }
    return true;
}""",
'model_read_data_disk (probe + fallback)')

# ------------------------------------------------- use it + integrity gate --
sub(
"""    serial_puts(" MB via bulk PIO\\n");
    if (!model_read_bulk_pio(dst, d->io_base, d->drive, d->start_lba, d->size)){
        big_free(dst, (uint32_t)alloc);
        return 0;
    }
    serial_puts("[MDL] read OK, head: ");
    for (int b = 0; b < 8; b++){ serial_puthex(dst[b]); serial_putc(' '); }
    serial_puts("\\n");
    return dst;
}""",
"""    serial_puts(" MB\\n");
    if (!model_read_data_disk(dst, d)){
        big_free(dst, (uint32_t)alloc);
        return 0;
    }
    // Integrity gate: a read that silently returned the wrong sector would
    // feed garbage into the GGUF parser, so the payload must start with the
    // GGUF magic and (when the descriptor carries one) the first 4 KiB must
    // match the recorded CRC-32.
    if (dst[0]!='G' || dst[1]!='G' || dst[2]!='U' || dst[3]!='F'){
        serial_puts("[MDL] FATAL: payload head is not GGUF -- wrong LBA?\\n");
        big_free(dst, (uint32_t)alloc);
        return 0;
    }
    if (d->crc32 != 0){
        uint32_t n = (d->size < 4096) ? (uint32_t)d->size : 4096u;
        uint32_t c = crc32_bytes(dst, n);
        if (c != d->crc32){
            serial_puts("[MDL] WARNING: head CRC mismatch (want ");
            serial_puthex((uint8_t)(d->crc32 >> 24)); serial_puthex((uint8_t)(d->crc32 >> 16));
            serial_puthex((uint8_t)(d->crc32 >> 8));  serial_puthex((uint8_t)(d->crc32));
            serial_puts(", got ");
            serial_puthex((uint8_t)(c >> 24)); serial_puthex((uint8_t)(c >> 16));
            serial_puthex((uint8_t)(c >> 8));  serial_puthex((uint8_t)(c));
            serial_puts(")\\n");
        }
    }
    serial_puts("[MDL] read OK, head: ");
    for (int b = 0; b < 8; b++){ serial_puthex(dst[b]); serial_putc(' '); }
    serial_puts("\\n");
    return dst;
}""",
'model_blob_read_desc integrity gate')

# ------------------------------------- carry the crc through ModelDesc -----
sub(
"""struct ModelDesc {
    uint16_t io_base;
    uint8_t  drive;
    uint64_t hdr_lba;
    uint64_t start_lba;
    uint64_t size;
    uint32_t version;
    char     name[48];
};""",
"""struct ModelDesc {
    uint16_t io_base;
    uint8_t  drive;
    uint64_t hdr_lba;
    uint64_t start_lba;
    uint64_t size;
    uint32_t version;
    uint32_t crc32;         // CRC-32 of the payload's first 4 KiB (0 = none)
    char     name[48];
};""",
'ModelDesc gains crc32')

sub(
"""            d.version = 2;
            memcpy_(d.name, v2.name, 47);""",
"""            d.version = 2;
            d.crc32   = v2.crc32;
            memcpy_(d.name, v2.name, 47);""",
'ModelDesc crc32 filled from v2')

io.open(PATH, 'w', encoding='utf-8', newline='').write(src)
print('written: %s (%d bytes)' % (PATH, len(src)))
