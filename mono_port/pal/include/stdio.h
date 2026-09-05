/* Freestanding shim <stdio.h> for MiniOS Mono port (PAL). */
#ifndef PAL_STDIO_H
#define PAL_STDIO_H
#include <stddef.h>
#include <stdarg.h>

/* POSIX-ish scalar types Mono expects to be visible once <stdio.h> is in.
   Guarded so a later <sys/types.h> shim can define the same names. */
#ifndef PAL_HAVE_POSIX_TYPES
#define PAL_HAVE_POSIX_TYPES 1
typedef long          off_t;
typedef int           ssize_t;
typedef int           pid_t;
typedef unsigned int  mode_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ   1024

/* eglib/mono refer to the stream type as FILE, keep PAL_FILE as an alias. */
typedef struct PAL_FILE FILE;
typedef FILE PAL_FILE;
#define NULL_FILE ((FILE*)0)
extern FILE *pal_stdout;
extern FILE *pal_stderr;
extern FILE *pal_stdin;
#define stdout pal_stdout
#define stderr pal_stderr
#define stdin  pal_stdin
#define EOF (-1)

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define FILENAME_MAX 260
#define L_tmpnam     260

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int snprintf(char *str, size_t size, const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
int vsprintf(char *str, const char *fmt, va_list ap);
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int fprintf(PAL_FILE *stream, const char *fmt, ...);
int vfprintf(PAL_FILE *stream, const char *fmt, va_list ap);
int fputs(const char *s, PAL_FILE *stream);
int puts(const char *s);
int fputc(int c, PAL_FILE *stream);
int putc(int c, PAL_FILE *stream);
int putchar(int c);
int fflush(PAL_FILE *stream);

/* ---- stream layer -------------------------------------------------
 * MiniOS has no host filesystem behind these; fopen() currently fails
 * (returns NULL) unless pal_fopen_hook is installed by the embedder,
 * which is how SFS-backed IO will be wired in P1.
 */
typedef PAL_FILE *(*pal_fopen_fn)(const char *path, const char *mode);
extern pal_fopen_fn pal_fopen_hook;

PAL_FILE *fopen  (const char *path, const char *mode);
PAL_FILE *fdopen (int fd, const char *mode);
PAL_FILE *freopen(const char *path, const char *mode, PAL_FILE *stream);
int    fclose (PAL_FILE *stream);
size_t fread  (void *ptr, size_t size, size_t nmemb, PAL_FILE *stream);
size_t fwrite (const void *ptr, size_t size, size_t nmemb, PAL_FILE *stream);
char  *fgets  (char *s, int size, PAL_FILE *stream);
int    fgetc  (PAL_FILE *stream);
int    getc   (PAL_FILE *stream);
int    ungetc (int c, PAL_FILE *stream);
int    feof   (PAL_FILE *stream);
int    ferror (PAL_FILE *stream);
void   clearerr(PAL_FILE *stream);
int    fseek  (PAL_FILE *stream, long offset, int whence);
long   ftell  (PAL_FILE *stream);
void   rewind (PAL_FILE *stream);
int    setvbuf(PAL_FILE *stream, char *buf, int mode, size_t size);
void   setbuf (PAL_FILE *stream, char *buf);
int    fileno (PAL_FILE *stream);
int    remove (const char *path);
int    rename (const char *old_, const char *new_);
void   perror (const char *s);
int    sscanf (const char *str, const char *fmt, ...);
int    fscanf (PAL_FILE *stream, const char *fmt, ...);

#endif /* PAL_STDIO_H */
