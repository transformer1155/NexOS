/* Freestanding shim <stdlib.h> for MiniOS Mono port (PAL). */
/* NOTE: gcc >= 14 treats an implicit declaration as a hard error, so every
 * symbol glib.h/mono reach for must be declared here even when the compiler
 * ultimately expands it to a builtin (alloca is the usual offender). */
#ifndef PAL_STDLIB_H
#define PAL_STDLIB_H
#include <stddef.h>

#ifndef alloca
#define alloca(sz) __builtin_alloca (sz)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7fffffff

void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);
void   abort(void) __attribute__((noreturn));
void   exit(int status) __attribute__((noreturn));
int    atexit(void (*func)(void));

int       atoi (const char *nptr);
long      atol (const char *nptr);
long long atoll(const char *nptr);
double    atof (const char *nptr);

long               strtol  (const char *nptr, char **endptr, int base);
unsigned long      strtoul (const char *nptr, char **endptr, int base);
long long          strtoll (const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double             strtod  (const char *nptr, char **endptr);
float              strtof  (const char *nptr, char **endptr);

int       abs  (int j);
long      labs (long j);
long long llabs(long long j);

/* console-unix.c splits a millisecond timeout into timeval with div(). */
typedef struct { int       quot; int       rem; } div_t;
typedef struct { long      quot; long      rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

div_t   div  (int       numer, int       denom);
ldiv_t  ldiv (long      numer, long      denom);
lldiv_t lldiv(long long numer, long long denom);

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

int    rand(void);
void   srand(unsigned int seed);

int    posix_memalign(void **memptr, size_t alignment, size_t size);
void  *aligned_alloc(size_t alignment, size_t size);
void  *memalign(size_t alignment, size_t size);
void  *valloc(size_t size);

char  *mktemp(char *tmpl);
int    mkstemp(char *tmpl);
char  *mkdtemp(char *tmpl);

char  *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);
int    system(const char *command);
long   random(void);
void   srandom(unsigned int seed);
void   qsort(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *));

#endif /* PAL_STDLIB_H */
