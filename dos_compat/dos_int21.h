#ifndef DOS_INT21_H
#define DOS_INT21_H

// =====================================================================
//  dos_int21.h  -  MS-DOS INT 21h translation layer (minimal subset)
// ---------------------------------------------------------------------
//  Design: translate DOS function calls into NexOS primitives, NOT a CPU
//  emulator.  Every helper here maps one INT 21h sub-function onto the
//  existing VFS / console layer.
//
//  Convention: every function returns 0 on success, non-zero on failure,
//  which the dispatcher converts into the DOS carry-flag / AX convention.
// =====================================================================

// ---- AH=02h / AH=09h : console output ----
// DOS writes to stdout; these forward to the NexOS console.
int dos_int21_print_char(int ch);
int dos_int21_print_string(const char *s);   // writes until NUL (caller
                                             // passes the DOS '$'-terminated
                                             // string stripped of '$')

// ---- AH=4Ch : terminate program ----
int dos_int21_exit(int code);

// ---- AH=01h / AH=0Ah : buffered keyboard input ----
// Returns the character read (>=0) or -1 when no console input is ready.
int dos_int21_read_char_echo(void);

// ---- AH=3Ch / 3Dh : open / create ----
// `attr` is the DOS attribute byte (0x00 normal, 0x02 hidden, ...).
// Returns a DOS handle (>=0) or a negative DOS error code.
int dos_int21_create_file(const char *name, int attr);
int dos_int21_open_file(const char *name, int mode);

// ---- AH=3Eh : close ----
int dos_int21_close_file(int handle);

// ---- AH=3Fh / 40h : read / write ----
// `bytes_read` / `bytes_written` receive the transferred byte count.
int dos_int21_read_file(int handle, void *buf, int count, int *bytes_read);
int dos_int21_write_file(int handle, const void *buf, int count, int *bytes_written);

#endif // DOS_INT21_H