; =====================================================================
;  switch32to64.asm  -  Transition from 32-bit PM to 64-bit long mode
; ---------------------------------------------------------------------
;  NASM ELF32 format, linked into the 32-bit kernel
;
;  void switch_to_64bit(uint32_t stage_phys);
;
;  The caller stages kernel64.bin in a heap buffer (`stage_phys`) instead of
;  reading it straight to 0x100000: the 32-bit kernel's own .bss sits at
;  0x120000 and a direct load corrupts the running ATA driver mid-read.
;  We blit the staged image down to 0x100000 here, where trashing 32-bit
;  globals no longer matters because nothing returns to C++.
; =====================================================================

[BITS 32]

; ---------------------------------------------------------------------
;  Long-mode page tables: RESERVED BY linker.ld, no longer hard-coded.
;
;  History of this address, because it moved twice for the same reason:
;    0x60000  -- free only while kernel.bin was small; the `rep stosd`
;                that clears the page-table pages ended up zeroing the
;                running kernel's own .text (hang after 'S', before 'P').
;    0x90000  -- also became unsafe.  The flat image grew to 0x90568, so
;                (a) the PML4/PDPT writes overwrote .data past 0x90000
;                (g_current, g_kernel_proc, g_skills, ...), and (b) the
;                32-bit stack, which grew DOWN from 0x90000, sat only 512
;                bytes above gdt64/gdt64_desc at 0x8FE00 and shredded the
;                very GDT `lgdt` loads below -> #GP on `mov ss` in long
;                mode -> triple fault + reboot loop, with serial stopping
;                right after the 'g' marker.
;
;  Both stacks and all page tables now come from the .lmboot region that
;  linker.ld reserves ABOVE .bss, so they track the image automatically
;  and PMM marks them used.  See linker.ld for the full layout.
; ---------------------------------------------------------------------
extern __lm_pml4
extern __lm_pdpt
extern __lm_pd
extern __lm_pd2
extern __lm_pdpt2
extern __lm_stack_top

%define PML4_ADDR       __lm_pml4
%define PDPT_ADDR       __lm_pdpt
%define PD_ADDR         __lm_pd        ; unused (1 GiB pages), kept for clarity
%define PD2_ADDR        __lm_pd2       ; unused (1 GiB pages), kept for clarity
%define PDPT2_ADDR      __lm_pdpt2     ; Extra PDPT for a >512GiB GOP framebuffer
%define KERNEL64_ENTRY  0x100000
; MUST cover the WHOLE kernel64.bin.  The 32-bit side stages KERNEL64_SECTORS
; (1440) sectors into stage_phys; anything we fail to blit here stays as
; whatever physical RAM held that page -- and rodata literals (e.g. the
; "msyh.ttf" string used by Sfs::find and font_vec's vec_init) live past the
; old 640 KiB window, so they read back as a zero page and strcmp() never
; matches -> vec_init fails (step 319).  Keep this == KERNEL64_SECTORS*512/4.
%define KERNEL64_DWORDS (1452 * 512 / 4)   ; MUST cover the whole kernel64.bin (now 742672 B = ~1452 sectors incl. embedded trampoline). Keep in sync with KERNEL64_SECTORS in kernel.cpp.

global switch_to_64bit

section .text

; ---- Serial debug helper (outputs one char to port 0x3F8) ----
%macro SERIAL_DBG 1
    ; NOTE: clobbers AX/DX only (no push/pop -- push is illegal in 64-bit
    ; mode and push eax is unencodable there).  Callers must not rely on
    ; AX/DX surviving a SERIAL_DBG call.
    mov dx, 0x3FD              ; LSR
%%w:
    in al, dx
    test al, 0x20             ; THRE (transmit hold reg empty)?
    jz %%w
    mov dx, 0x3F8
    mov al, %1
    out dx, al
%endmacro

switch_to_64bit:
    cli
    ; cdecl argument: stage_phys.  Grab it before anything touches the stack.
    mov esi, [esp + 4]
    SERIAL_DBG 'S'         ; Start

    ; ---- Check if already in long mode (UEFI compat path) ----
    mov ecx, 0xC0000080          ; EFER MSR
    rdmsr
    test eax, (1 << 8)           ; EFER.LME
    jnz .compat_path

    ; =================================================================
    ;  BIOS 32-bit path: Full transition
    ; =================================================================

    ; ---- 0. Disable paging FIRST ----
    ; The 32-bit kernel runs with its own paging enabled and that mapping
    ; is not guaranteed to translate the .lmboot region identically, so any
    ; page table we build through a virtual address could land on the wrong
    ; physical page while CR3 expects it at the linear address -> the CPU
    ; would triple-fault the instant long mode is entered.  Turn paging off
    ; here so every write below hits true physical RAM and CR3 lines up.
    mov eax, cr0
    and eax, 0x7FFFFFFF           ; Clear PG bit (bit 31)
    mov cr0, eax
    SERIAL_DBG 'd'         ; paging disabled (physical mode)

    ; ---- Blit the staged 64-bit image down to 0x100000 ----
    ; CRITICAL: this MUST run with paging OFF.  With paging on, writing the
    ; window 0x100000..0x180000 clobbers the 32-bit kernel's own page
    ; directory (g_page_directory_store @ 0x128000, inside the window) and
    ; every `movsd` past that point lands on the wrong physical page --
    ; the image tail (incl. gdt64 @ 0x1657E0) was garbage and the 64-bit
    ; entry #GP'd on `mov ds,0x10`.  With paging off, all writes are direct
    ; to physical RAM.  The blit also trashes .bss @ 0x120000..0x180000,
    ; which is deliberate: we never return to C++ from here.  switch_to_64bit
    ; itself lives at 0x10060 (< 1 MiB), so it is NOT overwritten.
    mov edi, KERNEL64_ENTRY
    mov ecx, KERNEL64_DWORDS
    cld
    rep movsd
    SERIAL_DBG 'M'         ; iMage copied into place

    ; (Removed: a TEMP DEBUG block that dumped 0x1657F0/0x1657F4/0x165808 as
    ;  "the copied 64-bit GDT".  Those addresses were only ever valid for one
    ;  historical build -- gdt64 lives in the 32-bit kernel's .data and moves
    ;  with every link -- so it was printing unrelated bytes and inviting
    ;  exactly the wrong conclusion during the triple-fault hunt.)

    ; ---- 1. Build identity-mapped page tables (now at physical addrs) ----
    ; Use pushad/popad to preserve registers
    pushad
    ; Clear 4 pages at PML4_ADDR (16384 bytes = 4096 dwords)
    mov edi, PML4_ADDR
    mov ecx, 4096
    xor eax, eax
    cld
    rep stosd

    ; PML4[0] -> PDPT (present=1, writable=1)
    ; NOTE: `+ 0x03` not `| 0x03` -- PDPT_ADDR is now a relocatable linker
    ; symbol and NASM cannot fold a bitwise OR into a relocation.  The
    ; symbol is 4 KiB-aligned, so + and | are equivalent here.
    mov dword [PML4_ADDR], PDPT_ADDR + 0x03

    ; PDPT[0..7] -> 1GiB pages, identity mapped, present=1, writable=1, PS=1.
    ; This covers 0..8 GiB of physical RAM so GB-scale model weights (loaded
    ; from NTFS) are directly accessible. The 3-4GB VBE LFB range is naturally
    ; included, so no separate PD2 mapping is needed.
    ;
    ; NOTE: 1 GiB pages need CPUID.80000001:EDX bit 26 (pdpe1gb).  QEMU's
    ; default CPU advertises it; VirtualBox's default feature set does not
    ; (and its NEM fallback rejects PS=1 at the PDPT level), so VBox users
    ; must enable the feature via  VBoxManage modifyvm --cpuidset 80000001.
    mov edi, PDPT_ADDR
    mov eax, 0x00000083           ; addr=0 | P=1 | W=1 | PS=1 (1GiB page)
    mov ecx, 8
.fill_pdpt:
    mov [edi], eax
    add eax, 0x40000000           ; next 1GiB block
    add edi, 8
    loop .fill_pdpt

    ; Mark the 3-4 GiB 1GiB page (PDPT[3], covering 0xC0000000-0xFFFFFFFF) as
    ; cache-disabled (PCD bit 4).  This region holds the LAPIC MMIO at
    ; 0xFEE00000 (and the VBE LFB); without PCD, reads of MMIO return stale
    ; cache lines (BIOS ROM shadow) instead of live LAPIC state.
    mov eax, [PDPT_ADDR + 24]
    or  eax, 0x10                 ; PCD (cache disable)
    mov [PDPT_ADDR + 24], eax

    popad

    SERIAL_DBG 'P'         ; Page tables built

    ; ---- 3. Set up CR4: clear PSE, enable PAE ----
    mov eax, cr4
    and eax, 0xFFFFFFEF           ; Clear PSE (bit 4)
    or eax, 0x00000020            ; Set PAE (bit 5)
    mov cr4, eax

    SERIAL_DBG 'A'         ; PAE enabled

    ; ---- 4. Load CR3 with PML4 address ----
    mov eax, PML4_ADDR
    mov cr3, eax

    SERIAL_DBG 'C'         ; CR3 loaded

    ; ---- 5. Set EFER.LME (MSR 0xC0000080, bit 8) ----
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x00000100            ; Set LME (bit 8)
    wrmsr

    SERIAL_DBG 'E'         ; EFER.LME set

    ; ---- 6. Enable paging (CR0.PG = bit 31) ----
    ; This is the magic moment: long mode activates!
    ; The CPU enters IA-32e compatibility mode (32-bit code in long mode)
    mov eax, cr0
    or eax, 0x80000000            ; Set PG bit
    mov cr0, eax

    ; If we get here, long mode is active in compatibility mode
    SERIAL_DBG 'L'         ; Long mode enabled

    ; ---- 7. Load 64-bit GDT ----
    lgdt [gdt64_desc]

    SERIAL_DBG 'G'         ; GDT loaded

    ; ---- 8. Far jump to 64-bit code segment (0x08) ----
    ; This switches from compat mode to true 64-bit mode
    jmp 0x08:long_mode_entry

.compat_path:
    ; =================================================================
    ;  UEFI compat mode path: Already in long mode, just switch CS
    ; =================================================================
    SERIAL_DBG 'U'         ; UEFI compat path

    ; -----------------------------------------------------------------
    ;  ORDER IS CRITICAL: install OUR page tables *before* blitting the
    ;  64-bit image to 0x100000.
    ;
    ;  Why: when the GOP framebuffer sits above 4 GiB the 32-bit kernel's
    ;  vmm_map_high_fb() builds 4-level tables in its own .bss and loads
    ;  them into CR3.  Those objects link at
    ;      g_win_pt 0x128000, g_pd 0x148000, g_pdpt 0x14C000, g_pml4 0x14D000
    ;  i.e. INSIDE the 0x100000..0x180000 window that the blit below
    ;  overwrites.  Blitting first therefore shreds the page tables the
    ;  CPU is actively walking (CR3 = 0x14D000) -> instant triple fault
    ;  and an endless reboot loop.  That was the real-metal "black screen
    ;  + memory error" bug: it only triggers when FB > 4 GiB, which is
    ;  why every <4 GiB QEMU run looked green.
    ;
    ;  Our tables live in linker.ld's .lmboot region (0x1800000, 24 MiB),
    ;  far outside the 0x100000..0x1A0000 blit range, so once CR3 points
    ;  there the blit can trample that window freely (nothing returns to the
    ;  32-bit C++ world anyway).
    ; -----------------------------------------------------------------

    ; UEFI's firmware page tables may mark free RAM (including 0x100000,
    ; where we are about to blit the 64-bit kernel) as NX, so executing it
    ; would #PF(NX) -> triple fault.  Build our own RWX identity (1 GiB)
    ; page tables at __lm_pml4 and switch CR3, exactly like the BIOS path.
    ; UEFI identity-maps all RAM, so the linear address of the .lmboot
    ; region equals its physical address and CR3 lines up.
    ; (pdpe1gb is required; QEMU advertises it.)
    pushad
    mov edi, PML4_ADDR
    mov ecx, 4096
    xor eax, eax
    cld
    rep stosd
    mov dword [PML4_ADDR], PDPT_ADDR + 0x03   ; + not | : relocatable symbol
    mov edi, PDPT_ADDR
    mov eax, 0x00000083           ; addr=0 | P=1 | W=1 | PS=1 (1GiB page)
    mov ecx, 8
.fill_pdpt_u:
    mov [edi], eax
    add eax, 0x40000000           ; next 1GiB block
    add edi, 8
    loop .fill_pdpt_u
    popad
    SERIAL_DBG 'P'         ; Page tables built

    ; ---- Map a GOP framebuffer that lives above 4 GiB ---------------
    ; The 8 x 1 GiB identity pages above only cover 0..8 GiB.  Real
    ; hardware (Intel Iris Xe and friends) reports the linear framebuffer
    ; at 0x4000000000 (256 GiB), so the 64-bit kernel's first pixel write
    ; would #PF against an absent PDPT entry -- the machine keeps running
    ; (fan spinning) but the screen stays black, which is exactly the
    ; real-metal symptom we were chasing.
    ;
    ; VbeInfo.framebuffer_phys64 lives at 0x5010 (vbe_ok flag at 0x500D).
    ; When it is above 4 GiB we add a 1 GiB identity page for it, plus the
    ; next one in case the framebuffer straddles the 1 GiB boundary.
    ; The 64-bit GUI consults framebuffer_phys64 and draws at that REAL
    ; address, so an identity mapping (linear == physical == fb) is what it
    ; needs -- NOT the 0xF0000000 window the 32-bit kernel uses.
    pushad
    cmp byte [0x500D], 1          ; vbe_ok?
    jne .no_high_fb
    mov eax, [0x5010]             ; framebuffer_phys64 low  dword
    mov edx, [0x5014]             ; framebuffer_phys64 high dword
    test edx, edx
    jz .no_high_fb                ; <= 4 GiB: already identity-mapped

    ; pdpt_idx = (fb >> 30) & 511
    mov ecx, eax
    shr ecx, 30
    mov ebx, edx
    shl ebx, 2
    or  ecx, ebx
    and ecx, 511

    ; pml4_idx = fb >> 39 == fb_hi >> 7
    mov ebx, edx
    shr ebx, 7
    test ebx, ebx
    jz .fb_pml4_zero

    ; Framebuffer sits beyond PML4[0]'s 512 GiB window: hang a second
    ; PDPT off PML4[pml4_idx].
    push ecx
    push edx
    mov edi, PDPT2_ADDR
    mov ecx, 1024                 ; zero 4 KiB
    xor eax, eax
    cld
    rep stosd
    pop edx
    pop ecx
    mov dword [PML4_ADDR + ebx*8],     PDPT2_ADDR + 0x03  ; + not | : reloc
    mov dword [PML4_ADDR + ebx*8 + 4], 0
    mov edi, PDPT2_ADDR
    jmp .fb_write

.fb_pml4_zero:
    mov edi, PDPT_ADDR

.fb_write:
    ; entry = (fb & 0xFFFFFFFF_C0000000) | P | W | PS(1 GiB)
    mov eax, [0x5010]
    and eax, 0xC0000000
    or  eax, 0x83
    mov [edi + ecx*8],     eax
    mov [edi + ecx*8 + 4], edx
    ; second 1 GiB page (boundary straddle guard)
    inc ecx
    cmp ecx, 512
    jae .no_high_fb
    add eax, 0x40000000
    jnc .fb_write2
    inc edx
.fb_write2:
    mov [edi + ecx*8],     eax
    mov [edi + ecx*8 + 4], edx
.no_high_fb:
    popad
    SERIAL_DBG 'H'         ; High framebuffer mapping handled

    ; Belt-and-suspenders: clear EFER.NXE so any residual NX attribute
    ; on a mapped page cannot trap us.
    mov ecx, 0xC0000080          ; EFER
    rdmsr
    and eax, 0xFFFFF7FF           ; clear bit 11 (NXE)
    wrmsr
    SERIAL_DBG 'X'         ; NXE cleared

    mov eax, PML4_ADDR
    mov cr3, eax
    SERIAL_DBG 'C'         ; CR3 loaded

    ; ---- Only NOW blit the staged 64-bit image down to 0x100000 ------
    ; The image was staged in a heap buffer (stage_phys), NOT pre-loaded
    ; at 0x100000 the way BIOS stage2 does it.  The BIOS full-transition
    ; path blits it with paging OFF; here paging stays on, but CR3 now
    ; points at our .lmboot tables (RWX identity 0..8 GiB, 1 GiB pages), so
    ; the write is safe and can no longer destroy the live page tables.
    mov esi, [esp + 4]           ; reload stage_phys (pushad/popad balanced)
    mov edi, KERNEL64_ENTRY
    mov ecx, KERNEL64_DWORDS
    cld
    rep movsd
    SERIAL_DBG 'M'         ; iMage copied into place

    lgdt [gdt64_desc]
    jmp 0x08:long_mode_entry

; =====================================================================
;  64-bit code entry point
; =====================================================================
[BITS 64]
long_mode_entry:
    ; We are now in 64-bit long mode!

    SERIAL_DBG '6'         ; 64-bit entry reached

    ; Load flat data segments.  SERIAL_DBG clobbers AX, so keep the
    ; selector in BX and reload AX before each load.
    mov bx, 0x10
    mov ax, bx
    mov ds, ax
    SERIAL_DBG 'a'         ; ds loaded
    mov ax, bx
    mov es, ax
    SERIAL_DBG 'e'         ; es loaded
    mov ax, bx
    mov fs, ax
    SERIAL_DBG 'f'         ; fs loaded
    mov ax, bx
    mov gs, ax
    SERIAL_DBG 'g'         ; gs loaded
    mov ax, bx
    mov ss, ax
    SERIAL_DBG 'b'         ; ss loaded

    ; ---- Set up the 64-bit boot stack ----
    ; Was hard-coded 0x1F0000, which sits INSIDE kernel64's .bss
    ; (0x19C000..0x3A2C60): the 64-bit stack and the 64-bit kernel's
    ; globals were silently sharing the same pages.  __lm_stack_top is a
    ; dedicated 64 KiB region reserved by the 32-bit linker.ld.
    ;
    ; `mov esp, imm32` (not `mov rsp, ...`): in 64-bit mode a 32-bit
    ; register write zero-extends into the full 64-bit register, and this
    ; form takes a plain 32-bit relocation that NASM can emit from an
    ; elf32 object.
    mov esp, __lm_stack_top
    SERIAL_DBG 'c'         ; rsp set

    ; Ensure interrupts are off
    cli

    ; Jump to the 64-bit kernel at 0x100000 (1MB)
    mov rax, KERNEL64_ENTRY
    SERIAL_DBG 'd'         ; about to jmp to 0x100000
    jmp rax

; =====================================================================
;  64-bit GDT (used during transition)
; =====================================================================
section .data
align 8

gdt64:
    dq 0x0000000000000000       ; 0x00: Null descriptor

gdt64_code:                     ; 0x08: 64-bit code (L=1, D=0)
    dq 0x00AF9A000000FFFF       ; P=1 DPL=0 S=1 Type=code G=1 L=1

gdt64_data:                     ; 0x10: Data (flat, G=1, D=1)
    dq 0x00CF92000000FFFF       ; P=1 DPL=0 S=1 Type=data G=1 D=1

gdt64_end:

; GDT descriptor (32-bit format: 2-byte limit + 4-byte base)
gdt64_desc:
    dw gdt64_end - gdt64 - 1
    dd gdt64
