/* Freestanding shim <dirent.h> for MiniOS Mono port (PAL).
 *
 * MiniOS SFS is a flat namespace with no directory enumeration syscall yet,
 * so opendir() always fails (returns NULL).  Kept as a real header because
 * eglib/mono include it unconditionally in a few places.
 */
#ifndef PAL_DIRENT_H
#define PAL_DIRENT_H

#include <sys/types.h>
#include <limits.h>

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

struct dirent {
    ino_t          d_ino;
    off_t          d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[NAME_MAX + 1];
};

typedef struct PAL_DIR DIR;

DIR           *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int            readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result);
int            closedir(DIR *dirp);
void           rewinddir(DIR *dirp);

#endif /* PAL_DIRENT_H */
