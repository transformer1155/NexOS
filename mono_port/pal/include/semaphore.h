/* Freestanding shim <semaphore.h> for MiniOS Mono port (PAL).
 *
 * Defining HAVE_SEMAPHORE_H in config.h steers mono/utils/mono-os-semaphore.h
 * onto its POSIX branch; without it the header falls through to the Win32
 * branch and explodes on `HANDLE`.  Phase 0 is single-threaded, so a plain
 * counter is a faithful (never-blocking) semaphore.
 */
#ifndef PAL_SEMAPHORE_H
#define PAL_SEMAPHORE_H

#include <time.h>

typedef struct { volatile int value; } sem_t;

#define SEM_FAILED ((sem_t *)0)

static inline int sem_init (sem_t *s, int pshared, unsigned int value)
	{ (void)pshared; if (s) s->value = (int)value; return 0; }
static inline int sem_destroy (sem_t *s)      { (void)s; return 0; }
static inline int sem_post (sem_t *s)         { if (s) s->value++; return 0; }
static inline int sem_getvalue (sem_t *s, int *v) { if (v) *v = s ? s->value : 0; return 0; }

/* Single-threaded: if the count is zero nobody can ever post, so instead of
 * deadlocking we report EAGAIN/ETIMEDOUT and let the caller decide. */
static inline int sem_trywait (sem_t *s)
	{ if (s && s->value > 0) { s->value--; return 0; } return -1; }
static inline int sem_wait (sem_t *s)
	{ return sem_trywait (s); }
static inline int sem_timedwait (sem_t *s, const struct timespec *abs)
	{ (void)abs; return sem_trywait (s); }

#endif /* PAL_SEMAPHORE_H */
