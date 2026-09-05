#ifndef DCN_RT_STDIO_H
#define DCN_RT_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
int printf(const char* fmt, ...);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int sscanf(const char* str, const char* fmt, ...);
#ifdef __cplusplus
}
#endif
#endif /* DCN_RT_STDIO_H */
