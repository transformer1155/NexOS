/* Freestanding shim <pthread.h> for MiniOS Mono port (PAL).
 *
 * Phase 0 runs the interpreter single-threaded in ring 3, so every lock is a
 * no-op counter and thread creation fails loudly (EAGAIN).  Phase 1 replaces
 * these with real MiniOS futex/scheduler syscalls -- keep the signatures
 * ABI-identical so callers never change.
 *
 * TLS keys are backed by a small fixed table in pal/libc_impl.c: with one
 * thread, "thread-local" and "global" are the same thing.
 */
#ifndef PAL_PTHREAD_H
#define PAL_PTHREAD_H

#include <stddef.h>
#include <time.h>
#include <signal.h>     /* sigset_t for pthread_sigmask */
#include <sched.h>      /* struct sched_param -- metadata/threads.c 直接在栈上放一个 */

typedef struct { int locked; int type; } pthread_mutex_t;
typedef struct { int type; }             pthread_mutexattr_t;
typedef struct { int waiters; }          pthread_cond_t;
typedef struct { int clock; }            pthread_condattr_t;
typedef struct { int done; }             pthread_once_t;
typedef unsigned long                    pthread_t;
typedef struct { size_t stacksize; void *stackaddr; } pthread_attr_t;
typedef unsigned int                     pthread_key_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, 0 }
#define PTHREAD_COND_INITIALIZER  { 0 }
#define PTHREAD_ONCE_INIT         { 0 }

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

#define PTHREAD_PRIO_NONE        0
#define PTHREAD_PRIO_INHERIT     1

#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1

/* ---------------- mutex ---------------- */
static inline int pthread_mutexattr_init (pthread_mutexattr_t *a)
	{ if (a) a->type = PTHREAD_MUTEX_NORMAL; return 0; }
static inline int pthread_mutexattr_destroy (pthread_mutexattr_t *a)
	{ (void)a; return 0; }
static inline int pthread_mutexattr_settype (pthread_mutexattr_t *a, int type)
	{ if (a) a->type = type; return 0; }
static inline int pthread_mutexattr_gettype (pthread_mutexattr_t *a, int *type)
	{ if (type) *type = a ? a->type : 0; return 0; }
static inline int pthread_mutexattr_setprotocol (pthread_mutexattr_t *a, int p)
	{ (void)a; (void)p; return 0; }

static inline int pthread_mutex_init (pthread_mutex_t *m, const pthread_mutexattr_t *a)
	{ if (m) { m->locked = 0; m->type = a ? a->type : 0; } return 0; }
static inline int pthread_mutex_destroy (pthread_mutex_t *m)  { (void)m; return 0; }
static inline int pthread_mutex_lock (pthread_mutex_t *m)     { if (m) m->locked++; return 0; }
static inline int pthread_mutex_trylock (pthread_mutex_t *m)  { if (m) m->locked++; return 0; }
static inline int pthread_mutex_unlock (pthread_mutex_t *m)
	{ if (m && m->locked) m->locked--; return 0; }

/* ---------------- condvar ---------------- */
static inline int pthread_condattr_init (pthread_condattr_t *a)
	{ if (a) a->clock = 0; return 0; }
static inline int pthread_condattr_destroy (pthread_condattr_t *a)  { (void)a; return 0; }
static inline int pthread_condattr_setclock (pthread_condattr_t *a, int clk)
	{ if (a) a->clock = clk; return 0; }

static inline int pthread_cond_init (pthread_cond_t *c, const pthread_condattr_t *a)
	{ (void)a; if (c) c->waiters = 0; return 0; }
static inline int pthread_cond_destroy (pthread_cond_t *c)   { (void)c; return 0; }
static inline int pthread_cond_signal (pthread_cond_t *c)    { (void)c; return 0; }
static inline int pthread_cond_broadcast (pthread_cond_t *c) { (void)c; return 0; }
/* Single-threaded: a wait can never be satisfied, so return immediately
 * rather than hanging the whole process. */
static inline int pthread_cond_wait (pthread_cond_t *c, pthread_mutex_t *m)
	{ (void)c; (void)m; return 0; }
static inline int pthread_cond_timedwait (pthread_cond_t *c, pthread_mutex_t *m,
                                          const struct timespec *abs)
	{ (void)c; (void)m; (void)abs; return 110 /*ETIMEDOUT*/; }

/* ---------------- once ---------------- */
static inline int pthread_once (pthread_once_t *o, void (*fn)(void))
	{ if (o && !o->done) { o->done = 1; if (fn) fn (); } return 0; }

/* ---------------- identity ---------------- */
static inline pthread_t pthread_self (void) { return 1; }
static inline int pthread_equal (pthread_t a, pthread_t b) { return a == b; }

/* ---------------- TLS (table lives in pal/libc_impl.c) ---------------- */
int   pthread_key_create   (pthread_key_t *key, void (*dtor)(void *));
int   pthread_key_delete   (pthread_key_t key);
void *pthread_getspecific  (pthread_key_t key);
int   pthread_setspecific  (pthread_key_t key, const void *value);

/* ---------------- threads (unsupported in Phase 0) ---------------- */
int  pthread_attr_init         (pthread_attr_t *a);
int  pthread_attr_destroy      (pthread_attr_t *a);
int  pthread_attr_setstacksize (pthread_attr_t *a, size_t sz);
int  pthread_attr_getstacksize (const pthread_attr_t *a, size_t *sz);
int  pthread_attr_getstack     (const pthread_attr_t *a, void **addr, size_t *sz);
int  pthread_getattr_np        (pthread_t t, pthread_attr_t *a);
int  pthread_create            (pthread_t *t, const pthread_attr_t *a,
                                void *(*fn)(void *), void *arg);
int  pthread_join              (pthread_t t, void **ret);
int  pthread_detach            (pthread_t t);
void pthread_exit              (void *ret) __attribute__((noreturn));
int  pthread_kill              (pthread_t t, int sig);
int  pthread_sigmask           (int how, const sigset_t *set, sigset_t *oldset);
int  pthread_setname_np        (pthread_t t, const char *name);
int  pthread_getname_np        (pthread_t t, char *buf, size_t len);

/* Thread.Priority maps onto these in metadata/threads.c.  MiniOS has one
 * scheduling class in Phase 0, so they report SCHED_OTHER / priority 0 and
 * accept any set as a no-op rather than failing -- a failure would make
 * Thread.Priority throw. */
int  pthread_getschedparam     (pthread_t t, int *policy, struct sched_param *p);
int  pthread_setschedparam     (pthread_t t, int policy, const struct sched_param *p);

#endif /* PAL_PTHREAD_H */
