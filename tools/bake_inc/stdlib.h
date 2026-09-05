/* Minimal freestanding stdlib.h stub for the bake kernel (stb_truetype needs
 * malloc/free; we override stb's allocators, so these are just declarations). */
#ifndef BAKE_STDLIB_H
#define BAKE_STDLIB_H
#include <stddef.h>
void* malloc(size_t n);
void  free(void* p);
void  abort(void);
int   abs(int x);
long  labs(long x);
#endif
