/* Freestanding shim <ctype.h> for MiniOS Mono port (PAL). */
#ifndef PAL_CTYPE_H
#define PAL_CTYPE_H

static inline int isalpha(int c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z'); }
static inline int isdigit(int c){ return c>='0'&&c<='9'; }
static inline int isalnum(int c){ return isalpha(c)||isdigit(c); }
static inline int isspace(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static inline int isxdigit(int c){ return isdigit(c)||(c>='A'&&c<='F')||(c>='a'&&c<='f'); }
static inline int isprint(int c){ return c>=32&&c<127; }
static inline int iscntrl(int c){ return (c>=0&&c<32)||c==127; }
/* ASCII punctuation ranges are inclusive: 33-47, 58-64, 91-96, 123-126. */
static inline int ispunct(int c){ return (c>=33&&c<=47)||(c>=58&&c<=64)||(c>=91&&c<=96)||(c>=123&&c<=126); }
static inline int islower(int c){ return c>='a'&&c<='z'; }
static inline int isupper(int c){ return c>='A'&&c<='Z'; }
static inline int isgraph(int c){ return c>=33&&c<127; }
static inline int tolower(int c){ return (c>='A'&&c<='Z')?c-'A'+'a':c; }
static inline int toupper(int c){ return (c>='a'&&c<='z')?c-'a'+'A':c; }
/* isascii/toascii 是 XSI 扩展而非 ISO C，但 metadata/icall.c 直接用了 isascii。 */
static inline int isascii(int c){ return (unsigned)c < 128u; }
static inline int toascii(int c){ return c & 0x7F; }
static inline int isblank(int c){ return c==' '||c=='\t'; }

#endif /* PAL_CTYPE_H */
