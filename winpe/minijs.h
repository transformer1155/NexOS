/* =============================================================================
 *  minijs.c  -  a tiny integer-only JavaScript subset evaluator for NexOS's
 *  freestanding PE browser (winpe/ntbrowser.c).
 *
 *  WHY INTEGER-ONLY
 *  ----------------
 *  The targets that can run this have no IEEE-754 float path at all: the
 *  MiniCLR stack is 32-bit int (clr.cpp), and the wine/win32 PE browser also
 *  builds -nostdlib freestanding with no libc.  A real JS engine needs
 *  doubles, objects and a GC heap; none of that exists here.  So this is an
 *  honest *subset*: every number is a signed 32-bit int, identifiers map to a
 *  fixed symbol table, there are no objects/closures/arrays, and operators
 *  are the integer ones.  It executes simple scripts like
 *
 *      var x = 6 * 7; if (x == 42) { done = 1; }
 *
 *  and leaves the value of the last expression (or an explicit `return`) in
 *  the "result" slot, which the caller renders.
 *
 *  It is deliberately structurally small: one pass over source -> one-pass
 *  AST-free tree-walk evaluator with a recursive-descent parser.  Everything
 *  is static buffers; ~600 lines.  Buildable for host unit tests with
 *  MINIJS_HOST defined (plain gcc -DMINIJS_HOST), or freestanding for the PE.
 * ============================================================================= */
#ifndef MINIJS_H
#define MINIJS_H

/* Run `src` (NUL-terminated).  Returns 0 on success; stores the integer
   result of the whole script in *out.  On parse/exec error returns a nonzero
   error id and stores a short message in errmsg (if not NULL, cap<=240). */
int minijs_run(const char* src, int* out, char* errmsg, int cap);

#endif
