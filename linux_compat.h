#ifndef NexOS_LINUX_COMPAT_H
#define NexOS_LINUX_COMPAT_H
// Linux binary-compatability layer (Milestone 0 of Wine-on-NexOS).
//
// Wine is a user-mode Win32 -> host-syscall translator. To run Wine on NexOS
// we must provide the host ABI its Linux backend expects: an `int 0x80`
// syscall dispatcher + an ELF loader. This file is the kernel-side shim.
//
// Milestone 0 runs the guest ELF in RING 0 (no privilege isolation yet) so it
// can be validated end-to-end without a TSS / ring-3 transition. Per-process
// virtual memory + real ring-3 isolation are later milestones.
#ifdef __cplusplus
extern "C" {
#endif

// Register a file reader (SFS/disk) so the ELF loader can fetch binaries.
void linux_compat_init(int (*reader)(const char*, unsigned char*, int));

// Load and execute a Linux ELF32 image by name (from the registered reader).
// argv/argc are copied onto the guest stack (Linux i386 ABI: [esp]=argc,
// [esp+4..]=argv pointers, NULL-terminated, then envp NULL).
int  linux_run(const char* name, int argc, const char** argv);

// int 0x80 trap entry — wired into the IDT by kernel.cpp.
void linux_syscall_entry(void);

#ifdef __cplusplus
}
#endif
#endif
