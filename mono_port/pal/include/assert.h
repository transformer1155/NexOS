/* Freestanding shim <assert.h> for MiniOS Mono port (PAL).
 *
 * Routed through pal_assert_fail() so a failed assertion prints on the ring-3
 * console (serial) and then terminates the process, instead of trapping into
 * glibc's __assert_fail which does not exist here.
 */
#ifndef PAL_ASSERT_H
#define PAL_ASSERT_H

void pal_assert_fail (const char *expr, const char *file, int line,
                      const char *func) __attribute__((noreturn));

#ifdef NDEBUG
#  define assert(e) ((void)0)
#else
#  define assert(e) \
     ((e) ? (void)0 : pal_assert_fail (#e, __FILE__, __LINE__, __func__))
#endif

/* C11 static assertion, used by a few Mono headers. */
#ifndef static_assert
#  define static_assert _Static_assert
#endif

#endif /* PAL_ASSERT_H */
