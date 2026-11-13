#ifndef NexOS_CLR_H
#define NexOS_CLR_H
// =====================================================================
//  clr.h  -  MiniCLR: a CIL interpreter for NexOS
// ---------------------------------------------------------------------
//  Managed apps are written in real C#, compiled by real Roslyn, then
//  flattened by tools/mex_pack.py into a .mex image whose metadata
//  tokens have already been resolved into direct indices.  This module
//  loads that image and interprets the IL.
// =====================================================================
#include <stdint.h>

extern "C" {

// reader(name, buf, bufsize) -> bytes read, or <0 on failure
typedef int (*clr_read_fn)(const char* name, unsigned char* buf, int bufsize);

void clr_init(clr_read_fn reader);

// Load and run a .mex image.  Returns 0 on success, negative on error.
//   -1 file not found     -2 bad image      -3 out of memory
//   -4 unbound internal call               -5 execution fault
int  clr_run(const char* filename);

// Load a .mex as the RESIDENT image and run its entry point (Program::Main).
// Unlike clr_run() (throwaway + return), the assembly stays the persistent
// managed context so the native GUI loop keeps painting/inputting any windows
// its Main opens.  Returns the same codes as clr_run().
int  clr_run_resident(const char* filename);

// Human readable description of the last load/run.
const char* clr_last_report(void);

// True once clr_init() has been called.
int  clr_ready(void);

// ---------------------------------------------------------------------
//  Resident hosting  (used by the managed GUI shell)
// ---------------------------------------------------------------------
//  clr_run() is fire-and-forget: load, run Main, done.  The GUI needs the
//  opposite -- one assembly stays loaded for the lifetime of the desktop
//  and the kernel calls into it many times per second.
//
//  Because the managed heap is a bump allocator with no GC, a repaint that
//  allocates would leak until exhaustion.  clr_heap_mark()/clr_heap_reset()
//  give the host a stack discipline: take a mark once the app's persistent
//  state is built, then rewind to it at the top of every frame.
// ---------------------------------------------------------------------

// Load an image and keep it resident. Returns 0 on success.
int  clr_load(const char* filename);

// True when an image is currently loaded and fault-free.
int  clr_loaded(void);

// Call a static method by fully-qualified name ("Type::Method").
// Returns 0 on success and stores the return value (if any) in *ret.
//   -1 method not found     -5 execution fault
int  clr_call(const char* fqname, const int32_t* args, int nargs, int32_t* ret);

// Managed heap watermark helpers (see note above).
uint32_t clr_heap_mark(void);
void     clr_heap_reset(uint32_t mark);

// Bytes currently allocated on the managed heap.
uint32_t clr_heap_used(void);

// Unload the resident image (frees nothing; just detaches it).
void clr_unload(void);

// ---------------------------------------------------------------------
//  Extensible internal calls
// ---------------------------------------------------------------------
//  clr.cpp ships a handful of built-in [MethodImpl(InternalCall)] targets
//  (NexOS.Sys::*).  Subsystems layered on top -- the managed GUI shell,
//  for one -- register theirs here rather than forcing clr.cpp to depend
//  on them.  Register before clr_load(); binding happens at load time.
typedef int32_t (*clr_icall_fn)(int32_t* args);
int  clr_register_icall(const char* fqname, clr_icall_fn fn);

// ---------------------------------------------------------------------
//  Managed object accessors (for internal call implementations)
// ---------------------------------------------------------------------
// Borrow the UTF-8 bytes of a managed string.  Returns "" for null.
const char* clr_str(int32_t obj);

// Allocate a managed string from a C string.  0 on failure.
int32_t clr_new_str(const char* s);

// Managed int[] accessors (OS API L3+): header is
// [0]=OBJ_ARRAY [4]=count [8]=elemSize [12..]=int32 data.
// 0 / -1 for null arrays.
int     clr_arr_len(int32_t obj);
int32_t* clr_arr_data(int32_t obj);

// ---------------------------------------------------------------------
//  Instance slots -- several .mex assemblies resident at once
// ---------------------------------------------------------------------
// The CLR used to hold exactly ONE image, so an "application" could only be a
// stub that asked the desktop shell to open its window.  A slot now holds an
// independent assembly with its own image, managed heap, stack and static
// fields; slot 0 is the desktop shell.
//
// Every call below switches the interpreter's CURRENT slot, so a subsystem
// that interleaves work for several owners must save and restore it with
// clr_current_slot() / clr_use().
// Shared capacity: one slot per independently resident application image.
// app_registry.h and the GUI window table derive their limits from this value.
// Budget check: each loaded slot costs ~840 KiB (image + heap + ClrVm tables).
// The 32-bit kernel has a 128 MiB heap; the 64-bit one only 16 MiB, so do not
// raise this without re-checking that budget.
#define CLR_MAX_VMS 12          // slot 0 = shell, 1..11 = independent apps
#define CLR_SLOT_SHELL 0
int clr_load_slot(int slot, const char* mex);   // <0 on failure
int clr_use(int slot);                          // returns the previous slot, or -1
int clr_current_slot(void);
int clr_close_slot(int slot);                   // slot 0 is refused
int clr_slot_loaded(int slot);                  // 1 when the slot holds a live image

} // extern "C"

#endif // NexOS_CLR_H
