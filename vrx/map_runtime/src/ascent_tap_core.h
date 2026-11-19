/* ascent_tap_core.h — file-descriptor lifecycle tracking for the UART mirror.
 *
 * Factored out of the preload library so it can be tested on the host. The
 * .so around it is thin syscall glue; this is where every decision is made.
 *
 * Fixes from the red-team review:
 *   F-TAP-3 (P1-7) `target_fd` was latched once and never invalidated, because
 *                  close() was not interposed. A recycled descriptor number
 *                  then mirrored unrelated bytes into the CRSF decoder, and a
 *                  reopened UART was missed entirely. The lifecycle is now
 *                  tracked through open/close/dup.
 *   F-TAP-4 (P2)   is_target_fd() ran a readlink("/proc/self/fd/N") on EVERY
 *                  successful read and write until the UART was seen - and
 *                  forever if it never was, which is the case whenever the
 *                  radio is not in BYPASS/ELRS-TT. Each descriptor is now
 *                  classified at most once per lifetime; after that the check
 *                  is a bitmap test.
 *
 * Descriptors at or above A_TAP_FDMAX are never mirrored. That is a deliberate
 * bounded-memory choice, not an oversight: the alternative is an allocation on
 * the stock daemon's hot path.
 */
#ifndef ASCENT_TAP_CORE_H
#define ASCENT_TAP_CORE_H
#include "ascent_types.h"

#define A_TAP_FDMAX   1024
#ifndef A_TAP_TTY_PATH
#define A_TAP_TTY_PATH "/dev/ttyS2"
#endif

typedef struct {
    au8  known[A_TAP_FDMAX / 8];      /* classification already decided */
    au8  target[A_TAP_FDMAX / 8];     /* ...and the answer was yes      */
    au64 opens, closes, dups, probes, probe_hits;
    au64 out_of_range;
} a_tap_fds;

void a_tap_fds_init(a_tap_fds *t);

/* Record the result of an open()/openat(). path may be NULL. */
void a_tap_note_open(a_tap_fds *t, int fd, const char *path);
/* Record a close(). Clears the classification so a recycled number is re-judged. */
void a_tap_note_close(a_tap_fds *t, int fd);
/* Record dup/dup2/dup3: the alias inherits the original's classification. */
void a_tap_note_dup(a_tap_fds *t, int oldfd, int newfd);

/* 1 when fd is a known UART descriptor. */
int  a_tap_is_target(const a_tap_fds *t, int fd);
/* 1 when fd has not been classified yet and a one-shot readlink probe is
 * warranted. Returns 0 for out-of-range descriptors. */
int  a_tap_needs_probe(const a_tap_fds *t, int fd);
/* Record the outcome of that one-shot probe. */
void a_tap_note_probe(a_tap_fds *t, int fd, int is_target);

/* Exact match against A_TAP_TTY_PATH. Exposed for tests. */
int  a_tap_path_is_target(const char *path);

#endif
