/* Minimal <string.h> for freestanding DCN test harness (QEMU/i686-elf). */
#include "string.h"

void* memcpy(void* d, const void* s, size_t n){
  unsigned char* pd = (unsigned char*)d;
  const unsigned char* ps = (const unsigned char*)s;
  for (size_t i = 0; i < n; i++) pd[i] = ps[i];
  return d;
}

void* memset(void* d, int c, size_t n){
  unsigned char* p = (unsigned char*)d;
  unsigned char v = (unsigned char)c;
  for (size_t i = 0; i < n; i++) p[i] = v;
  return d;
}

void* memmove(void* d, const void* s, size_t n){
  unsigned char* pd = (unsigned char*)d;
  const unsigned char* ps = (const unsigned char*)s;
  if (pd < ps){
    for (size_t i = 0; i < n; i++) pd[i] = ps[i];
  } else {
    for (size_t i = n; i > 0; i--) pd[i-1] = ps[i-1];
  }
  return d;
}

int memcmp(const void* a, const void* b, size_t n){
  const unsigned char* pa = (const unsigned char*)a;
  const unsigned char* pb = (const unsigned char*)b;
  for (size_t i = 0; i < n; i++){
    if (pa[i] != pb[i]) return (pa[i] < pb[i]) ? -1 : 1;
  }
  return 0;
}

size_t strlen(const char* s){
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

char* strncpy(char* d, const char* s, size_t n){
  size_t i = 0;
  for (; i < n && s[i]; i++) d[i] = s[i];
  for (; i < n; i++) d[i] = 0;
  return d;
}

int strcmp(const char* a, const char* b){
  while (*a && (*a == *b)) { a++; b++; }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
