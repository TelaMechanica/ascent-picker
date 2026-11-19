#include "ascent_tap_core.h"

static int in_range(int fd){ return fd >= 0 && fd < A_TAP_FDMAX; }

/* The bitmap lives inside the stock daemon and is touched from whichever
 * thread happens to perform the I/O. Descriptors 0..7 share a byte, so a
 * plain read-modify-write could lose another thread's bit and misclassify a
 * descriptor. These are single instructions on AArch64 and cost nothing on
 * the hot path, which is the only path that matters here. */
static void bset(au8 *m, int fd){ __atomic_fetch_or (&m[fd >> 3], (au8)(1u << (fd & 7)), __ATOMIC_RELAXED); }
static void bclr(au8 *m, int fd){ __atomic_fetch_and(&m[fd >> 3], (au8)~(1u << (fd & 7)), __ATOMIC_RELAXED); }
static int  bget(const au8 *m, int fd){
    au8 v = __atomic_load_n(&m[fd >> 3], __ATOMIC_RELAXED);
    return (v >> (fd & 7)) & 1;
}

void a_tap_fds_init(a_tap_fds *t){ a_memset(t, 0, sizeof(*t)); }

int a_tap_path_is_target(const char *path)
{
    return path && a_streq(path, A_TAP_TTY_PATH);
}

void a_tap_note_open(a_tap_fds *t, int fd, const char *path)
{
    if (!in_range(fd)) { if (fd >= 0) t->out_of_range++; return; }
    t->opens++;
    bset(t->known, fd);
    if (a_tap_path_is_target(path)) bset(t->target, fd);
    else                            bclr(t->target, fd);
}

void a_tap_note_close(a_tap_fds *t, int fd)
{
    if (!in_range(fd)) return;
    t->closes++;
    bclr(t->known, fd);
    bclr(t->target, fd);
}

void a_tap_note_dup(a_tap_fds *t, int oldfd, int newfd)
{
    if (!in_range(newfd)) { if (newfd >= 0) t->out_of_range++; return; }
    t->dups++;
    /* dup2/dup3 silently close newfd first; the alias then takes the
     * original's identity. An unknown original yields an unknown alias, which
     * will be probed once if it is ever used. */
    if (in_range(oldfd) && bget(t->known, oldfd)) {
        bset(t->known, newfd);
        if (bget(t->target, oldfd)) bset(t->target, newfd);
        else                        bclr(t->target, newfd);
    } else {
        bclr(t->known, newfd);
        bclr(t->target, newfd);
    }
}

int a_tap_is_target(const a_tap_fds *t, int fd)
{
    if (!in_range(fd)) return 0;
    return bget(t->known, fd) && bget(t->target, fd);
}

int a_tap_needs_probe(const a_tap_fds *t, int fd)
{
    if (!in_range(fd)) return 0;
    return !bget(t->known, fd);
}

void a_tap_note_probe(a_tap_fds *t, int fd, int is_target)
{
    if (!in_range(fd)) return;
    t->probes++;
    bset(t->known, fd);
    if (is_target) { bset(t->target, fd); t->probe_hits++; }
    else             bclr(t->target, fd);
}
