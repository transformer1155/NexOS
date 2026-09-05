/* =====================================================================
 *  bootuefi.c  -  UEFI bootloader (x86_64, gnu-efi)
 * ---------------------------------------------------------------------
 *  UEFI equivalent of boot.asm + stage2.asm.  Loads kernel.bin from
 *  the ESP, copies it to 0x10000, exits boot services, then calls
 *  enter_kernel() which drops to 32-bit protected mode and jumps to
 *  the C++ kernel at 0x10000.
 *
 *  NOTE: kernel.bin is embedded via linker --format binary to work
 *  around a bug in the OVMF FAT12 driver that causes data corruption
 *  for files larger than ~124 KiB.  The embedded data is accessed via
 *  RIP-relative LEA (get_embedded.S) to avoid PE32+ relocation issues.
 *
 *  Real-hardware compatibility (v2):
 *  - Properly allocate memory at 0x10000 via AllocatePages
 *  - Fallback: allocate anywhere, copy to 0x10000 after ExitBootServices
 *  - Allocate VbeInfo at 0x5000 and stack at 0x90000
 *  - Visible on-screen debug messages with delays
 *  - Handle all GOP pixel formats and framebuffer >4GB
 * ===================================================================== */

#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

#define KERNEL_LOAD_ADDR 0x10000ULL
#define VBE_INFO_ADDR    0x5000ULL
/* Boot scratch region reserved by linker.ld's .lmboot block:
 *   LMBOOT_BASE + 0x00000 .. +0x10000   32-bit boot stack (grows down from top)
 *   LMBOOT_BASE + 0x10000 .. +0x18000   long-mode page tables (PML4/PDPT/...)
 *   LMBOOT_BASE + 0x18000 .. +0x28000   64-bit boot stack
 * STACK_ADDR is the 32-bit stack TOP that enter_kernel.S loads into ESP.
 * It was 0x90000, which now lands inside the kernel image -- see the
 * AllocatePages call below for the full story.
 * COUPLING: keep in sync with linker.ld and .attic64/kernel64.cpp. */
#define LMBOOT_BASE      0x1800000ULL
#define LMBOOT_SIZE      0x0030000ULL
#define STACK_ADDR       (LMBOOT_BASE + 0x10000ULL)
/* 64-bit kernel handoff:
 *   - Loaded to K64_LOAD_ADDR (20 MiB), ABOVE the 32-bit kmalloc heap
 *     (3-19 MiB) so the two never collide, and below the 32-MiB cap the
 *     32-bit switch path requires for the staging buffer.
 *   - A small struct at K64_HANDOFF_ADDR (0x7000, below the kernel at
 *     0x10000, in the free boot-params zone) tells the 32-bit kernel the
 *     64-bit image is preloaded so it must NOT use its (UEFI-broken) ATA
 *     read. */
#define K64_LOAD_ADDR    0x1400000ULL
#define K64_HANDOFF_ADDR 0x7000ULL
#define K64_HANDOFF_MAGIC 0x4B36344EULL  /* "K64N" */

struct __attribute__((packed)) k64_handoff {
    UINT32 magic;   /* K64_HANDOFF_MAGIC */
    UINT32 phys;    /* physical address of loaded 64-bit kernel */
    UINT32 size;    /* size in bytes */
};

/* ---- RAM-backed SFS handoff -------------------------------------------
 *  Under UEFI (q35/AHCI) the kernels' legacy IDE PIO ata_read_sector()
 *  reads all zeros, so Sfs::init() never finds the "SFS\0" superblock.
 *  Consequence: shell.mex is reported "file not found", the managed Win11
 *  shell draws nothing and the screen stays black.
 *
 *  Fix: embed the whole SFS image in this EFI binary, stage it into RAM
 *  before ExitBootServices, and publish {magic, base, size} at 0x0900 --
 *  exactly the handoff the 32-bit CD-boot path already defines, so BOTH
 *  the 32-bit and the 64-bit kernel pick it up with no protocol change.
 *
 *  Address choice 0x50000000 (1280 MiB): above the 32-bit PMM ceiling
 *  (256 MiB) and above the 64-bit PMM ceiling (~1 GiB), so neither
 *  kernel's page allocator can ever hand this region out.
 */
#define SFS_RAM_ADDR      0x50000000ULL
#define SFS_HANDOFF_ADDR  0x0900ULL
#define SFS_RAM_MAGIC     0xC0DE5A5FUL

struct __attribute__((packed)) sfs_handoff {
    UINT32 magic;   /* SFS_RAM_MAGIC */
    UINT32 base;    /* physical address of the SFS image in RAM */
    UINT32 size;    /* size in bytes */
};

/* Embedded 64-bit kernel blob (linked via tools/build_uefi_gnuefi_free.sh) */
extern const unsigned char _binary_build_kernel64_blob_start[];
extern const unsigned char _binary_build_kernel64_blob_end[];

/* Embedded SFS filesystem image blob (same build script, step [1c]) */
extern const unsigned char _binary_build_sfs_blob_start[];
extern const unsigned char _binary_build_sfs_blob_end[];

/* VBE info structure (at physical address 0x5000, set by stage2 in BIOS path)
 *
 * Extended for real-hardware UEFI support:
 *   0x500F: pixel_format (0=BGRX32, 1=RGBX32, 2=RGB24, 3=RGB565, 4=BltOnly)
 *   0x5010-0x5017: 64-bit framebuffer physical address (for >4GB framebuffers)
 *   0x5018-0x501B: shadow buffer physical address (if >4GB, mapped to low memory)
 */
struct __attribute__((packed)) VbeInfo {
    UINT32 framebuffer_phys;   /* 0x5000 - low 32 bits of framebuffer (or shadow buffer) */
    UINT16 width;              /* 0x5004 */
    UINT16 height;             /* 0x5006 */
    UINT8  bpp;                /* 0x5008 */
    UINT16 pitch;              /* 0x5009 */
    UINT16 mode_number;        /* 0x500B */
    UINT8  vbe_ok;             /* 0x500D */
    UINT8  vbe_mode_set;       /* 0x500E: 1=mode already set (BIOS INT 10h or UEFI GOP) */
    UINT8  pixel_format;       /* 0x500F: 0=BGRX32, 1=RGBX32, 2=RGB24, 3=RGB565, 4=BltOnly */
    UINT64 framebuffer_phys64; /* 0x5010: full 64-bit framebuffer address */
    UINT32 shadow_buffer;      /* 0x5018: shadow buffer addr if framebuffer > 4GB */
    UINT8  reserved[4];        /* 0x501C-0x501F: padding */
};

/* Pixel format constants */
#define PXF_BGRX32  0   /* Blue-Green-Red-Reserved, 8 bits each (most common UEFI) */
#define PXF_RGBX32  1   /* Red-Green-Blue-Reserved, 8 bits each */
#define PXF_RGB24   2   /* 24-bit packed RGB */
#define PXF_RGB565  3   /* 16-bit RGB565 */
#define PXF_BLT_ONLY 4  /* No linear framebuffer, only Blt() available (unsupported) */

/* RIP-relative functions to get embedded kernel address (see get_embedded.S).
 * These avoid the GOT and work without PE32+ base relocations. */
extern const unsigned char *get_embedded_kernel_start(void);
extern const unsigned char *get_embedded_kernel_end(void);
extern const unsigned char *get_embedded_kernel_size(void);

extern void enter_kernel(void);

/* Serial port I/O helpers */
#define SERIAL_PORT 0x3F8
#define SERIAL_PUTC(c) __asm__ __volatile__("outb %0, %1" : : "a"((UINT8)(c)), "Nd"(SERIAL_PORT))
#define SERIAL_PUTHEX(v) do { \
    const char *h__ = "0123456789ABCDEF"; \
    SERIAL_PUTC(h__[((v) >> 4) & 0xF]); \
    SERIAL_PUTC(h__[(v) & 0xF]); \
} while(0)
#define SERIAL_PUTHEX64(v) do { \
    UINT64 x__ = (UINT64)(v); \
    SERIAL_PUTHEX((x__ >> 56) & 0xFF); \
    SERIAL_PUTHEX((x__ >> 48) & 0xFF); \
    SERIAL_PUTHEX((x__ >> 40) & 0xFF); \
    SERIAL_PUTHEX((x__ >> 32) & 0xFF); \
    SERIAL_PUTHEX((x__ >> 24) & 0xFF); \
    SERIAL_PUTHEX((x__ >> 16) & 0xFF); \
    SERIAL_PUTHEX((x__ >> 8) & 0xFF); \
    SERIAL_PUTHEX(x__ & 0xFF); \
} while(0)
#define SERIAL_PUTHEX32(v) do { \
    const char *h__ = "0123456789ABCDEF"; \
    UINT32 x__ = (UINT32)(v); \
    SERIAL_PUTHEX((x__ >> 24) & 0xFF); \
    SERIAL_PUTHEX((x__ >> 16) & 0xFF); \
    SERIAL_PUTHEX((x__ >> 8) & 0xFF); \
    SERIAL_PUTHEX(x__ & 0xFF); \
} while(0)
#define SERIAL_PUTDEC(v) do { \
    UINT32 n__ = (UINT32)(v); \
    char b__[12]; int i__ = 0; \
    if (n__ == 0) { SERIAL_PUTC('0'); } \
    else { while (n__ > 0 && i__ < 11) { b__[i__++] = (char)('0' + (n__ % 10)); n__ /= 10; } \
           while (i__ > 0) SERIAL_PUTC(b__[--i__]); } \
} while(0)

/* Simple inline memcpy for use after ExitBootServices (no boot services available) */
static void inline_memcpy(void *dst, const void *src, UINTN n) {
    volatile UINT8 *d = (volatile UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    while (n--) *d++ = *s++;
}

/* File-scope VbeInfo backup - accessible after ExitBootServices */
static struct VbeInfo g_vbe_backup;
static int g_vbe_valid = 0;

/* File-scope 64-bit kernel handoff backup - accessible after ExitBootServices */
static struct k64_handoff g_k64;
static int g_k64_valid = 0;
static int g_k64_at_final = 0;
static EFI_PHYSICAL_ADDRESS g_k64_temp = 0;

/* File-scope RAM-SFS handoff backup - accessible after ExitBootServices */
static struct sfs_handoff g_sfs;
static int g_sfs_valid = 0;

EFI_STATUS efi_main(EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
    /* Early debug: print ST address before InitializeLib */
    SERIAL_PUTC('S');
    SERIAL_PUTC('T');
    SERIAL_PUTC('\n');

    InitializeLib(Image, SystemTable);

    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;

    const unsigned char *kern_start = get_embedded_kernel_start();
    const unsigned char *kern_end   = get_embedded_kernel_end();
    UINTN kern_size = (UINTN)(kern_end - kern_start);

    /* ===========================================================
     *  Step 1: Load kernel
     * =========================================================== */
    Print(L"\r\n");
    Print(L"==============================\r\n");
    Print(L" NexOS UEFI Bootloader v2.0\r\n");
    Print(L"==============================\r\n");
    Print(L"\r\n");
    Print(L"[1/5] Loading kernel: %d bytes\r\n", kern_size);

    /* ---- Allocate memory for kernel ----
     * CRITICAL: We must NOT write to 0x10000 without allocating it first.
     * On real UEFI hardware, 0x10000 may be in use by firmware.
     *
     * Strategy:
     *  1. Try AllocateAddress at 0x10000 (ideal - kernel is linked there)
     *  2. If that fails, allocate anywhere and copy to 0x10000 AFTER
     *     ExitBootServices (we own all memory then)
     */
    UINTN kern_pages = (kern_size + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS kern_target = KERNEL_LOAD_ADDR;
    EFI_STATUS kern_alloc_status;
    int kern_at_final = 0;  /* 1 = kernel already at 0x10000 */
    EFI_PHYSICAL_ADDRESS kern_temp = 0;  /* temp location if 0x10000 unavailable */

    /* Try to allocate at 0x10000 */
    kern_alloc_status = bs->AllocatePages(
        AllocateAddress, EfiLoaderData, kern_pages, &kern_target);

    if (!EFI_ERROR(kern_alloc_status)) {
        /* Got 0x10000 - copy kernel directly there */
        Print(L"      Allocated 0x10000 (%d pages)\r\n", kern_pages);
        bs->CopyMem((VOID *)KERNEL_LOAD_ADDR, (VOID *)kern_start, kern_size);
        kern_at_final = 1;
        Print(L"      Kernel copied to 0x10000\r\n");
    } else {
        /* Can't get 0x10000 - allocate anywhere */
        Print(L"      0x10000 unavailable (err=0x%lx), using temp buffer\r\n", kern_alloc_status);
        kern_temp = 0;
        EFI_STATUS alt_status = bs->AllocatePages(
            AllocateAnyPages, EfiLoaderData, kern_pages, &kern_temp);

        if (EFI_ERROR(alt_status)) {
            Print(L"\r\n*** FATAL: Cannot allocate kernel memory! ***\r\n");
            Print(L"*** Halting. ***\r\n");
            bs->Stall(5000000);  /* 5 seconds to read */
            return EFI_OUT_OF_RESOURCES;
        }

        Print(L"      Temp buffer at 0x%lx\r\n", kern_temp);
        bs->CopyMem((VOID *)kern_temp, (VOID *)kern_start, kern_size);
        Print(L"      Kernel copied to temp buffer\r\n");
        Print(L"      Will relocate to 0x10000 after ExitBootServices\r\n");
        kern_at_final = 0;
    }

    /* ---- Verify kernel data ---- */
    {
        volatile UINT8 *k = kern_at_final ?
            (volatile UINT8 *)KERNEL_LOAD_ADDR :
            (volatile UINT8 *)kern_temp;
        /* Check first few bytes match source */
        int match = 1;
        for (int i = 0; i < 8; i++) {
            if (k[i] != kern_start[i]) { match = 0; break; }
        }
        Print(L"      Verify: %s\r\n", match ? L"OK" : L"MISMATCH!");
    }

    /* ===========================================================
     *  Step 1b: Preload 64-bit kernel from embedded blob
     * ===========================================================
     *  The 32-bit kernel's own ATA driver reads garbage (all zeros)
     *  under UEFI, so we stage the 64-bit image here and hand its
     *  address off at 0x7000.  The 32-bit switch path then blits it
     *  from this known-good buffer instead of re-reading the disk.
     */
    {
        UINTN k64_size = (UINTN)(_binary_build_kernel64_blob_end -
                                 _binary_build_kernel64_blob_start);
        EFI_PHYSICAL_ADDRESS k64_target = K64_LOAD_ADDR;
        int k64_at_final = 0;
        EFI_PHYSICAL_ADDRESS k64_temp = 0;
        UINTN k64_pages = (k64_size + 4095) / 4096;

        EFI_STATUS k64st = bs->AllocatePages(
            AllocateAddress, EfiLoaderData, k64_pages, &k64_target);
        if (!EFI_ERROR(k64st)) {
            bs->CopyMem((VOID *)K64_LOAD_ADDR,
                        (VOID *)_binary_build_kernel64_blob_start, k64_size);
            k64_at_final = 1;
            Print(L"      K64 preloaded @0x1400000 (%d bytes)\r\n", k64_size);
        } else {
            k64_temp = 0;
            EFI_STATUS alt = bs->AllocatePages(
                AllocateAnyPages, EfiLoaderData, k64_pages, &k64_temp);
            if (!EFI_ERROR(alt)) {
                bs->CopyMem((VOID *)k64_temp,
                            (VOID *)_binary_build_kernel64_blob_start, k64_size);
                Print(L"      K64 temp @0x%lx, will relocate post-ExitBS\r\n", k64_temp);
            } else {
                Print(L"      *** K64 preload FAILED ***\r\n");
            }
        }

        /* Stash handoff info for post-ExitBootServices write to 0x7000 */
        g_k64.magic = (UINT32)K64_HANDOFF_MAGIC;
        g_k64.phys  = (UINT32)K64_LOAD_ADDR;
        g_k64.size  = (UINT32)k64_size;
        g_k64_valid = 1;
        g_k64_at_final = k64_at_final;
        g_k64_temp = k64_temp;
    }

    /* ===========================================================
     *  Step 1c: Preload the SFS filesystem image into RAM
     * ===========================================================
     *  Legacy IDE PIO (ata_read_sector) returns all zeros on the q35/AHCI
     *  machines UEFI runs on, so neither kernel can mount SFS from disk and
     *  shell.mex -- the managed Win11 shell -- is never found (black screen).
     *  Stage the embedded image in RAM here; the handoff at 0x0900 is written
     *  after ExitBootServices (Step 4).
     */
    {
        UINTN sfs_size = (UINTN)(_binary_build_sfs_blob_end -
                                 _binary_build_sfs_blob_start);
        UINTN sfs_pages = (sfs_size + 4095) / 4096;
        EFI_PHYSICAL_ADDRESS sfs_target = SFS_RAM_ADDR;

        EFI_STATUS sst = bs->AllocatePages(
            AllocateAddress, EfiLoaderData, sfs_pages, &sfs_target);

        if (!EFI_ERROR(sst)) {
            bs->CopyMem((VOID *)SFS_RAM_ADDR,
                        (VOID *)_binary_build_sfs_blob_start, sfs_size);
            g_sfs.base  = (UINT32)SFS_RAM_ADDR;
            g_sfs.size  = (UINT32)sfs_size;
            g_sfs.magic = (UINT32)SFS_RAM_MAGIC;
            g_sfs_valid = 1;
            Print(L"      SFS staged @0x50000000 (%d bytes)\r\n", sfs_size);
        } else {
            /* 0x50000000 unavailable (small-RAM box or firmware squatting).
             * Fall back to anywhere BELOW 4 GiB -- the 32-bit kernel reads the
             * image through a 32-bit pointer -- and use the buffer in place
             * instead of relocating, so we never write to RAM that may not
             * exist.  The 64-bit kernel reserves this exact range in its PMM
             * from the handoff, so the allocator cannot recycle it. */
            EFI_PHYSICAL_ADDRESS any = 0xFFFFF000ULL;
            EFI_STATUS alt = bs->AllocatePages(
                AllocateMaxAddress, EfiLoaderData, sfs_pages, &any);
            if (!EFI_ERROR(alt)) {
                bs->CopyMem((VOID *)any,
                            (VOID *)_binary_build_sfs_blob_start, sfs_size);
                g_sfs.base  = (UINT32)any;
                g_sfs.size  = (UINT32)sfs_size;
                g_sfs.magic = (UINT32)SFS_RAM_MAGIC;
                g_sfs_valid = 1;
                Print(L"      SFS staged @0x%lx (fallback, %d bytes)\r\n",
                      any, sfs_size);
            } else {
                Print(L"      *** SFS preload FAILED - GUI will be blank ***\r\n");
                g_sfs_valid = 0;
            }
        }

        /* Serial trace so headless runs can confirm the staging address */
        SERIAL_PUTC('['); SERIAL_PUTC('S'); SERIAL_PUTC('F');
        SERIAL_PUTC('S'); SERIAL_PUTC(']'); SERIAL_PUTC(' ');
        SERIAL_PUTHEX32(g_sfs.base);
        SERIAL_PUTC(' ');
        SERIAL_PUTHEX32(g_sfs.size);
        SERIAL_PUTC('\n');
    }

    /* ===========================================================
     *  Step 2: Set up graphics (GOP)
     * =========================================================== */
    Print(L"\r\n[2/5] Setting up graphics mode\r\n");

    /* ---- Allocate VbeInfo at 0x5000 ---- */
    EFI_PHYSICAL_ADDRESS vbe_addr = VBE_INFO_ADDR;
    EFI_STATUS vbe_alloc = bs->AllocatePages(
        AllocateAddress, EfiLoaderData, 1, &vbe_addr);
    int vbe_at_final = 0;

    if (!EFI_ERROR(vbe_alloc)) {
        vbe_at_final = 1;
    } else {
        Print(L"      0x5000 alloc failed, will write after ExitBS\r\n");
        vbe_at_final = 0;
    }

    /* ---- Reserve the boot scratch region (.lmboot) ----------------------
     *  enter_kernel.S's 32-bit stack USED to be 0x90000, which stopped being
     *  safe once the flat kernel image grew to 0x90568: the stack grew DOWN
     *  from 0x90000 straight into .data, where gdt64/gdt64_desc sit only 512
     *  bytes below.  switch_to_64bit's `lgdt` then loaded a shredded GDT and
     *  `mov ss, 0x10` #GP'd -> triple fault + reboot loop.
     *
     *  linker.ld now reserves LMBOOT_BASE..+LMBOOT_SIZE for the 32-bit boot
     *  stack, the long-mode page tables and the 64-bit boot stack.  We ask
     *  UEFI for it here as well, for two reasons:
     *    1. If the firmware has something live there (its own image, its
     *       stack, a runtime service) the allocation FAILS and we get a loud
     *       warning instead of a mystery reboot on real hardware.
     *    2. On success the firmware will not hand the range to anything else
     *       between now and ExitBootServices.
     *  COUPLING: LMBOOT_* must match the .lmboot block in linker.ld and
     *  LMBOOT_BASE/LMBOOT_SIZE in .attic64/kernel64.cpp. */
    EFI_PHYSICAL_ADDRESS lmboot_addr = LMBOOT_BASE;
    EFI_STATUS lmboot_alloc = bs->AllocatePages(
        AllocateAddress, EfiLoaderData, LMBOOT_SIZE / 4096, &lmboot_addr);
    if (!EFI_ERROR(lmboot_alloc)) {
        Print(L"      Boot scratch (stacks+PML4) reserved at 0x%lx\r\n",
              (UINT64)LMBOOT_BASE);
    } else {
        Print(L"      WARNING: 0x%lx reserve FAILED - firmware may own it;\r\n",
              (UINT64)LMBOOT_BASE);
        Print(L"               page tables/stacks will still be written after ExitBS\r\n");
    }

    /* ---- GOP setup ---- */
    {
        EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
        EFI_STATUS gopStatus = bs->LocateProtocol(&gopGuid, NULL, (VOID**)&gop);

        if (!EFI_ERROR(gopStatus) && gop != NULL && gop->Mode != NULL) {
            /* Use a local VbeInfo struct first, copy to 0x5000 later */
            struct VbeInfo vbe_local;
            bs->SetMem(&vbe_local, sizeof(vbe_local), 0);

            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *ginfo = gop->Mode->Info;
            UINT32 pxformat = PXF_BGRX32;
            UINT8  bpp_val = 32;

            /* ---- Determine pixel format and bpp from the CURRENT mode ---- */
            switch (ginfo->PixelFormat) {
            case PixelBlueGreenRedReserved8BitPerColor:
                pxformat = PXF_BGRX32;
                bpp_val = 32;
                break;
            case PixelRedGreenBlueReserved8BitPerColor:
                pxformat = PXF_RGBX32;
                bpp_val = 32;
                break;
            case PixelBitMask:
                if (ginfo->PixelInformation.RedMask   == 0x00FF0000 &&
                    ginfo->PixelInformation.GreenMask == 0x0000FF00 &&
                    ginfo->PixelInformation.BlueMask  == 0x000000FF) {
                    pxformat = PXF_BGRX32;
                    bpp_val = 32;
                } else if (ginfo->PixelInformation.RedMask   == 0xF800 &&
                           ginfo->PixelInformation.GreenMask == 0x07E0 &&
                           ginfo->PixelInformation.BlueMask  == 0x001F) {
                    pxformat = PXF_RGB565;
                    bpp_val = 16;
                } else {
                    pxformat = PXF_BGRX32;
                    bpp_val = 32;
                }
                break;
            case PixelBltOnly:
                pxformat = PXF_BLT_ONLY;
                bpp_val = 0;
                break;
            default:
                pxformat = PXF_BGRX32;
                bpp_val = 32;
                break;
            }

            /* ---- Keep the firmware's CURRENT GOP mode.  On real hardware the
             *      UEFI firmware powers on with the panel's NATIVE resolution
             *      already programmed, so leaving it alone makes the GUI fill
             *      the whole screen.  We deliberately do NOT enumerate/SetMode
             *      to a "larger" mode: OVMF's ramfb GOP QueryMode() lies and
             *      reports the current resolution for every mode index, so
             *      picking by area is unreliable and can DOWNgrade the mode
             *      (e.g. selecting 640x480 when 800x600 was already active). */
            if (pxformat == PXF_BLT_ONLY) {
                Print(L"      GOP is BltOnly - no linear framebuffer available\r\n");
            }

            if (pxformat == PXF_BLT_ONLY || gop->Mode->FrameBufferBase == 0) {
                Print(L"      No linear framebuffer available\r\n");
                vbe_local.vbe_ok = 0;
            } else {
                UINT64 fb_base = gop->Mode->FrameBufferBase;
                UINT32 fb_size = gop->Mode->FrameBufferSize;
                UINT16 width   = ginfo->HorizontalResolution;
                UINT16 height  = ginfo->VerticalResolution;
                UINT16 ppsl    = ginfo->PixelsPerScanLine;

                UINT16 pitch_val;
                if (bpp_val == 32) pitch_val = ppsl * 4;
                else if (bpp_val == 24) pitch_val = ppsl * 3;
                else if (bpp_val == 16) pitch_val = ppsl * 2;
                else pitch_val = ppsl * 4;

                Print(L"      GOP: %dx%d bpp=%d fmt=%d\r\n", width, height, bpp_val, pxformat);
                Print(L"      LFB=0x%lx size=%d pitch=%d\r\n", fb_base, fb_size, pitch_val);

                /* Handle framebuffer above 4GB.
                 * A 32-bit kernel cannot reach a >4GB physical framebuffer with a
                 * 32-bit pointer under the firmware's identity mapping (VA == PA).
                 * We keep the REAL 64-bit address in framebuffer_phys64 and leave
                 * shadow_buffer = 0.  The 32-bit kernel's vmm_init() then builds
                 * its own 4-level page tables (we run in long-mode compat) that
                 * map the real high framebuffer to a <4GB virtual window
                 * (0xF0000000) so 32-bit code can draw to it directly.  A shadow
                 * RAM buffer would need an extra copy that the display never
                 * scans, so we deliberately do NOT use one. */
                if (fb_base > 0xFFFFFFFFULL) {
                    Print(L"      FB above 4GB: kernel will 4-level map it\r\n");
                    vbe_local.framebuffer_phys   = (UINT32)(fb_base & 0xFFFFFFFF);
                    vbe_local.framebuffer_phys64 = fb_base;
                    vbe_local.shadow_buffer      = 0;
                } else {
                    vbe_local.framebuffer_phys   = (UINT32)fb_base;
                    vbe_local.shadow_buffer      = 0;
                    vbe_local.framebuffer_phys64 = fb_base;
                }

                vbe_local.width   = width;
                vbe_local.height  = height;
                vbe_local.bpp     = bpp_val;
                vbe_local.pitch   = pitch_val;
                vbe_local.pixel_format = pxformat;
                vbe_local.vbe_ok  = 1;
                vbe_local.vbe_mode_set = 1;

#ifdef TEST_HIGH_FB
                /* TEST ONLY (never set in shipping builds): pretend the GOP
                 * framebuffer lives above 4GB so the switch32to64.asm
                 * high-framebuffer mapping path is exercised under QEMU.
                 *
                 * 2026-08-19 -- WHY THE OLD VALUE MADE THIS TEST USELESS.
                 * The fake used to be 0x100000000 (4 GiB).  Its high dword is
                 * non-zero, so switch32to64.asm's `test edx,edx` did enter the
                 * high-FB branch -- but the resulting index is
                 *   pdpt_idx = 0x100000000 >> 30 = 4
                 * and PDPT[0..7] was ALREADY filled with 1 GiB identity pages
                 * covering 0..8 GiB.  The branch therefore just rewrote an
                 * entry that was already correct.  Every "high framebuffer
                 * path verified" run was really testing nothing, which is why
                 * the real bug (boot_beacon/diag_mark drawing to the
                 * 0xF0000000 compat window instead of fb64) survived so long.
                 *
                 * Real hardware (Intel Iris Xe) reports the LFB at
                 * 0x4000000000 == 256 GiB, i.e. pdpt_idx = 0x4000000000 >> 30
                 * = 256, an entry NOTHING else touches -- so it is the high-FB
                 * code, and only that code, which must create it.  Default to
                 * the genuine hardware address.
                 *
                 * QEMU obviously has no RAM at 256 GiB, so stores land in a
                 * black hole and the visible screen stays at whatever the
                 * 32-bit stage drew.  That is fine and intended: this test
                 * validates the MAPPING (now reported explicitly by
                 * fb_mapping_selfcheck()'s [FBMAP] serial lines) and proves no
                 * #PF/freeze -- it does not check on-screen pixels.  Pixel
                 * output is covered by the normal (low-FB) build. */
#ifndef TEST_HIGH_FB_ADDR
#define TEST_HIGH_FB_ADDR 0x4000000000ULL   /* real Intel Iris Xe GOP address */
#endif
                vbe_local.framebuffer_phys   = 0;
                vbe_local.framebuffer_phys64 = TEST_HIGH_FB_ADDR;
                Print(L"      [TEST_HIGH_FB] faking FB at 0x%lx (pdpt_idx=%d, exercises the real-metal high-FB path)\r\n",
                      (UINT64)TEST_HIGH_FB_ADDR,
                      (int)((TEST_HIGH_FB_ADDR >> 30) & 511));
#endif

                Print(L"      Graphics OK: %dx%d @%dbpp\r\n", width, height, bpp_val);
            }

            /* Serial debug */
            SERIAL_PUTC('[');
            SERIAL_PUTC('G');
            SERIAL_PUTC('O');
            SERIAL_PUTC('P');
            SERIAL_PUTC(']');
            SERIAL_PUTC(' ');
            SERIAL_PUTHEX64(gop->Mode->FrameBufferBase);
            SERIAL_PUTC('\n');

#ifdef FB_DIAG
            /* [GOPF] Raw GOP geometry/format so real-hardware mismatches are
             * visible in the serial log: the GOP PixelFormat enum value,
             * PixelsPerScanLine, and the PixelBitMask R/G/B masks (the last
             * are only meaningful when PixelFormat == PixelBitMask). */
            SERIAL_PUTC('['); SERIAL_PUTC('G'); SERIAL_PUTC('O');
            SERIAL_PUTC('P'); SERIAL_PUTC('F'); SERIAL_PUTC(']'); SERIAL_PUTC(' ');
            SERIAL_PUTDEC((UINT32)ginfo->PixelFormat);
            SERIAL_PUTC(' '); SERIAL_PUTHEX32(ginfo->PixelsPerScanLine);
            SERIAL_PUTC(' '); SERIAL_PUTHEX32(ginfo->PixelInformation.RedMask);
            SERIAL_PUTC(' '); SERIAL_PUTHEX32(ginfo->PixelInformation.GreenMask);
            SERIAL_PUTC(' '); SERIAL_PUTHEX32(ginfo->PixelInformation.BlueMask);
            SERIAL_PUTC('\n');
#endif

            /* Copy VbeInfo to 0x5000 (if allocated) or save for later */
            if (vbe_at_final) {
                bs->CopyMem((VOID *)VBE_INFO_ADDR, &vbe_local, sizeof(vbe_local));
            } else {
                /* Save in file-scope backup for post-ExitBS copy to 0x5000 */
                bs->CopyMem(&g_vbe_backup, &vbe_local, sizeof(vbe_local));
                g_vbe_valid = 1;
            }
        } else {
            Print(L"      GOP not available\r\n");
            SERIAL_PUTC('[');
            SERIAL_PUTC('N');
            SERIAL_PUTC('G');
            SERIAL_PUTC(']');
            SERIAL_PUTC('\n');
        }
    }

    /* ===========================================================
     *  Step 3: Exit boot services
     * =========================================================== */
    Print(L"\r\n[3/5] Exiting boot services\r\n");
    Print(L"      (screen will go dark briefly)\r\n");
    bs->Stall(1000000);  /* 1 second delay to read messages */

    UINTN mapSize = 0, mapKey = 0, descSize = 0;
    UINT32 descVer = 0;
    UINT8 *map = NULL;
    bs->GetMemoryMap(&mapSize, NULL, &mapKey, &descSize, NULL);
    mapSize += 4096;
    bs->AllocatePool(EfiLoaderData, mapSize, (VOID **)&map);
    bs->GetMemoryMap(&mapSize, (EFI_MEMORY_DESCRIPTOR *)map,
                     &mapKey, &descSize, &descVer);

    EFI_STATUS s = bs->ExitBootServices(Image, mapKey);
    if (EFI_ERROR(s)) {
        bs->GetMemoryMap(&mapSize, (EFI_MEMORY_DESCRIPTOR *)map,
                         &mapKey, &descSize, &descVer);
        s = bs->ExitBootServices(Image, mapKey);
    }
    if (EFI_ERROR(s)) {
        /* Can't use Print after ExitBootServices, but we haven't exited yet */
        /* This should not happen - if it does, halt */
        while (1) __asm__ __volatile__("hlt");
    }

    /* ===========================================================
     *  Step 4: Relocate kernel to 0x10000 if needed
     * ===========================================================
     *  After ExitBootServices, we own ALL memory. We can safely
     *  write to 0x10000, 0x5000, and 0x90000 without worrying
     *  about UEFI data structures. UEFI's identity-mapped page
     *  tables are still active, so all physical addresses work.
     */
    SERIAL_PUTC('[');
    SERIAL_PUTC('4');
    SERIAL_PUTC(']');

    if (!kern_at_final && kern_temp != 0) {
        /* Copy kernel from temp buffer to 0x10000 */
        inline_memcpy((void *)KERNEL_LOAD_ADDR, (const void *)kern_temp, kern_size);
        SERIAL_PUTC('K');  /* K = kernel relocated */
    }

    /* Ensure VbeInfo is at 0x5000 */
    if (!vbe_at_final && g_vbe_valid) {
        /* Copy VbeInfo from file-scope backup to 0x5000 */
        inline_memcpy((void *)VBE_INFO_ADDR, &g_vbe_backup, sizeof(g_vbe_backup));
        SERIAL_PUTC('V');  /* V = VbeInfo written to 0x5000 */
    }

    /* Relocate 64-bit kernel to K64_LOAD_ADDR if it was staged in a temp
     * buffer, then publish the handoff struct at 0x7000 so the 32-bit
     * kernel knows it must use this buffer instead of reading the disk. */
    if (g_k64_valid) {
        if (!g_k64_at_final && g_k64_temp != 0) {
            inline_memcpy((void *)K64_LOAD_ADDR, (const void *)g_k64_temp,
                          (UINTN)g_k64.size);
            SERIAL_PUTC('6');  /* 6 = K64 relocated to 0x1400000 */
        } else if (g_k64_at_final) {
            SERIAL_PUTC('6');  /* 6 = K64 already at 0x1400000 */
        }
        inline_memcpy((void *)K64_HANDOFF_ADDR, &g_k64, sizeof(g_k64));
        SERIAL_PUTC('7');  /* 7 = handoff struct written to 0x7000 */
    }

    /* Publish the RAM-SFS handoff at 0x0900.  Both kernels probe this: the
     * 32-bit one in kmain (before fs_init) and the 64-bit one in fs_init.
     * The image itself was staged before ExitBootServices and is NOT moved,
     * so only the descriptor needs writing here. */
    if (g_sfs_valid) {
        inline_memcpy((void *)SFS_HANDOFF_ADDR, &g_sfs, sizeof(g_sfs));
        SERIAL_PUTC('9');  /* 9 = RAM-SFS handoff written to 0x0900 */
    }

    /* Write a simple test pattern to VGA text buffer to prove we're alive */
    {
        volatile UINT16 *vga = (volatile UINT16 *)0xB8000;
        vga[0] = 0x0F00 | 'M';  /* White 'M' on black */
        vga[1] = 0x0F00 | 'O';  /* White 'O' on black */
        vga[2] = 0x0F00 | 'S';  /* White 'S' on black */
    }

    SERIAL_PUTC('\n');

    /* ===========================================================
     *  Step 5: Enter kernel
     * =========================================================== */
    SERIAL_PUTC('[');
    SERIAL_PUTC('5');
    SERIAL_PUTC(']');
    SERIAL_PUTC('\n');

    enter_kernel();
    return EFI_SUCCESS;
}
