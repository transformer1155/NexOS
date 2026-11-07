#ifndef DOS_PRIM_H
#define DOS_PRIM_H

// =====================================================================
//  dos_prim.h  -  Low-level primitives the INT 21h translation layer
//                 needs but that NexOS does not already expose.
// ---------------------------------------------------------------------
//  Why this file exists:
//    dos_int21.c is generated function-by-function by micro-task-pipeline
//    from a minimal context.  Those functions must be able to COMPILE and
//    RUN on their own; otherwise the generation loop has no feedback.
//    These primitives give the generated code a real, stable API.
//
//  Two build modes:
//    * kernel (default)      - dos_prim_kernel.cpp, real NexOS services
//    * host  (-DDOS_HOST)    - dos_prim_host.c, stdout + RAM-backed VFS
//  Both implement exactly this header.
// =====================================================================

// ---- console output ----
// Returns 0 on success, non-zero on failure.
int dos_console_putc(int ch);
int dos_console_puts(const char *s);

// ---- GUI console sink (kernel only) ----
// The DOS primitives write to COM1 by default, which is invisible in the
// VBox GUI unless a serial port is wired up.  The kernel installs a sink
// here so DOS output ALSO reaches the on-screen terminal:
//
//     dos_console_set_sink( [](const char* s){ term.write(s); } );
//
// The sink receives printable text and '\n'; the primitive still drives the
// serial port itself, so output appears in both places.  Passing 0 restores
// serial-only behaviour (used by the host build and by tests).
typedef void (*dos_console_sink_fn)(const char *s);
void dos_console_set_sink(dos_console_sink_fn fn);

// ---- console input ----
// Returns the character read (0..255), or -1 when no input is pending.
// DOS AH=01h has no "nothing to read" concept, so -1 is the sentinel the
// translation layer maps onto "no character available".
int dos_console_getc(void);

// ---- process termination ----
// Terminates the current DOS/ring-3 program with `code`.
//
// KERNEL behaviour: does NOT return.  It records the exit code and unwinds
// straight back into enter_user() through the same mini_longjmp context
// that SYS_EXIT uses, so a .COM program's final RET / INT 21h AH=4Ch both
// land back in the kernel cleanly.
//
// HOST behaviour: records the code and returns 0 so tests can assert on it.
int dos_process_exit(int code);

// Last exit code recorded by dos_process_exit(), for testing.
int dos_process_exit_code(void);

// ---- writable-medium support -------------------------------------------------
// NexOS overlays a writable layer (RAMFS) on the read-only SFS boot medium.
// vfs_create() establishes ownership there; SFS bytes stay protected.
// Declared in vfs.h for the kernel build; mirrored here so the host build
// links against one stable contract.
#ifndef NEXOS_VFS_H
int vfs_create(const char *path, int flags);
int vfs_is_writable(const char *path);
#endif

#endif // DOS_PRIM_H