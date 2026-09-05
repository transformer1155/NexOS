/* Minimal <stdio.h>: vsnprintf + printf (to COM1). */
#include "stdio.h"
#include "serial.h"

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap){
  struct Sink {
    char* b; size_t sz; size_t n;
    void put(char c){ if (sz > 0 && n < sz - 1) b[n] = c; if (n < sz) n++; }
    void puts(const char* s, int len){ for (int i=0;i<len;i++) put(s[i]); }
  } s;
  s.b = buf; s.sz = size; s.n = 0;

  static const char* HX = "0123456789abcdef0123456789ABCDEF";
  int zero = 0, left = 0, width = 0;

  /* emit a finished string with width/flag handling */
  auto emit = [&](const char* str, int len){
    int pad = width - len; if (pad < 0) pad = 0;
    if (!left){ for (int i=0;i<pad;i++) s.put(zero ? '0' : ' '); }
    s.puts(str, len);
    if (left){ for (int i=0;i<pad;i++) s.put(' '); }
  };

  for (const char* p = fmt; *p; ){
    if (*p != '%'){ s.put(*p); p++; continue; }
    p++;
    if (*p == '%'){ s.put('%'); p++; continue; }
    zero = 0; left = 0; width = 0;
    while (*p == '-' || *p == '0'){ if (*p=='-') left = 1; else zero = 1; p++; }
    while (*p >= '0' && *p <= '9'){ width = width*10 + (*p - '0'); p++; }
    int lng = 0;
    while (*p == 'l' || *p == 'L'){ lng = 1; p++; }
    char spec = *p++;
    switch (spec){
      case 's': {
        const char* t = va_arg(ap, const char*);
        if (!t) t = "(null)";
        int tl = 0; while (t[tl]) tl++;
        emit(t, tl);
      } break;
      case 'c': {
        char tmp[1]; tmp[0] = (char)va_arg(ap, int);
        emit(tmp, 1);
      } break;
      case 'd': case 'i': {
        long v = lng ? va_arg(ap, long) : va_arg(ap, int);
        char d[24]; int di = sizeof(d); int neg = 0; unsigned long uv;
        if (v < 0){ neg = 1; uv = (unsigned long)(-(v + 1)) + 1UL; }
        else uv = (unsigned long)v;
        if (uv == 0) d[--di] = '0';
        while (uv){ d[--di] = (char)('0' + (uv % 10)); uv /= 10; }
        if (neg) d[--di] = '-';
        emit(d + di, (int)(sizeof(d) - di));
      } break;
      case 'u': {
        unsigned long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
        char d[24]; int di = sizeof(d);
        if (v == 0) d[--di] = '0';
        while (v){ d[--di] = (char)('0' + (v % 10)); v /= 10; }
        emit(d + di, (int)(sizeof(d) - di));
      } break;
      case 'x': case 'X': {
        unsigned long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
        const char* H = (spec == 'X') ? (HX + 16) : HX;
        char d[24]; int di = sizeof(d);
        if (v == 0) d[--di] = '0';
        while (v){ d[--di] = H[v & 0xF]; v >>= 4; }
        emit(d + di, (int)(sizeof(d) - di));
      } break;
      case 'p': {
        unsigned long v = (unsigned long)va_arg(ap, void*);
        char d[24]; int di = sizeof(d);
        while (v){ d[--di] = HX[v & 0xF]; v >>= 4; }
        emit(d + di, (int)(sizeof(d) - di));
      } break;
      default: s.put('?'); break;
    }
  }
  if (size > 0){
    size_t term = (s.n < size) ? s.n : (size - 1);
    buf[term] = 0;
  }
  return (int)s.n;
}

int snprintf(char* buf, size_t size, const char* fmt, ...){
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

int printf(const char* fmt, ...){
  char buf[512];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  for (int i = 0; i < n && i < (int)sizeof(buf) && buf[i]; i++) serial_putc(buf[i]);
  return n;
}

static int is_space(char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'; }

int sscanf(const char* str, const char* fmt, ...){
  va_list ap; va_start(ap, fmt);
  int assigned = 0;
  const char* s = str;
  for (const char* p = fmt; *p; ){
    if (is_space(*p)){ while (is_space(*s)) s++; p++; continue; }
    if (*p != '%'){
      if (*s != *p) break;
      s++; p++;
      continue;
    }
    p++;
    if (*p == '%'){ if (*s == '%'){ s++; p++; } else break; continue; }
    int width = 0;
    while (*p >= '0' && *p <= '9'){ width = width*10 + (*p - '0'); p++; }
    if (*p == 'l' || *p == 'L') p++;
    char spec = *p++;
    if (spec != 'c'){ while (is_space(*s)) s++; }
    switch (spec){
      case 'd': case 'i': {
        if (*s != '-' && *s != '+' && (*s < '0' || *s > '9')) break;
        int sign = 1; long v = 0;
        if (*s == '-'){ sign = -1; s++; } else if (*s == '+'){ s++; }
        if (*s < '0' || *s > '9') break;
        while (*s >= '0' && *s <= '9'){ v = v*10 + (*s - '0'); s++; }
        *(va_arg(ap, int*)) = (int)(sign * v);
        assigned++;
      } break;
      case 'u': {
        if (*s < '0' || *s > '9') break;
        unsigned long v = 0;
        while (*s >= '0' && *s <= '9'){ v = v*10 + (*s - '0'); s++; }
        *(va_arg(ap, unsigned int*)) = (unsigned int)v;
        assigned++;
      } break;
      case 'x': case 'X': {
        if (*s < '0' || *s > '9') break;
        unsigned long v = 0;
        while (1){
          char c = *s; int d;
          if (c>='0'&&c<='9') d=c-'0';
          else if (c>='a'&&c<='f') d=c-'a'+10;
          else if (c>='A'&&c<='F') d=c-'A'+10;
          else break;
          v = v*16 + (unsigned long)d; s++;
        }
        *(va_arg(ap, unsigned int*)) = (unsigned int)v;
        assigned++;
      } break;
      case 's': {
        char* out = va_arg(ap, char*);
        int n = 0;
        int max = width ? width : 0x7fffffff;
        while (*s && !is_space(*s)){
          if (n >= max) break;
          out[n++] = *s++;
        }
        if (n == 0) break;
        out[n] = 0;
        assigned++;
      } break;
      case 'c': {
        char* out = va_arg(ap, char*);
        int n = 0;
        int max = width ? width : 1;
        while (*s && n < max){ out[n++] = *s++; }
        if (n == 0) break;
        assigned++;
      } break;
      default: break;
    }
  }
  va_end(ap);
  return assigned;
}
