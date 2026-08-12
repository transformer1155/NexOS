/* Freestanding shim <sys/param.h> for MiniOS Mono port (PAL). */
#ifndef PAL_SYS_PARAM_H
#define PAL_SYS_PARAM_H

#include <limits.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define BSD 1

#endif /* PAL_SYS_PARAM_H */
