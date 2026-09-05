#ifndef DCN_RT_STDLIB_H
#define DCN_RT_STDLIB_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* malloc(size_t size);
void* calloc(size_t n, size_t size);
void  free(void* p);
#ifdef __cplusplus
}
#endif
#endif /* DCN_RT_STDLIB_H */
