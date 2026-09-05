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
#define STACK_ADDR       0x90000ULL

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

/* Simple inline memcpy for use after ExitBootServices (no boot services available) */
static void inline_memcpy(void *dst, const void *src, UINTN n) {
    volatile UINT8 *d = (volatile UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    while (n--) *d++ = *s++;
}

/* File-scope VbeInfo backup - accessible after ExitBootServices */
static struct VbeInfo g_vbe_backup;
static int g_vbe_valid = 0;

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
    Print(L" MiniOS UEFI Bootloader v2.0\r\n");
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

    /* ---- Allocate stack at 0x90000 ---- */
    EFI_PHYSICAL_ADDRESS stack_addr_val = STACK_ADDR;
    EFI_STATUS stack_alloc = bs->AllocatePages(
        AllocateAddress, EfiLoaderData, 1, &stack_addr_val);
    if (!EFI_ERROR(stack_alloc)) {
        Print(L"      Stack at 0x90000 allocated\r\n");
    } else {
        Print(L"      0x90000 alloc failed (OK - will use after ExitBS)\r\n");
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

            /* ---- Determine pixel format and bpp ---- */
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
                Print(L"      GOP is BltOnly - searching for LFB mode\r\n");
                {
                    UINTN best_mode = 0xFFFFFFFF;
                    UINT32 best_area = 0;
                    UINTN mi;
                    for (mi = 0; mi < gop->Mode->MaxMode; mi++) {
                        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *m_info = NULL;
                        UINTN size_of_info = 0;
                        EFI_STATUS st = gop->QueryMode(gop, mi, &size_of_info, &m_info);
                        if (EFI_ERROR(st) || !m_info) continue;
                        if (m_info->PixelFormat == PixelBltOnly) {
                            bs->FreePool(m_info);
                            continue;
                        }
                        UINT32 area = m_info->HorizontalResolution * m_info->VerticalResolution;
                        if (area > best_area && area <= 1920*1200) {
                            best_area = area;
                            best_mode = mi;
                        }
                        bs->FreePool(m_info);
                    }
                    if (best_mode != 0xFFFFFFFF) {
                        Print(L"      Setting GOP mode %d\r\n", best_mode);
                        EFI_STATUS ss = gop->SetMode(gop, best_mode);
                        if (!EFI_ERROR(ss)) {
                            ginfo = gop->Mode->Info;
                            pxformat = (ginfo->PixelFormat == PixelRedGreenBlueReserved8BitPerColor)
                                       ? PXF_RGBX32 : PXF_BGRX32;
                            bpp_val = 32;
                        }
                    }
                }
                break;
            default:
                pxformat = PXF_BGRX32;
                bpp_val = 32;
                break;
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

                /* Handle framebuffer above 4GB */
                if (fb_base > 0xFFFFFFFFULL) {
                    Print(L"      WARNING: FB above 4GB, allocating shadow\r\n");
                    EFI_PHYSICAL_ADDRESS shadow = 0xFFFFFFFFULL;
                    UINTN spages = (fb_size + 4095) / 4096;
                    EFI_STATUS as = bs->AllocatePages(
                        AllocateMaxAddress, EfiLoaderData, spages, &shadow);
                    if (!EFI_ERROR(as)) {
                        Print(L"      Shadow at 0x%lx\r\n", shadow);
                        vbe_local.framebuffer_phys = (UINT32)shadow;
                        vbe_local.shadow_buffer = (UINT32)shadow;
                        vbe_local.framebuffer_phys64 = shadow;
                    } else {
                        Print(L"      Shadow alloc failed!\r\n");
                        vbe_local.framebuffer_phys = (UINT32)(fb_base & 0xFFFFFFFF);
                        vbe_local.shadow_buffer = 0;
                        vbe_local.framebuffer_phys64 = fb_base;
                    }
                } else {
                    vbe_local.framebuffer_phys = (UINT32)fb_base;
                    vbe_local.shadow_buffer = 0;
                    vbe_local.framebuffer_phys64 = fb_base;
                }

                vbe_local.width   = width;
                vbe_local.height  = height;
                vbe_local.bpp     = bpp_val;
                vbe_local.pitch   = pitch_val;
                vbe_local.pixel_format = pxformat;
                vbe_local.vbe_ok  = 1;
                vbe_local.vbe_mode_set = 1;

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
