/* =====================================================================
 *  efi_min.c  -  gnu-efi-free runtime shims for the NexOS UEFI loader
 * ---------------------------------------------------------------------
 *  Provides the two gnu-efi symbols bootuefi.c depends on:
 *    - InitializeLib()  (sets the global ST / BS pointers)
 *    - Print()          (minimal wprintf -> screen + serial mirror)
 *  Plus the global ST / BS the convenience macros expand to.
 *  Compiled with -mabi=ms so it matches the firmware ABI.
 * ===================================================================== */
#include "efi_min.h"
#include <stdarg.h>

EFI_SYSTEM_TABLE  *ST = NULL;
EFI_BOOT_SERVICES *BS = NULL;

/* EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID (per UEFI spec 2.x) */
EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID = {
    0x9042a9de, 0x23dc, 0x4a38,
    { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a }
};

void InitializeLib(EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
    (void)Image;
    ST = SystemTable;
    BS = SystemTable->BootServices;
}

/* ---- serial mirror (COM1, 0x3F8) ---- */
static void serial_putc(char c)
{
    __asm__ __volatile__("outb %0, %1" : : "a"((UINT8)c), "Nd"(0x3F8));
}

static const CHAR16 HEX[] = L"0123456789ABCDEF";

EFI_STATUS Print(IN CONST CHAR16 *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    CHAR16 out[600];
    int o = 0;
    for (const CHAR16 *p = fmt; *p && o < 580; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        p++;
        /* skip length modifiers (l/ll/h/z) - all integer args occupy an
         * 8-byte slot under the MS x64 ABI, so we always consume 64 bits */
        while (*p == 'l' || *p == 'L' || *p == 'h' || *p == 'z' || *p == 'I') p++;
        if (*p == 0) break;
        if (*p == '%') { out[o++] = '%'; continue; }
        if (*p == 'c') {
            CHAR16 c = (CHAR16) va_arg(ap, INT64);
            out[o++] = c; continue;
        }
        if (*p == 's') {
            const CHAR16 *s = va_arg(ap, const CHAR16 *);
            if (!s) s = L"(null)";
            while (*s && o < 580) out[o++] = *s++;
            continue;
        }
        if (*p == 'd' || *p == 'i') {
            INT64 v = va_arg(ap, INT64);
            if (v < 0) { out[o++] = '-'; v = -v; }
            CHAR16 t[24]; int i = 0;
            if (v == 0) t[i++] = '0';
            else while (v) { t[i++] = HEX[v % 10]; v /= 10; }
            while (i) out[o++] = t[--i];
            continue;
        }
        if (*p == 'u') {
            UINT64 v = va_arg(ap, UINT64);
            CHAR16 t[24]; int i = 0;
            if (v == 0) t[i++] = '0';
            else while (v) { t[i++] = HEX[v % 10]; v /= 10; }
            while (i) out[o++] = t[--i];
            continue;
        }
        if (*p == 'x' || *p == 'X') {
            UINT64 v = va_arg(ap, UINT64);
            CHAR16 t[24]; int i = 0;
            if (v == 0) t[i++] = '0';
            else while (v) { t[i++] = HEX[v % 16]; v /= 16; }
            while (i) out[o++] = t[--i];
            continue;
        }
        /* unknown specifier: copy verbatim */
        out[o++] = *p;
    }
    out[o] = 0;

    /* screen */
    if (ST && ST->ConOut && ST->ConOut->OutputString)
        ST->ConOut->OutputString(ST->ConOut, out);

    /* serial mirror (ASCII) */
    for (int i = 0; out[i]; i++) {
        CHAR16 c = out[i];
        if (c == '\r')      serial_putc('\r');
        else if (c == '\n') serial_putc('\n');
        else if (c >= 0x20 && c < 0x80) serial_putc((char)c);
    }
    va_end(ap);
    return EFI_SUCCESS;
}
