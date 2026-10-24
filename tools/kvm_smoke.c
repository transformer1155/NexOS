/* =====================================================================
 *  kvm_smoke.c  -  host-side KVM test harness for the NexOS VMM guest
 * =====================================================================
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  The NexOS built-in VMM (plugins/vmm.cpp) runs a hardware-virtualized
 *  guest.  Writing that guest-entry code is risky: a mistake in the VMCS /
 *  VMCB setup or the exit handler produces a triple fault with no output.
 *
 *  This harness validates the SAME guest program (plugins/guest_smoke.asm)
 *  on a real hypervisor -- KVM -- from ordinary Linux user space.  It proves:
 *
 *     1. the guest blob is valid 32-bit code and runs to completion,
 *     2. it really writes the shared framebuffer region (we check pixels),
 *     3. it emits "ready" on the UART path (we watch the port exits),
 *     4. it triggers a VM-exit cleanly instead of faulting.
 *
 *  If this passes, the guest payload is known-good and any remaining bug in
 *  NexOS is in the VMCB/VMCS plumbing, not the guest.  That is exactly the
 *  isolation a bring-up harness should provide.
 *
 *  BUILD & RUN (inside WSL or any Linux with /dev/kvm)
 *  ---------------------------------------------------
 *    gcc -O2 -o /tmp/kvm_smoke tools/kvm_smoke.c
 *    sudo /tmp/kvm_smoke          # /dev/kvm needs read-write access
 *
 *  The harness is deliberately self-contained: it parses the flat guest
 *  binary, sets up a 32-bit flat segment, runs the guest in KVM, services
 *  the UART/exit port I/O exits, and then verifies the framebuffer.
 * =====================================================================
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/kvm.h>

/* ---- guest memory map (must match plugins/guest_smoke.asm) ----------- */
#define GUEST_MEM_SIZE   (16u * 1024 * 1024)   /* 16 MiB flat identity space */
#define GUEST_CODE_PHYS  0x00001000u
#define GUEST_STACK_TOP  0x00008000u
#define GUEST_FB_PHYS    0x00100000u
#define GUEST_FB_W       1024u
#define GUEST_FB_H       768u
#define GUEST_FB_BYTES   (GUEST_FB_W * GUEST_FB_H * 4u)

#define UART_PORT        0x03F8u
#define EXIT_PORT        0x0F00u

static int g_fail = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  [ OK ] %s\n", msg);                                   \
        } else {                                                            \
            printf("  [FAIL] %s\n", msg);                                   \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

/* ---- load the flat guest binary ------------------------------------- */
static uint8_t* load_guest(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t* buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

int main(int argc, char** argv) {
    const char* blob_path = (argc > 1) ? argv[1] : "plugins/guest_smoke.bin";

    printf("=== NexOS VMM guest smoke test (KVM) ===\n");
    printf("guest blob : %s\n", blob_path);

    size_t blob_len = 0;
    uint8_t* blob = load_guest(blob_path, &blob_len);
    if (!blob) {
        fprintf(stderr, "error: cannot read guest blob '%s'\n", blob_path);
        fprintf(stderr, "build it with: nasm -f bin plugins/guest_smoke.asm -o plugins/guest_smoke.bin\n");
        return 2;
    }
    printf("blob size  : %zu bytes\n\n", blob_len);

    /* ---- open /dev/kvm -------------------------------------------- */
    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) {
        fprintf(stderr, "error: open /dev/kvm: %s\n", strerror(errno));
        fprintf(stderr, "(needs root or membership in the kvm group, and nested virt enabled)\n");
        free(blob);
        return 2;
    }
    int api = ioctl(kvm, KVM_GET_API_VERSION, 0);
    printf("KVM API version: %d\n", api);
    CHECK(api == KVM_API_VERSION, "KVM API version matches");

    /* ---- create the VM -------------------------------------------- */
    int vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0) { perror("KVM_CREATE_VM"); free(blob); return 2; }

    /* ---- guest RAM: one 16 MiB slot, identity mapped -------------- */
    size_t map_size = GUEST_MEM_SIZE;
    uint8_t* mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 2; }

    struct kvm_userspace_memory_region region;
    memset(&region, 0, sizeof(region));
    region.slot            = 0;
    region.guest_phys_addr = 0;
    region.memory_size     = map_size;
    region.userspace_addr  = (uint64_t)(uintptr_t)mem;
    if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return 2;
    }
    CHECK(1, "16 MiB guest RAM mapped identity (slot 0)");

    /* ---- place the guest code and patch its message pointer ------- */
    if (GUEST_CODE_PHYS + blob_len > map_size) {
        fprintf(stderr, "guest blob does not fit\n");
        return 2;
    }
    memcpy(mem + GUEST_CODE_PHYS, blob, blob_len);
    CHECK(1, "guest blob copied to 0x1000");

    /* Zero the framebuffer so we can detect what the guest wrote. */
    memset(mem + GUEST_FB_PHYS, 0, GUEST_FB_BYTES);

    /* ---- create one vCPU ------------------------------------------ */
    int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu < 0) { perror("KVM_CREATE_VCPU"); return 2; }

    int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    struct kvm_run* run = mmap(NULL, (size_t)run_size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED) { perror("mmap kvm_run"); return 2; }
    CHECK(1, "vCPU created and kvm_run mapped");

    /* ---- initial CPU state: 32-bit flat protected mode ------------ */
    struct kvm_sregs sregs;
    if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0) { perror("KVM_GET_SREGS"); return 2; }

    /* Flat 4 GiB code and data segments, base 0, 32-bit. */
    struct kvm_segment flat;
    memset(&flat, 0, sizeof(flat));
    flat.base     = 0;
    flat.limit    = 0xFFFFFFFF;
    flat.selector = 0x08;
    flat.type     = 11;          /* code: execute/read, accessed   */
    flat.present  = 1;
    flat.dpl      = 0;
    flat.db       = 1;           /* 32-bit                         */
    flat.s        = 1;
    flat.l        = 0;
    flat.g        = 1;           /* 4 KiB granularity              */
    sregs.cs = flat;

    flat.selector = 0x10;
    flat.type     = 3;           /* data: read/write, accessed     */
    sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = flat;

    /* Real (unpaged) mode is simplest for the smoke test: no paging. */
    sregs.cr0 = 0x60000001;      /* PE | ET (no paging, no protected-vm bits) */
    sregs.cr3 = 0;
    sregs.cr4 = 0;
    sregs.efer = 0;

    /* GDT with the two flat descriptors above. */
    struct kvm_dtable gdt;
    memset(&gdt, 0, sizeof(gdt));
    gdt.base  = 0;
    gdt.limit = 0xFFFF;
    sregs.gdt = gdt;

    if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) { perror("KVM_SET_SREGS"); return 2; }

    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rip    = GUEST_CODE_PHYS;
    regs.rflags = 0x2;           /* reserved bit 1 must be set */
    regs.rsp    = GUEST_STACK_TOP;
    regs.rbp    = GUEST_STACK_TOP;
    if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) { perror("KVM_SET_REGS"); return 2; }
    CHECK(1, "vCPU state set (32-bit flat, RIP=0x1000)");

    /* ---- run the guest, servicing I/O exits ----------------------- */
    printf("\n--- running guest ---\n");
    char uart_buf[256];
    size_t uart_len = 0;
    int exits = 0, io_exits = 0, done = 0;

    for (int spins = 0; spins < 200000 && ioctl(vcpu, KVM_RUN, 0) >= 0; spins++) {
        exits++;
        if (run->exit_reason == KVM_EXIT_IO) {
            io_exits++;
            uint16_t port = run->io.port;
            uint8_t* data = (uint8_t*)run + run->io.data_offset;
            if (run->io.direction == KVM_EXIT_IO_OUT && run->io.size == 1) {
                if (port == UART_PORT) {
                    if (uart_len + 1 < sizeof(uart_buf)) uart_buf[uart_len++] = (char)*data;
                } else if (port == EXIT_PORT) {
                    done = 1;
                    break;
                }
            }
        } else if (run->exit_reason == KVM_EXIT_HLT) {
            printf("  guest HLT\n");
            done = 1;
            break;
        } else if (run->exit_reason == KVM_EXIT_SHUTDOWN) {
            printf("  guest triple-faulted (shutdown)\n");
            break;
        } else {
            printf("  unexpected exit_reason=%u\n", run->exit_reason);
            break;
        }
    }

    uart_buf[uart_len] = 0;
    printf("\n--- results ---\n");
    printf("exits     : %d (%d I/O)\n", exits, io_exits);
    printf("uart text : \"%s\"\n", uart_buf);

    CHECK(exits > 0,                 "guest executed at least one exit");
    CHECK(io_exits > 0,              "guest performed port I/O (UART/exit ports)");
    CHECK(done == 1,                 "guest reached its exit port / HLT");

    /* "ready\n" must have arrived through the UART path. */
    CHECK(strstr(uart_buf, "ready") != NULL, "guest emitted \"ready\" on the vUART");

    /* ---- verify the shared framebuffer was painted ---------------- */
    uint32_t* fb = (uint32_t*)(mem + GUEST_FB_PHYS);
    uint32_t p00 = fb[0];
    uint32_t p_mid = fb[(GUEST_FB_H / 2) * GUEST_FB_W + GUEST_FB_W / 2];
    uint32_t p_last = fb[GUEST_FB_W * GUEST_FB_H - 1];
    int nonzero = 0;
    for (uint32_t i = 0; i < GUEST_FB_W * GUEST_FB_H; i++)
        if (fb[i]) nonzero++;

    printf("\n--- framebuffer (shared RAM) ---\n");
    printf("pixel(0,0)        : 0x%06X\n", p00 & 0xFFFFFF);
    printf("pixel(512,384)    : 0x%06X\n", p_mid & 0xFFFFFF);
    printf("pixel(1023,767)   : 0x%06X\n", p_last & 0xFFFFFF);
    printf("non-zero pixels   : %d / %u\n", nonzero, GUEST_FB_W * GUEST_FB_H);

    CHECK(nonzero == (int)(GUEST_FB_W * GUEST_FB_H), "guest painted every framebuffer pixel");
    CHECK(p00 != p_mid && p_mid != p_last,           "gradient varies across the framebuffer");
    CHECK((p_mid & 0x80) == 0x80,                    "blue channel present (0x80) as designed");

    /* ---- verdict -------------------------------------------------- */
    printf("\n=== %s ===\n", g_fail ? "FAILED" : "ALL CHECKS PASSED");
    if (!g_fail)
        printf("The guest payload is valid: it runs, paints shared RAM,\n"
               "emits \"ready\" on the vUART, and exits cleanly.\n"
               "Any remaining NexOS VMM bug is in VMCB/VMCS plumbing,\n"
               "not in the guest program.\n");

    munmap(fb, 0);
    munmap(run, (size_t)run_size);
    munmap(mem, map_size);
    close(vcpu);
    close(vm);
    close(kvm);
    free(blob);
    return g_fail ? 1 : 0;
}
