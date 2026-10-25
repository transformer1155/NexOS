/* ============================================================================
 * driver_elf.c - real ELF32 loader for the NexOS hot-loadable driver framework
 * ----------------------------------------------------------------------------
 * Replaces mock_read_file().  Pipeline:
 *
 *      SFS path  --kern_fs_read-->  raw bytes
 *                --elf_validate--> Ehdr + Phdr sanity
 *                --elf_load----->  PT_LOAD segments copied to their vaddr,
 *                                  .rel.dyn / .rela.dyn applied (SHT_REL/
 *                                  SHT_RELA), shstrtab/strtab resolved
 *                --elf_export--->  find the DriverHeader export
 *
 * Everything is defensive: every offset is bounds-checked against the file
 * size before use, every pointer tested before dereference, and all failure
 * paths free whatever was allocated.
 *
 * NexOS note: the kernel only identity-maps the low 32 MiB (see vmm_init in
 * kernel.cpp), so a driver's PT_LOAD vaddr must live in that window.  We
 * validate that up front instead of faulting later.
 * ==========================================================================*/
#include "driver_contract.h"

/* ---- kernel / SFS services ---------------------------------------------- */
extern int  kern_fs_read(const char* name, unsigned char* buf, int bufsize);
/* True size of a filesystem object, or <0 if unknown.  Weak so the host-side
 * test harness (no filesystem) still links. */
extern int  kern_fs_size(const char* name) __attribute__((weak));
extern void* kmalloc(unsigned int size);
extern void  kfree(void* ptr);
extern void  serial_puts(const char* s);

/* ---- tiny freestanding helpers ------------------------------------------ */
static unsigned int e_strlen(const char* s)
{
    unsigned int n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

static int e_streq(const char* a, const char* b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; ++a; ++b; }
    return *a == *b;
}

static void e_memcpy(void* dst, const void* src, unsigned int n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (!d || !s) return;
    while (n--) *d++ = *s++;
}

static void e_memset(void* dst, int v, unsigned int n)
{
    unsigned char* d = (unsigned char*)dst;
    if (!d) return;
    while (n--) *d++ = (unsigned char)v;
}

static void e_puthex(unsigned int v)
{
    static const char* h = "0123456789ABCDEF";
    char b[11]; b[0]='0'; b[1]='x';
    for (int i = 0; i < 8; ++i) b[2+i] = h[(v >> (28 - i*4)) & 0xF];
    b[10] = 0;
    serial_puts(b);
}

static void e_putdec(unsigned int v)
{
    char b[12]; int n = 0;
    if (!v) { serial_puts("0"); return; }
    while (v && n < 11) { b[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    char o[12]; int k = 0;
    while (n) o[k++] = b[--n];
    o[k] = 0;
    serial_puts(o);
}

/* ==========================================================================
 * ELF32 structures (only what we need; defined locally so the driver loader
 * does not depend on any host <elf.h>).
 * ========================================================================*/
#define EI_NIDENT      16
#define EI_MAG0        0
#define EI_CLASS       4
#define EI_DATA        5
#define ELFCLASS32     1
#define ELFCLASS64     2   /* recognised (and refused) on 32-bit builds too */
#define ELFDATA2LSB    1
#define ET_REL         1      /* relocatable object                        */
#define ET_DYN         3      /* shared object / PIE                       */
#define EM_386         3

#define PT_LOAD        1
#define PF_X           1
#define PF_W           2
#define PF_R           4

#define SHT_NULL       0
#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHT_STRTAB     3
#define SHT_RELA       4
#define SHT_NOBITS     8
#define SHT_REL        9
#define SHT_DYNSYM     11

#define SHF_ALLOC      0x2
#define SHF_EXECINSTR  0x4

/* relocation types (i386) */
#define R_386_NONE     0
#define R_386_32       1
#define R_386_PC32     2

typedef struct { unsigned char e_ident[EI_NIDENT];
                 uint16_t e_type, e_machine; uint32_t e_version;
                 uint32_t e_entry, e_phoff, e_shoff, e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum;
                 uint16_t e_shentsize, e_shnum, e_shstrndx; } Elf32_Ehdr;

typedef struct { uint32_t p_type, p_offset, p_vaddr, p_paddr;
                 uint32_t p_filesz, p_memsz, p_flags, p_align; } Elf32_Phdr;

typedef struct { uint32_t sh_name, sh_type, sh_flags, sh_addr;
                 uint32_t sh_offset, sh_size, sh_link, sh_info;
                 uint32_t sh_addralign, sh_entsize; } Elf32_Shdr;

typedef struct { uint32_t r_offset, r_info; } Elf32_Rel;
typedef struct { uint32_t r_offset, r_info; int32_t r_addend; } Elf32_Rela;

typedef struct { uint32_t st_name, st_value, st_size;
                 unsigned char st_info, st_other; uint16_t st_shndx; } Elf32_Sym;

/* ==========================================================================
 * Handle returned to driver_loader.c.  Owns the loaded image.
 * ========================================================================*/
typedef struct ElfDriverImage {
    DriverHeader* header;      /* export, inside the loaded image          */
    void*         image_base;  /* contiguous kmalloc'd load region         */
    uint32_t      image_size;
    uint32_t      load_span;   /* highest (vaddr+filesz) - lowest vaddr    */
    uint32_t      entry;
    const char*   name;
} ElfDriverImage;

/* NexOS identity-maps the low 32 MiB; a driver must live there. */
#define ELF_MIN_VADDR   0x00100000u   /* above the first megabyte          */
#define ELF_MAX_VADDR   0x02000000u   /* 32 MiB ceiling                    */
#define ELF_MAX_IMAGE   (4u * 1024u * 1024u)

/* --------------------------------------------------------------------------
 * The ELF64 half is compiled ONLY into 64-bit builds.
 * --------------------------------------------------------------------------
 * A 32-bit kernel can never relocate an ELF64 image: DriverHeader and every
 * pointer inside it are pointer-sized, so parsing ELF64 there would read the
 * fields at the wrong offsets.  The 32-bit loader refuses EM_X86_64 anyway,
 * and kernel.bin has a hard 576 KiB budget -- so the parser is compiled out
 * entirely instead of being carried as dead code.  driver_elf_read() filters
 * an ELF64 image by e_ident[EI_CLASS] long before this code would be used.
 * ========================================================================*/
/* --------------------------------------------------------------------------
 * ELF64 half: compiled ONLY into 64-bit builds.
 * --------------------------------------------------------------------------
 * A 32-bit kernel can never relocate an ELF64 image -- DriverHeader and every
 * pointer in it are pointer-sized, so the fields would be read at the wrong
 * offsets -- and kernel.bin has a hard 576 KiB budget.  The 32-bit loader
 * therefore refuses ELF64 by e_ident[EI_CLASS] alone and never carries the
 * parser.  Nothing below this point is referenced on a 32-bit build.
 * ========================================================================*/
#if __SIZEOF_POINTER__ == 8
/* ==========================================================================
 * ELF64 structures (long mode).  Duplicated on purpose: the ELF32 path below
 * stays byte-for-byte untouched, and a shared/parameterised parser would make
 * both classes harder to audit.  Firmware for the 64-bit kernel is selected by
 * e_ident[EI_CLASS] in driver_elf_read().
 * ========================================================================*/
#define ELFCLASS64      2
#define EM_X86_64       62

/* x86-64 relocation types we can apply. */
#define R_X86_64_NONE   0
#define R_X86_64_64     1    /* S + A            (64-bit)   */
#define R_X86_64_PC32   2    /* S + A - P        (32-bit)   */
#define R_X86_64_PLT32  4    /* S + A - P        (32-bit)   */
#define R_X86_64_32    10    /* S + A            (zero-ext) */
#define R_X86_64_32S   11    /* S + A            (sign-ext) */

#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)((i) & 0xFFFFFFFFu))

typedef struct { unsigned char e_ident[EI_NIDENT];
                 uint16_t e_type, e_machine; uint32_t e_version;
                 uint64_t e_entry, e_phoff, e_shoff;
                 uint32_t e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum;
                 uint16_t e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;

typedef struct { uint32_t sh_name, sh_type;
                 uint64_t sh_flags, sh_addr, sh_offset, sh_size;
                 uint32_t sh_link, sh_info;
                 uint64_t sh_addralign, sh_entsize; } Elf64_Shdr;

/* ELF64 r_info is a 64-bit word: symbol index in the high half, type in the
 * low half.  Both relocation records are 8-byte aligned and 24/16 bytes. */
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
typedef struct { uint64_t r_offset, r_info; } Elf64_Rel;

typedef struct { uint32_t st_name;
                 unsigned char st_info, st_other; uint16_t st_shndx;
                 uint64_t st_value, st_size; } Elf64_Sym;

static void e_puthex64(uint64_t v)
{
    static const char* h = "0123456789ABCDEF";
    char b[19]; b[0]='0'; b[1]='x';
    for (int i = 0; i < 16; ++i) b[2+i] = h[(unsigned)((v >> (60 - i*4)) & 0xF)];
    b[18] = 0;
    serial_puts(b);
}
#endif  /* __SIZEOF_POINTER__ == 8: ELF64 structures */



/* ==========================================================================
 * Validation
 * ========================================================================*/
static DriverStatus elf_validate(const unsigned char* raw, uint32_t size,
                                 Elf32_Ehdr* out_hdr)
{
    if (!raw || !out_hdr) return DRIVER_ERR_INVALID_ARG;
    if (size < sizeof(Elf32_Ehdr)) {
        serial_puts("[ELF] image too small for Ehdr\n");
        return DRIVER_ERR_IO;
    }

    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)raw;

    /* magic */
    if (eh->e_ident[EI_MAG0]     != 0x7F ||
        eh->e_ident[EI_MAG0 + 1] != 'E'  ||
        eh->e_ident[EI_MAG0 + 2] != 'L'  ||
        eh->e_ident[EI_MAG0 + 3] != 'F') {
        serial_puts("[ELF] bad magic\n");
        return DRIVER_ERR_INVALID_MAGIC;
    }
    serial_puts("[ELF] Magic OK\n");

    if (eh->e_ident[EI_CLASS] != ELFCLASS32) {
        serial_puts("[ELF] not ELF32\n");
        return DRIVER_ERR_IO;
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        serial_puts("[ELF] not little-endian\n");
        return DRIVER_ERR_IO;
    }
    if (eh->e_machine != EM_386) {
        serial_puts("[ELF] not i386\n");
        return DRIVER_ERR_IO;
    }
    if (eh->e_type != ET_REL && eh->e_type != ET_DYN) {
        serial_puts("[ELF] not REL/DYN\n");
        return DRIVER_ERR_IO;
    }
    serial_puts("[ELF] class/endian/machine OK\n");

    /* program header table bounds (ET_DYN) */
    if (eh->e_phnum) {
        uint32_t need = eh->e_phoff + (uint32_t)eh->e_phnum * eh->e_phentsize;
        if (eh->e_phentsize != sizeof(Elf32_Phdr) || need > size) {
            serial_puts("[ELF] bad phdr table\n");
            return DRIVER_ERR_IO;
        }
    }
    /* section header table bounds (ET_REL) */
    if (eh->e_shnum) {
        uint32_t need = eh->e_shoff + (uint32_t)eh->e_shnum * eh->e_shentsize;
        if (eh->e_shentsize != sizeof(Elf32_Shdr) || need > size) {
            serial_puts("[ELF] bad shdr table\n");
            return DRIVER_ERR_IO;
        }
        if (eh->e_shstrndx >= eh->e_shnum) {
            serial_puts("[ELF] bad shstrndx\n");
            return DRIVER_ERR_IO;
        }
    }

    e_memcpy(out_hdr, eh, sizeof(Elf32_Ehdr));
    serial_puts("[ELF] header valid: type=");
    e_putdec(eh->e_type);
    serial_puts(" phnum=");
    e_putdec(eh->e_phnum);
    serial_puts(" shnum=");
    e_putdec(eh->e_shnum);
    serial_puts("\n");
    return DRIVER_OK;
}

/* ==========================================================================
 * Symbol resolution: an undefined symbol in the driver is looked up in the
 * kernel export table.  Mirrors resolve_symbol() in driver_loader.c, but is
 * kept local so this file has no link-time dependency on the loader.
 * ========================================================================*/
typedef struct { const char* name; void* addr; } ElfExport;

/* memcpy/memset are emitted by the compiler for struct copies and
 * initialisers in almost every freestanding C module, so a driver that does
 * not declare them (or a loader table that omits them) fails relocation.
 * Kept deliberately minimal otherwise. */
extern void* memcpy(void* d, const void* s, unsigned int n);
extern void* memset(void* d, int v, unsigned int n);

static const ElfExport g_elf_exports[] = {
    { "kmalloc",   (void*)&kmalloc       },
    { "kfree",     (void*)&kfree         },
    { "printk",    (void*)&serial_puts   },
    { "kern_fs_read", (void*)&kern_fs_read },
    { "memcpy",    (void*)&memcpy        },
    { "memset",    (void*)&memset        },
};
static const int g_elf_export_count =
    (int)(sizeof(g_elf_exports) / sizeof(g_elf_exports[0]));

static void* elf_resolve(const char* name)
{
    if (!name) return 0;
    for (int i = 0; i < g_elf_export_count; ++i)
        if (e_streq(g_elf_exports[i].name, name)) return g_elf_exports[i].addr;
    return 0;
}

/* ==========================================================================
 * Load: copy PT_LOAD segments, then apply relocations.
 * For ET_REL we build a flat image by placing each SHF_ALLOC section at its
 * sh_addr (the NexOS link script assigns fixed addresses) or, when sh_addr
 * is zero, at a packed cursor inside one contiguous buffer.
 * ========================================================================*/
static DriverStatus elf_load(const unsigned char* raw, uint32_t size,
                             const Elf32_Ehdr* eh, ElfDriverImage* out)
{
    if (!raw || !eh || !out) return DRIVER_ERR_INVALID_ARG;

    /* ---- determine the VA range we must reserve ---------------------- */
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    int alloc_sections = 0;

    for (uint32_t i = 0; i < eh->e_shnum; ++i) {
        const Elf32_Shdr* sh = (const Elf32_Shdr*)
            (raw + eh->e_shoff + i * eh->e_shentsize);
        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;
        if (sh->sh_type == SHT_NOBITS) { /* .bss: needs VA space, no file data */
        } else if (sh->sh_offset + sh->sh_size > size) {
            serial_puts("[ELF] section out of file bounds\n");
            return DRIVER_ERR_IO;
        }
        alloc_sections++;
        if (sh->sh_addr) {
            if (sh->sh_addr < lo) lo = sh->sh_addr;
            if (sh->sh_addr + sh->sh_size > hi) hi = sh->sh_addr + sh->sh_size;
        }
    }

    if (!alloc_sections) {
        serial_puts("[ELF] no SHF_ALLOC sections\n");
        return DRIVER_ERR_IO;
    }

    /* ---- Two-pass layout -------------------------------------------
     * ET_REL images carry sh_addr == 0.  We cannot invent a fake virtual
     * address and hand it to relocation code: the driver's code will index
     * memory through those values, so they MUST be the real load address.
     * Pass 1 computes relative offsets; after kmalloc we rewrite every
     * sh_addr to base + offset, then copy and relocate against real memory. */
    int packed = (hi <= lo);
    uint32_t span;

    /* Pass 1: compute relative offsets for packed images. */
    if (packed) {
        uint32_t cursor = 0;
        for (uint32_t i = 0; i < eh->e_shnum; ++i) {
            Elf32_Shdr* sh = (Elf32_Shdr*)(raw + eh->e_shoff + i * eh->e_shentsize);
            if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;
            if (sh->sh_addr) continue;               /* explicit address      */
            sh->sh_addr = cursor;                    /* SECTION-RELATIVE now  */
            cursor += (sh->sh_size + 15u) & ~15u;    /* 16-byte align         */
        }
        span = cursor;
        if (span == 0 || span > ELF_MAX_IMAGE) {
            serial_puts("[ELF] bad packed span\n");
            return DRIVER_ERR_IO;
        }
        lo = 0;
        hi = span;
    } else {
        span = hi - lo;
        if (span > ELF_MAX_IMAGE) {
            serial_puts("[ELF] image too large\n");
            return DRIVER_ERR_IO;
        }
        if (lo < ELF_MIN_VADDR || hi > ELF_MAX_VADDR) {
            serial_puts("[ELF] VA range outside identity map\n");
            return DRIVER_ERR_IO;
        }
    }

    /* ---- allocate one contiguous region --------------------------------- */
    void* base = kmalloc(span);
    if (!base) {
        serial_puts("[ELF] OOM for image span ");
        e_puthex(span);
        serial_puts("\n");
        return DRIVER_ERR_NO_MEMORY;
    }
    e_memset(base, 0, span);
    uint32_t base32 = (uint32_t)(uintptr_t)base;

    if (packed) {
        /* Pass 2: convert section-relative offsets into real addresses. */
        for (uint32_t i = 0; i < eh->e_shnum; ++i) {
            Elf32_Shdr* sh = (Elf32_Shdr*)(raw + eh->e_shoff + i * eh->e_shentsize);
            if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;
            sh->sh_addr = base32 + sh->sh_addr;
        }
        lo = base32;
        hi = base32 + span;
    }

    serial_puts("[ELF] load base=");
    e_puthex(base32);
    serial_puts(" span=");
    e_puthex(span);
    serial_puts(packed ? " (packed)\n" : " (fixed)\n");

    /* ---- copy SHF_ALLOC sections ------------------------------------- */
    int copied = 0;
    for (uint32_t i = 0; i < eh->e_shnum; ++i) {
        const Elf32_Shdr* sh = (const Elf32_Shdr*)
            (raw + eh->e_shoff + i * eh->e_shentsize);
        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;
        if (sh->sh_addr < lo) continue;

        uint32_t dst_off = sh->sh_addr - lo;
        if (dst_off + sh->sh_size > span) {
            serial_puts("[ELF] section overflows image\n");
            kfree(base);
            return DRIVER_ERR_IO;
        }
        if (sh->sh_type != SHT_NOBITS) {
            e_memcpy((unsigned char*)base + dst_off,
                     raw + sh->sh_offset, sh->sh_size);
        }
        /* SHT_NOBITS stays zeroed from the memset above */
        copied++;
    }
    serial_puts("[ELF] sections copied=");
    e_putdec((uint32_t)copied);
    serial_puts("\n");

#ifdef ELF_DEBUG_LAYOUT
    for (uint32_t i = 0; i < eh->e_shnum; ++i) {
        const Elf32_Shdr* sh = (const Elf32_Shdr*)
            (raw + eh->e_shoff + i * eh->e_shentsize);
        if (sh->sh_size == 0) continue;
        serial_puts("[ELF][sec] idx=");
        e_putdec(i);
        serial_puts(" flags=");
        e_puthex(sh->sh_flags);
        serial_puts(" addr=");
        e_puthex(sh->sh_addr);
        serial_puts(" size=");
        e_puthex(sh->sh_size);
        serial_puts(" type=");
        e_putdec(sh->sh_type);
        serial_puts("\n");
    }
#endif

    /* ---- locate the symbol/string tables for relocation -------------- */
    const Elf32_Shdr* symtab = 0;
    const Elf32_Shdr* strtab = 0;
    for (uint32_t i = 0; i < eh->e_shnum; ++i) {
        const Elf32_Shdr* sh = (const Elf32_Shdr*)
            (raw + eh->e_shoff + i * eh->e_shentsize);
        if (sh->sh_type == SHT_SYMTAB || sh->sh_type == SHT_DYNSYM) {
            symtab = sh;
            if (sh->sh_link < eh->e_shnum)
                strtab = (const Elf32_Shdr*)
                    (raw + eh->e_shoff + sh->sh_link * eh->e_shentsize);
            break;
        }
    }

    
/* ---- apply relocations ------------------------------------------- */
    uint32_t applied = 0, failed = 0;
    for (uint32_t i = 0; i < eh->e_shnum; ++i) {
        const Elf32_Shdr* sh = (const Elf32_Shdr*)
            (raw + eh->e_shoff + i * eh->e_shentsize);
        if (sh->sh_type != SHT_REL && sh->sh_type != SHT_RELA) continue;
        if (!symtab || !strtab) {
            serial_puts("[ELF] relocs but no symtab/strtab\n");
            kfree(base);
            return DRIVER_ERR_IO;
        }
        if (sh->sh_offset + sh->sh_size > size) {
            kfree(base);
            return DRIVER_ERR_IO;
        }

        uint32_t entsz = (sh->sh_type == SHT_RELA) ? sizeof(Elf32_Rela)
                                                   : sizeof(Elf32_Rel);
        if (sh->sh_entsize && sh->sh_entsize != entsz) {
            kfree(base);
            return DRIVER_ERR_IO;
        }
        uint32_t count = sh->sh_size / entsz;

        /* For ET_REL the relocation target is the section named by sh_info,
         * and r_offset is relative to THAT section -- not an absolute VA.
         * Resolve the target section once per relocation table. */
        uint32_t tgt_addr = 0;
        int      have_tgt = 0;
        if (sh->sh_info < eh->e_shnum) {
            const Elf32_Shdr* tgt = (const Elf32_Shdr*)
                (raw + eh->e_shoff + sh->sh_info * eh->e_shentsize);
            tgt_addr = tgt->sh_addr;
            have_tgt = (tgt->sh_size > 0);
        }

        for (uint32_t r = 0; r < count; ++r) {
            const unsigned char* ent = raw + sh->sh_offset + r * entsz;
            uint32_t off, info;
            int32_t addend = 0;
            if (sh->sh_type == SHT_RELA) {
                const Elf32_Rela* ra = (const Elf32_Rela*)ent;
                off = ra->r_offset; info = ra->r_info; addend = ra->r_addend;
            } else {
                const Elf32_Rel* rr = (const Elf32_Rel*)ent;
                off = rr->r_offset; info = rr->r_info;
            }

            uint32_t type = info & 0xFF;
            uint32_t symi = info >> 8;

            if (type == R_386_NONE) continue;

            if (!have_tgt) { failed++; continue; }
            /* Convert the section-relative offset into an absolute VA. */
            off += tgt_addr;

            /* where in our image does this relocation land? */
            if (off < lo || off + 4 > hi) { failed++; continue; }
            uint32_t* patch = (uint32_t*)((unsigned char*)base + (off - lo));

            uint32_t symval = 0;
            if (symi && symtab) {
                if (symi * sizeof(Elf32_Sym) + sizeof(Elf32_Sym) > symtab->sh_size) {
                    failed++; continue;
                }
                const Elf32_Sym* s = (const Elf32_Sym*)
                    (raw + symtab->sh_offset + symi * sizeof(Elf32_Sym));
                if (s->st_shndx == 0) {              /* undefined: kernel sym */
                    const char* nm = (const char*)
                        (raw + strtab->sh_offset + s->st_name);
                    if (s->st_name >= strtab->sh_size) { failed++; continue; }
                    void* ext = elf_resolve(nm);
                    if (!ext) {
                        serial_puts("[ELF] unresolved import: ");
                        serial_puts(nm);
                        serial_puts("\n");
                        failed++;
                        continue;
                    }
                    symval = (uint32_t)(uintptr_t)ext;
                } else {
                    /* A symbol defined in this image: for ET_REL, symbols
                     * with st_value==0 that point at a section (STT_SECTION)
                     * mean "start of that section", which is sh_addr after
                     * we assigned addresses. */
                    if (s->st_value == 0 && s->st_shndx < eh->e_shnum) {
                        const Elf32_Shdr* defsec = (const Elf32_Shdr*)
                            (raw + eh->e_shoff + s->st_shndx * eh->e_shentsize);
                        symval = defsec->sh_addr;
                    } else {
                        symval = s->st_value;
                    }
                }
            }

            switch (type) {
            case R_386_32:
                *patch = symval + (sh->sh_type == SHT_RELA ? (uint32_t)addend : *patch);
                break;
            case R_386_PC32:
                *patch = symval + (sh->sh_type == SHT_RELA ? (uint32_t)addend : *patch)
                         - (uint32_t)(uintptr_t)patch;
                break;
            default:
                /* Unsupported relocation: refuse rather than leave a bogus
                 * value that would fault at run time. */
                failed++;
                continue;
            }
            applied++;
        }
    }

    serial_puts("[ELF] relocs applied=");
    e_putdec(applied);
    serial_puts(" failed=");
    e_putdec(failed);
    serial_puts("\n");

    if (failed) {
        serial_puts("[ELF] relocation failure - refusing image\n");
        kfree(base);
        return DRIVER_ERR_SYMBOL_NOT_FOUND;
    }

    out->image_base = base;
    out->image_size = span;
    out->load_span  = span;
    out->entry      = eh->e_entry;
    return DRIVER_OK;
}

/* ==========================================================================
 * Export lookup: find the DriverHeader the driver publishes.
 * Accepts any symbol whose name begins with "nexos_driver_header" so both
 * the versioned (v1/v2) and plain names work.
 * ========================================================================*/
static DriverHeader* elf_find_header(const unsigned char* raw, uint32_t size,
                                     const Elf32_Ehdr* eh,
                                     const ElfDriverImage* img)
{
    if (!raw || !size || !eh || !img || !img->image_base) return 0;

    const Elf32_Shdr* symtab = 0;
    const Elf32_Shdr* strtab = 0;
    for (uint32_t i = 0; i < eh->e_shnum; ++i) {
        const Elf32_Shdr* sh = (const Elf32_Shdr*)
            (raw + eh->e_shoff + i * eh->e_shentsize);
        if (sh->sh_type == SHT_SYMTAB || sh->sh_type == SHT_DYNSYM) {
            symtab = sh;
            if (sh->sh_link < eh->e_shnum)
                strtab = (const Elf32_Shdr*)
                    (raw + eh->e_shoff + sh->sh_link * eh->e_shentsize);
            break;
        }
    }
    if (!symtab || !strtab) {
        serial_puts("[ELF] no symbol table (strip?)\n");
        return 0;
    }

    uint32_t count = symtab->sh_size / sizeof(Elf32_Sym);
    for (uint32_t i = 0; i < count; ++i) {
        const Elf32_Sym* s = (const Elf32_Sym*)
            (raw + symtab->sh_offset + i * sizeof(Elf32_Sym));
        if (!s->st_name || s->st_name >= strtab->sh_size) continue;
        if (s->st_shndx == 0) continue;             /* undefined */
        const char* nm = (const char*)(raw + strtab->sh_offset + s->st_name);

        if (e_strlen(nm) >= 18 &&
            nm[0]=='n' && nm[1]=='e' && nm[2]=='x' && nm[3]=='o' && nm[4]=='s' &&
            nm[5]=='_' && nm[6]=='d' && nm[7]=='r' && nm[8]=='i' && nm[9]=='v' &&
            nm[10]=='e' && nm[11]=='r' && nm[12]=='_' && nm[13]=='h' &&
            nm[14]=='e' && nm[15]=='a' && nm[16]=='d' && nm[17]=='e') {
            /* st_value is an absolute address in the loaded layout */
            /* ET_REL: st_value is an offset into its defining section.
             * Translate to the absolute address the image was loaded at. */
            uint32_t sym_addr = s->st_value;
            if (s->st_shndx < eh->e_shnum) {
                const Elf32_Shdr* defsec = (const Elf32_Shdr*)
                    (raw + eh->e_shoff + s->st_shndx * eh->e_shentsize);
                if (defsec->sh_addr) sym_addr = defsec->sh_addr + s->st_value;
            }
            uint32_t base32 = (uint32_t)(uintptr_t)img->image_base;
            if (sym_addr < base32 ||
                sym_addr + sizeof(DriverHeader) > base32 + img->image_size) {
                serial_puts("[ELF] header symbol outside image\n");
                continue;
            }
            serial_puts("[ELF] found export ");
            serial_puts(nm);
            serial_puts(" @");
            e_puthex(sym_addr);
            serial_puts("\n");
            return (DriverHeader*)(uintptr_t)sym_addr;
        }
    }

    serial_puts("[ELF] no nexos_driver_header export\n");
    return 0;
}

#if __SIZEOF_POINTER__ == 8
/* ==========================================================================
 * ELF64 loader (long mode)
 * --------------------------------------------------------------------------
 * Same pipeline as the ELF32 case -- validate, pack SHF_ALLOC sections into
 * one contiguous buffer, apply relocations, locate nexos_driver_header --
 * but with 64-bit section/symbol/relocation records and uintptr_t addresses.
 *
 * Takes ownership of 'raw' (frees it on every path).
 * ========================================================================*/
static int elf64_load_image(unsigned char* raw, uint32_t size, const char* path,
                            void** out_base, uint32_t* out_size,
                            ElfDriverImage** out_image)
{
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)raw;

    /* ---- validate ---------------------------------------------------- */
    if (size < sizeof(Elf64_Ehdr)) {
        serial_puts("[ELF64] image too small for Ehdr\n");
        kfree(raw); return DRIVER_ERR_IO;
    }
    if (eh->e_ident[EI_MAG0]     != 0x7F || eh->e_ident[EI_MAG0 + 1] != 'E' ||
        eh->e_ident[EI_MAG0 + 2] != 'L'  || eh->e_ident[EI_MAG0 + 3] != 'F') {
        serial_puts("[ELF64] bad magic\n");
        kfree(raw); return DRIVER_ERR_INVALID_MAGIC;
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        serial_puts("[ELF64] not little-endian\n");
        kfree(raw); return DRIVER_ERR_IO;
    }
    if (eh->e_machine != EM_X86_64) {
        serial_puts("[ELF64] not x86-64, machine=\n");
        e_putdec(eh->e_machine);
        kfree(raw); return DRIVER_ERR_IO;
    }
    if (eh->e_type != ET_REL && eh->e_type != ET_DYN) {
        serial_puts("[ELF64] not REL/DYN\n");
        kfree(raw); return DRIVER_ERR_IO;
    }
    if (eh->e_shnum == 0 || eh->e_shentsize != sizeof(Elf64_Shdr)) {
        serial_puts("[ELF64] bad shdr table\n");
        kfree(raw); return DRIVER_ERR_IO;
    }
    {
        uint64_t need = eh->e_shoff + (uint64_t)eh->e_shnum * eh->e_shentsize;
        if (need > (uint64_t)size) {
            serial_puts("[ELF64] shdr table out of file\n");
            kfree(raw); return DRIVER_ERR_IO;
        }
    }
    serial_puts("[ELF64] magic/class/endian/machine OK, shnum=");
    e_putdec(eh->e_shnum);
    serial_puts("\n");

    /* ---- pass 1: pack SHF_ALLOC sections ----------------------------- */
    /* ET_REL carries sh_addr == 0, so assign 16-byte-aligned offsets first
     * and rewrite them to real addresses once the buffer exists. */
    uint64_t cursor = 0;
    int alloc_sections = 0;
    for (uint32_t i = 0; i < eh->e_shnum; ++i) {
        Elf64_Shdr* sh = (Elf64_Shdr*)(raw + eh->e_shoff + (uint64_t)i * eh->e_shentsize);
        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;
        if (sh->sh_type != SHT_NOBITS &&
            sh->sh_offset + sh->sh_size > (uint64_t)size) {
            serial_puts("[ELF64] section out of file bounds\n");
            kfree(raw); return DRIVER_ERR_IO;
        }
        alloc_sections++;
        if (sh->sh_addr) continue;                 /* honour explicit address */
        sh->sh_addr = cursor;
        cursor += (sh->sh_size + 15u) & ~(uint64_t)15u;
    }
    if (!alloc_sections) {
        serial_puts("[ELF64] no SHF_ALLOC sections\n");
        kfree(raw); return DRIVER_ERR_IO;
    }
    if (cursor == 0 || cursor > (uint64_t)ELF_MAX_IMAGE) {
        serial_puts("[ELF64] bad span\n");
        kfree(raw); return DRIVER_ERR_IO;
    }
    uint64_t span = cursor;

    /* ---- allocate + rewrite section addresses ------------------------ */
    void* base = kmalloc((unsigned int)span);
    if (!base) {
        serial_puts("[ELF64] OOM for image span\n");
        kfree(raw); return DRIVER_ERR_NO_MEMORY;
    }
    e_memset(base, 0, (unsigned int)span);
    uintptr_t base_addr = (uintptr_t)base;

    for (uint32_t i = 0; i < eh->e_shnum; ++i) {
        Elf64_Shdr* sh = (Elf64_Shdr*)(raw + eh->e_shoff + (uint64_t)i * eh->e_shentsize);
        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;
        sh->sh_addr = (uint64_t)(base_addr + (uintptr_t)sh->sh_addr);
    }
    serial_puts("[ELF64] load base=");
    e_puthex64((uint64_t)base_addr);
    serial_puts(" span=");
    e_puthex64(span);
    serial_puts("\n");

    /* ---- copy section payloads --------------------------------------- */
    int copied = 0;
    for (uint32_t i = 0; i < eh->e_shnum; ++i) {
        const Elf64_Shdr* sh = (const Elf64_Shdr*)
            (raw + eh->e_shoff + (uint64_t)i * eh->e_shentsize);
        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;
        uintptr_t dst = (uintptr_t)sh->sh_addr;
        if (dst < base_addr || dst + sh->sh_size > base_addr + span) {
            serial_puts("[ELF64] section overflows image\n");
            kfree(base); kfree(raw); return DRIVER_ERR_IO;
        }
        if (sh->sh_type != SHT_NOBITS)
            e_memcpy((void*)dst, raw + sh->sh_offset, (unsigned int)sh->sh_size);
        copied++;
    }
    serial_puts("[ELF64] sections copied=");
    e_putdec((uint32_t)copied);
    serial_puts("\n");

    /* ---- symtab / strtab --------------------------------------------- */
    const Elf64_Shdr* symtab = 0;
    const Elf64_Shdr* strtab = 0;
    for (uint32_t i = 0; i < eh->e_shnum; ++i) {
        const Elf64_Shdr* sh = (const Elf64_Shdr*)
            (raw + eh->e_shoff + (uint64_t)i * eh->e_shentsize);
        if (sh->sh_type == SHT_SYMTAB || sh->sh_type == SHT_DYNSYM) {
            symtab = sh;
            if (sh->sh_link < eh->e_shnum)
                strtab = (const Elf64_Shdr*)
                    (raw + eh->e_shoff + (uint64_t)sh->sh_link * eh->e_shentsize);
            break;
        }
    }

/* ---- apply relocations ------------------------------------------- */
    uint32_t applied = 0, failed = 0;
    for (uint32_t i = 0; i < eh->e_shnum; ++i) {
        const Elf64_Shdr* sh = (const Elf64_Shdr*)
            (raw + eh->e_shoff + (uint64_t)i * eh->e_shentsize);
        if (sh->sh_type != SHT_REL && sh->sh_type != SHT_RELA) continue;
        if (!symtab || !strtab) {
            serial_puts("[ELF64] relocs but no symtab/strtab\n");
            kfree(base); kfree(raw); return DRIVER_ERR_IO;
        }
        if (sh->sh_offset + sh->sh_size > (uint64_t)size) {
            kfree(base); kfree(raw); return DRIVER_ERR_IO;
        }
        uint64_t entsz = (sh->sh_type == SHT_RELA) ? sizeof(Elf64_Rela)
                                                   : sizeof(Elf64_Rel);
        if (sh->sh_entsize && sh->sh_entsize != entsz) {
            kfree(base); kfree(raw); return DRIVER_ERR_IO;
        }
        uint64_t count = sh->sh_size / entsz;

        /* ET_REL: r_offset is relative to the section named by sh_info. */
        uint64_t tgt_addr = 0;
        int have_tgt = 0;
        if (sh->sh_info < eh->e_shnum) {
            const Elf64_Shdr* tgt = (const Elf64_Shdr*)
                (raw + eh->e_shoff + (uint64_t)sh->sh_info * eh->e_shentsize);
            tgt_addr = tgt->sh_addr;
            have_tgt = (tgt->sh_size > 0);
        }

        for (uint64_t r = 0; r < count; ++r) {
            const unsigned char* ent = raw + sh->sh_offset + r * entsz;
            uint64_t off;
            uint64_t info;
            int64_t addend = 0;
            if (sh->sh_type == SHT_RELA) {
                const Elf64_Rela* ra = (const Elf64_Rela*)ent;
                off = ra->r_offset; info = ra->r_info; addend = ra->r_addend;
            } else {
                const Elf64_Rel* rr = (const Elf64_Rel*)ent;
                off = rr->r_offset; info = rr->r_info;
            }
            uint32_t type = ELF64_R_TYPE(info);
            uint32_t symi = ELF64_R_SYM(info);
            if (type == R_X86_64_NONE) continue;
            if (!have_tgt) { failed++; continue; }
            off += tgt_addr;
            if (off < (uint64_t)base_addr || off + 8 > (uint64_t)base_addr + span) {
                failed++; continue;
            }

            uint64_t symval = 0;
            if (symi) {
                if ((uint64_t)symi * sizeof(Elf64_Sym) + sizeof(Elf64_Sym) > symtab->sh_size) {
                    failed++; continue;
                }
                const Elf64_Sym* sym = (const Elf64_Sym*)
                    (raw + symtab->sh_offset + (uint64_t)symi * sizeof(Elf64_Sym));
                if (sym->st_shndx == 0) {                 /* undefined -> kernel */
                    if (sym->st_name >= strtab->sh_size) { failed++; continue; }
                    const char* nm = (const char*)(raw + strtab->sh_offset + sym->st_name);
                    void* ext = elf_resolve(nm);
                    if (!ext) {
serial_puts("[ELF64] unresolved import: ");
                        serial_puts(nm);
                        serial_puts("\n");
                        failed++;
                        continue;
                    }
                    symval = (uint64_t)(uintptr_t)ext;
                } else if (sym->st_shndx < eh->e_shnum) {
                    const Elf64_Shdr* defsec = (const Elf64_Shdr*)
                        (raw + eh->e_shoff + (uint64_t)sym->st_shndx * eh->e_shentsize);
                    symval = defsec->sh_addr + sym->st_value;
                } else {
                    symval = sym->st_value;               /* SHN_ABS */
                }
            }

            uintptr_t P = base_addr + (uintptr_t)(off - (uint64_t)base_addr);
            switch (type) {
            case R_X86_64_64:
                *(uint64_t*)P = symval + (uint64_t)addend;
                break;
            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                int64_t v = (int64_t)symval + addend - (int64_t)P;
                *(int32_t*)P = (int32_t)v;
                break;
            }
            case R_X86_64_32:
            case R_X86_64_32S: {
                int64_t v = (int64_t)symval + addend;
                *(uint32_t*)P = (uint32_t)v;
                break;
            }
            default:
                serial_puts("[ELF64] unsupported reloc type\n");
                e_putdec(type);
                serial_puts("\n");
                failed++;
                continue;
            }
            applied++;
        }
    }
    serial_puts("[ELF64] relocs applied=");
    e_putdec(applied);
    serial_puts(" failed=");
    e_putdec(failed);
    serial_puts("\n");
    if (failed) {
        serial_puts("[ELF64] relocation failure - refusing image\n");
        kfree(base); kfree(raw);
        return DRIVER_ERR_SYMBOL_NOT_FOUND;
    }

    /* ---- locate the nexos_driver_header export ----------------------- */
    DriverHeader* hdr = 0;
    if (!symtab || !strtab) {
        serial_puts("[ELF64] no symbol table (strip?)\n");
    } else {
        uint64_t scount = symtab->sh_size / sizeof(Elf64_Sym);
        for (uint64_t i = 0; i < scount; ++i) {
            const Elf64_Sym* sym = (const Elf64_Sym*)
                (raw + symtab->sh_offset + i * sizeof(Elf64_Sym));
            if (!sym->st_name || sym->st_name >= strtab->sh_size) continue;
            if (sym->st_shndx == 0) continue;
            const char* nm = (const char*)(raw + strtab->sh_offset + sym->st_name);
            if (e_strlen(nm) < 18) continue;
            if (!(nm[0]=='n'&&nm[1]=='e'&&nm[2]=='x'&&nm[3]=='o'&&nm[4]=='s'&&
                  nm[5]=='_'&&nm[6]=='d'&&nm[7]=='r'&&nm[8]=='i'&&nm[9]=='v'&&
                  nm[10]=='e'&&nm[11]=='r'&&nm[12]=='_'&&nm[13]=='h'&&
                  nm[14]=='e'&&nm[15]=='a'&&nm[16]=='d'&&nm[17]=='e')) continue;
            uint64_t addr = sym->st_value;
            if (sym->st_shndx < eh->e_shnum) {
                const Elf64_Shdr* defsec = (const Elf64_Shdr*)
                    (raw + eh->e_shoff + (uint64_t)sym->st_shndx * eh->e_shentsize);
                addr = defsec->sh_addr + sym->st_value;
            }
            if (addr < (uint64_t)base_addr || addr + sizeof(DriverHeader) > (uint64_t)base_addr + span) {
                serial_puts("[ELF64] header symbol outside image\n");
                continue;
            }
            serial_puts("[ELF64] found export ");
            serial_puts(nm);
            serial_puts("\n");
            hdr = (DriverHeader*)(uintptr_t)addr;
            break;
        }
    }

    if (!hdr) {
        kfree(base); kfree(raw);
        return DRIVER_ERR_IO;
    }

    ElfDriverImage* img = (ElfDriverImage*)kmalloc(sizeof(ElfDriverImage));
    if (!img) {
        serial_puts("[ELF64] OOM for image record\n");
        kfree(base); kfree(raw);
        return DRIVER_ERR_NO_MEMORY;
    }
    e_memset(img, 0, sizeof(*img));
    img->name       = path;
    img->header     = hdr;
    img->image_base = base;
    img->image_size = (uint32_t)span;
    img->load_span  = (uint32_t)span;
    img->entry      = 0;                      /* ET_REL has no entry point */

    kfree(raw);                               /* file bytes no longer needed */

    *out_base  = img->image_base;
    *out_size  = img->image_size;
    *out_image = img;
    return DRIVER_OK;
}



#endif  /* __SIZEOF_POINTER__ == 8: ELF64 loader */

/* ==========================================================================
 * Public entry point, called by driver_loader.c in place of mock_read_file().
 * ========================================================================*/
int driver_elf_read(const char* path, void** out_base, uint32_t* out_size,
                    ElfDriverImage** out_image)
{
    if (!path || !out_base || !out_size || !out_image) return DRIVER_ERR_INVALID_ARG;

    /* ---- 1) read the file out of SFS -------------------------------- */
    /* kern_fs_read needs a caller-provided buffer.  We start with a probe
     * read into a modest stack-free heap buffer, then re-read at the real
     * size when the caller asks for more than one page. */
    const int PROBE = 4096;
    unsigned char* raw = (unsigned char*)kmalloc(PROBE);
    if (!raw) {
        /* Silent before: a heap that is not up yet made the very first
         * allocation fail and the caller only saw DRIVER_ERR_NO_MEMORY. */
        serial_puts("[ELF] OOM for probe buffer ");
        e_putdec((uint32_t)PROBE);
        serial_puts("\n");
        return DRIVER_ERR_NO_MEMORY;
    }

    int n = kern_fs_read(path, raw, PROBE);
    if (n <= 0) {
        serial_puts("[ELF] SFS read failed: ");
        serial_puts(path);
        serial_puts("\n");
        kfree(raw);
        return DRIVER_ERR_IO;
    }

    /* The SFS reader clamps to bufsize, so a full probe means "possibly
     * truncated".  Ask the filesystem for the TRUE size and allocate exactly
     * that, rather than blindly grabbing ELF_MAX_IMAGE (4 MiB).
     *
     * This used to kmalloc(ELF_MAX_IMAGE) on every driver whose size was a
     * multiple of the probe size.  At the point driver_boot_scan() runs --
     * right after the root filesystem is mounted, before the GUI heap has
     * grown -- a 4 MiB allocation fails, so every such driver reported
     * DRIVER_ERR_NO_MEMORY (-4) and was silently skipped.  Sizing the buffer
     * from the file's own metadata costs one extra stat and makes the load
     * independent of how early it happens. */
    uint32_t size = (uint32_t)n;
    if (n == PROBE) {
        int real = kern_fs_size ? kern_fs_size(path) : -1;
        uint32_t want = (real > 0) ? (uint32_t)real : (uint32_t)ELF_MAX_IMAGE;
        if (want < (uint32_t)PROBE) want = (uint32_t)PROBE;
        if (want > (uint32_t)ELF_MAX_IMAGE) want = (uint32_t)ELF_MAX_IMAGE;
        /* A driver image must at least be able to hold an ELF header. */
        unsigned char* big = (unsigned char*)kmalloc(want);
        if (!big) {
            serial_puts("[ELF] OOM for file buffer ");
            e_putdec(want);
            serial_puts("\n");
            kfree(raw);
            return DRIVER_ERR_NO_MEMORY;
        }
        int n2 = kern_fs_read(path, big, (int)want);
        if (n2 <= 0) { kfree(raw); kfree(big); return DRIVER_ERR_IO; }
        kfree(raw);
        raw  = big;
        size = (uint32_t)n2;
    }
    serial_puts("[ELF] read ");
    e_putdec(size);
    serial_puts(" bytes from SFS: ");
    serial_puts(path);
    serial_puts("\n");

    /* ---- 2a) ELF64 dispatch (purely additive) -----------------------
     * A long-mode kernel cannot execute ELF32 and a 32-bit kernel cannot
     * execute ELF64, so e_ident[EI_CLASS] selects the pipeline.  An
     * ELFCLASS32 image never enters this branch, which keeps the 32-bit
     * loading path below unchanged.
     *
     * The ELF64 branch is compiled only for 64-bit builds: DriverHeader and
     * every pointer inside it are pointer-sized, so parsing an ELF64 image in
     * a 32-bit kernel would silently read fields at the wrong offsets.  There
     * we refuse it as a wrong-architecture image instead. */
#if __SIZEOF_POINTER__ == 8
    if (size >= EI_NIDENT && raw[EI_CLASS] == ELFCLASS64)
        return elf64_load_image(raw, size, path, out_base, out_size, out_image);
#else
    if (size >= EI_NIDENT && raw[EI_CLASS] == ELFCLASS64) {
        serial_puts("[ELF] ELF64 image on a 32-bit kernel - skipped: ");
        serial_puts(path);
        serial_puts("\n");
        kfree(raw);
        return DRIVER_ERR_IO;
    }
#endif

    /* ---- 2) validate ------------------------------------------------ */
    Elf32_Ehdr eh;
    DriverStatus st = elf_validate(raw, size, &eh);
    if (st != DRIVER_OK) { kfree(raw); return (int)st; }

    /* ---- 3) load + relocate ----------------------------------------- */
    ElfDriverImage* img = (ElfDriverImage*)kmalloc(sizeof(ElfDriverImage));
    if (!img) {
        serial_puts("[ELF] OOM for image record ");
        e_putdec((uint32_t)sizeof(ElfDriverImage));
        serial_puts("\n");
        kfree(raw);
        return DRIVER_ERR_NO_MEMORY;
    }
    e_memset(img, 0, sizeof(*img));
    img->name = path;

    st = elf_load(raw, size, &eh, img);
    if (st != DRIVER_OK) { kfree(img); kfree(raw); return (int)st; }

    /* ---- 4) find the descriptor export ------------------------------ */
    DriverHeader* h = elf_find_header(raw, size, &eh, img);
    if (!h) {
        serial_puts("[ELF] no driver header - not a driver image\n");
        kfree(img->image_base);
        kfree(img);
        kfree(raw);
        return DRIVER_ERR_IO;
    }
    img->header = h;

    /* The raw file is no longer needed once the image is loaded. */
    kfree(raw);

    *out_base  = img->image_base;
    *out_size  = img->image_size;
    *out_image = img;
    return DRIVER_OK;
}

/* Address of the DriverHeader export inside the loaded image. */
void* elf_image_header(const ElfDriverImage* img)
{
    return (img && img->header) ? (void*)img->header : 0;
}

/* Free an image returned by driver_elf_read(). */
void driver_elf_free(ElfDriverImage* img)
{
    if (!img) return;
    if (img->image_base) kfree(img->image_base);
    kfree(img);
}
