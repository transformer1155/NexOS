#!/usr/bin/env python3
"""One-shot patcher for the L4/L12 follow-up work: model externalisation.

The kernel source is CRLF, which the string-matching editor cannot match
reliably, so every edit below is applied as an exact byte-level replacement
with an assertion.  Run once, then delete.
"""
import io, sys, os

PATH = os.path.join(os.path.dirname(__file__), '..', '.attic64', 'kernel64.cpp')

src = io.open(PATH, 'r', encoding='utf-8', newline='').read()

def sub(old, new, tag):
    global src
    o = old.replace('\n', '\r\n')
    n = new.replace('\n', '\r\n')
    if o not in src:
        print('MISS  %s' % tag)
        sys.exit(2)
    if src.count(o) != 1:
        print('DUP(%d) %s' % (src.count(o), tag))
        sys.exit(3)
    src = src.replace(o, n)
    print('OK    %s' % tag)


# ---------------------------------------------------------------- ATA drive --
sub(
"""// Read one sector from the given channel, master device (drv = 0xA0).
static void ata_read_sector_base(uint16_t base, uint32_t lba, uint16_t* buf){""",
"""// Read one sector from the given channel with an explicit drive-select byte.
// The model lives on the SECONDARY channel's master (0xE0) and the boot disk
// on the PRIMARY one, so a single hard-coded 0xE0 would silently read the
// wrong volume -- hence the drive byte is now a parameter.
static void ata_read_sector_drive(uint16_t base, uint8_t drv, uint32_t lba, uint16_t* buf){""",
'ata_read_sector_drive decl')

sub(
"""    //    (the LBA registers never latched and the drive used its default CHS).
    outb(base+6, 0xE0 | ((lba>>24)&0x0F));""",
"""    //    (the LBA registers never latched and the drive used its default CHS).
    outb(base+6, (uint8_t)(drv | ((lba>>24)&0x0F)));""",
'ata drive-select byte')

sub(
"""    ata_last_status = inb(base+ATA_CMD);   // final status / clear IRQ
}

// Boot-disk read: unchanged signature and unchanged channel (PRIMARY), because""",
"""    ata_last_status = inb(base+ATA_CMD);   // final status / clear IRQ
}

// Same as above with the default master drive select (0xE0).  Every
// pre-existing caller keeps its exact behaviour.
static void ata_read_sector_base(uint16_t base, uint32_t lba, uint16_t* buf){
    ata_read_sector_drive(base, 0xE0, lba, buf);
}

// Boot-disk read: unchanged signature and unchanged channel (PRIMARY), because""",
'ata_read_sector_base wrapper')

# ------------------------------------------------------- bulk PIO for 0x170 --
sub(
"""static bool ata_read_multi(uint32_t lba, uint8_t count, uint16_t* buf){
    ata_last_err = 0;
    if(!ata_nobsy(50000)) { ata_last_status = inb(0x1F7); ata_last_err = 1; return false; }
    outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
    if(!ata_nobsy(50000)) { ata_last_status = inb(0x1F7); ata_last_err = 2; return false; }
    outb(0x1F1, 0x00);
    outb(0x1F2, count);
    outb(0x1F3, (uint8_t)(lba & 0xFF));
    outb(0x1F4, (uint8_t)((lba>>8) & 0xFF));
    outb(0x1F5, (uint8_t)((lba>>16) & 0xFF));
    outb(0x1F7, 0x20);                 // READ SECTORS""",
"""// Multi-sector PIO read on an ARBITRARY channel / drive.  The secondary
// channel (0x170) has no usable BM-DMA or virtio fast path, so pulling the
// 379 MiB model off the data disk has to go through this path: one READ
// SECTORS command per 128 sectors instead of one per sector.
static bool ata_read_multi_drive(uint16_t base, uint8_t drv, uint32_t lba, uint8_t count, uint16_t* buf){
    ata_last_err = 0;
    if(!ata_nobsy_base(base, 50000)) { ata_last_status = inb(base+ATA_CMD); ata_last_err = 1; return false; }
    outb(base+6, (uint8_t)(drv | ((lba>>24)&0x0F)));
    if(!ata_nobsy_base(base, 50000)) { ata_last_status = inb(base+ATA_CMD); ata_last_err = 2; return false; }
    outb(base+1, 0x00);
    outb(base+2, count);
    outb(base+3, (uint8_t)(lba & 0xFF));
    outb(base+4, (uint8_t)((lba>>8) & 0xFF));
    outb(base+5, (uint8_t)((lba>>16) & 0xFF));
    outb(base+ATA_CMD, 0x20);         // READ SECTORS""",
'ata_read_multi_drive head')

sub(
"""    for (uint8_t s = 0; s < count; s++){
        if(!ata_drq(2000000)) { ata_last_status = inb(0x1F7); ata_last_err = 3; return false; }
        __asm__ __volatile__("cld; rep insw" :: "d"((uint16_t)0x1F0), "D"(buf), "c"((uint32_t)256) : "memory");
        buf += 256;
    }
    // Command complete: BSY clears (no more DRQ), then read status to clear IRQ.
    if(!ata_nobsy(2000000)) { ata_last_status = inb(0x1F7); ata_last_err = 5; return false; }
    ata_last_status = inb(0x1F7);
    if (ata_last_status & 0x01) { ata_last_err = 4; return false; }   // ERR latched
    return true;
}""",
"""    for (uint8_t s = 0; s < count; s++){
        if(!ata_drq_base(base, 2000000)) { ata_last_status = inb(base+ATA_CMD); ata_last_err = 3; return false; }
        __asm__ __volatile__("cld; rep insw" :: "d"(base), "D"(buf), "c"((uint32_t)256) : "memory");
        buf += 256;
    }
    // Command complete: BSY clears (no more DRQ), then read status to clear IRQ.
    if(!ata_nobsy_base(base, 2000000)) { ata_last_status = inb(base+ATA_CMD); ata_last_err = 5; return false; }
    ata_last_status = inb(base+ATA_CMD);
    if (ata_last_status & 0x01) { ata_last_err = 4; return false; }   // ERR latched
    return true;
}

// PRIMARY-channel multi-sector read (the original behaviour, unchanged).
static bool ata_read_multi(uint32_t lba, uint8_t count, uint16_t* buf){
    return ata_read_multi_drive(ATA_PRIMARY, 0xE0, lba, count, buf);
}""",
'ata_read_multi_drive body + wrapper')

io.open(PATH, 'w', encoding='utf-8', newline='').write(src)
print('written: %s (%d bytes)' % (PATH, len(src)))
