/* Minimal eglib-config.h for MiniOS freestanding 32-bit port (PAL). */
#ifndef EGLIB_CONFIG_H
#define EGLIB_CONFIG_H

/* Platform: little-endian, unix-ish (so -unix eglib files would compile,
   though we only compile the platform-generic core subset). */
#define G_OS_UNIX 1
#define G_LITTLE_ENDIAN 1234
#define G_BIG_ENDIAN    4321
#define G_BYTE_ORDER    G_LITTLE_ENDIAN

#define G_GNUC_PRETTY_FUNCTION  __func__
#define G_GNUC_UNUSED           __attribute__((unused))
#define G_GNUC_NORETURN         __attribute__((noreturn))

#define G_SEARCHPATH_SEPARATOR_S ":"
#define G_SEARCHPATH_SEPARATOR   ':'
#define G_DIR_SEPARATOR_S        "/"
#define G_DIR_SEPARATOR          '/'
#define G_BREAKPOINT()           do { } while (0)

/* Basic scalar typedefs (eglib expects these in addition to stdint). */
typedef unsigned int  gsize;
typedef int           gssize;
typedef unsigned int  GPid;
#define G_GSIZE_FORMAT "u"

/* PAL supplies a shim <unistd.h> (read/write/close/...), so let glib.h
   pull it in -- otherwise g_read/g_write expand to undeclared symbols. */
#define G_HAVE_UNISTD_H 1
#undef  HAVE_ALLOC_H
#undef  HAVE_ALLOCA_H

/* NOTE: do NOT define EGLIB_NO_REMAP.
   glib.h unconditionally rewrites the allocator call sites to
   monoeg_malloc / monoeg_realloc / monoeg_malloc0 / ... (glib.h ~1475-1488),
   while the *definitions* in gmem.c are only renamed to those symbols by
   eglib-remap.h.  Suppressing the remap therefore leaves every allocator
   call unresolved (implicit-int) -- exactly the g_realloc->int error.
   Upstream always builds with the remap enabled; so do we. */

#endif /* EGLIB_CONFIG_H */
