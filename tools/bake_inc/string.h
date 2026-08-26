/* Minimal freestanding string.h stub for the bake kernel. */
#ifndef BAKE_STRING_H
#define BAKE_STRING_H
#include <stddef.h>
void* memcpy(void* d, const void* s, size_t n);
void* memset(void* d, int v, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
#endif
