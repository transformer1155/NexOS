// ai_env.h -- runtime environment detection (VM vs bare metal)
//
// Shared between ai_engine.cpp (implementation) and the 32/64-bit kernels
// (command layer).  Detection is via the CPUID hypervisor bit (leaf 1,
// ECX bit 31) plus the hypervisor vendor signature at leaf 0x40000000.
//
// Design rule for this OS:
//   * Inside a VM (QEMU/KVM/VBox/VMware/...): use the built-in, lightweight
//     Markov engine ("just output something").
//   * On bare metal: enable the real transformer forward-pass inference path.

#ifndef AI_ENV_H
#define AI_ENV_H

#ifdef __cplusplus
extern "C" {
#endif

// 1 if running inside a virtual machine, 0 on bare metal.
int ai_env_is_vm(void);

// Short human-readable description: "KVM", "VirtualBox", "VMware",
// "bare-metal", etc.
const char* ai_env_desc(void);

// 1 when real inference is allowed (bare metal), 0 inside a VM.
int ai_env_real_inference(void);

#ifdef __cplusplus
}
#endif

#endif // AI_ENV_H
