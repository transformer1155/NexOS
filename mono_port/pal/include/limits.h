/* Freestanding shim <limits.h> for MiniOS Mono port (PAL). */
#ifndef PAL_LIMITS_H
#define PAL_LIMITS_H

#define CHAR_BIT   8
#define SCHAR_MIN  (-128)
#define SCHAR_MAX  127
#define UCHAR_MAX  255
#define SHRT_MIN   (-32768)
#define SHRT_MAX   32767
#define USHRT_MAX  65535
#define INT_MIN    (-2147483647 - 1)
#define INT_MAX    2147483647
#define UINT_MAX   4294967295U
#define LONG_MIN   (-2147483647L - 1)
#define LONG_MAX   2147483647L
#define ULONG_MAX  4294967295UL
#define LLONG_MIN  (-9223372036854775807LL - 1)
#define LLONG_MAX  9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL

#define CHAR_MIN   SCHAR_MIN
#define CHAR_MAX   SCHAR_MAX

/* POSIX-ish path/name limits. MiniOS SFS is a flat 19-char namespace,
 * but Mono sizes fixed buffers off PATH_MAX, so keep the usual value. */
#define PATH_MAX    4096
#define NAME_MAX    255
#define MAXPATHLEN  PATH_MAX
#define SSIZE_MAX   INT_MAX
#define OPEN_MAX    64
#define NGROUPS_MAX 32

#endif /* PAL_LIMITS_H */
