#ifndef DOS_EXEC_H
#define DOS_EXEC_H

#include <stdint.h>

// Load and execute a .COM program from the NexOS VFS.
// 
// The .COM file is loaded at USER_BASE + 0x100 (segment-aligned in our mapping).
// PSP is built at USER_BASE.
// 
// Critical detail (as requested): the stack is set up so that the program's
// first RET will go to a DOS exit stub, and INT 21h AH=4Ch will also call
// dos_process_exit(). Both paths converge on the same termination logic
// that unwinds through enter_user() via sys_exit_current().
// 
// Returns 0 if started, -1 on error. When the program exits, control
// returns to the caller of enter_user() (like cmd_user does today).
int dos_exec_com(const char *name);

#endif // DOS_EXEC_H