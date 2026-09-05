#ifndef DCN_RT_STRING_H
#define DCN_RT_STRING_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
char* strncpy(char* dst, const char* src, size_t n);
int   strcmp(const char* a, const char* b);
#ifdef __cplusplus
}
#endif
#endif /* DCN_RT_STRING_H */
