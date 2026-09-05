/* Freestanding shim <inttypes.h> for MiniOS Mono port (PAL). */
#ifndef PAL_INTTYPES_H
#define PAL_INTTYPES_H
#include <stdint.h>

#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "lld"
#define PRIi8   "i"
#define PRIi16  "i"
#define PRIi32  "i"
#define PRIi64  "lli"
#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  "llu"
#define PRIx32  "x"
#define PRIx64  "llx"

#endif /* PAL_INTTYPES_H */
