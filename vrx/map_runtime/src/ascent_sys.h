/* ascent_sys.h — the only place the OS is named.
 *
 * ASCENT_HOSTED  : host builds (tests, sanitizers) use the real libc headers.
 * otherwise      : AArch64 -nostdlib -ffreestanding builds declare what they
 *                  need, exactly as the original POC did, so the shipped
 *                  binaries stay independent of the cross-toolchain's crt and
 *                  glibc version. That property is hardware-proven (the
 *                  aca01d4b engine ran on the VRX) and is deliberately kept.
 */
#ifndef ASCENT_SYS_H
#define ASCENT_SYS_H
#include "ascent_types.h"

#ifdef ASCENT_HOSTED
#  include <unistd.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/mman.h>
#  include <stdlib.h>
#  include <time.h>
#  include <signal.h>
#  include <setjmp.h>
#  include <errno.h>
#  include <stdio.h>   /* rename() */
#  include <dlfcn.h>
#else
   struct pollfd { int fd; short events; short revents; };
   extern int poll(struct pollfd*, unsigned long, int);
   extern int getsockopt(int, int, int, void*, unsigned int*);
   struct timeval { as64 tv_sec; as64 tv_usec; };
   struct timespec { as64 tv_sec; as64 tv_nsec; };
   extern int      open(const char*, int, ...);
   extern int      close(int);
   extern assize   read(int, void*, ausize);
   extern assize   write(int, const void*, ausize);
   extern as64     lseek(int, as64, int);
   extern int      ftruncate(int, as64);
   extern int      rename(const char*, const char*);
   extern int      unlink(const char*);
   extern void    *mmap(void*, ausize, int, int, int, as64);
   extern int      munmap(void*, ausize);
   extern void    *malloc(ausize);
   extern void     free(void*);
   extern int      nanosleep(const struct timespec*, struct timespec*);
   extern int      clock_gettime(int, struct timespec*);
   extern int      socket(int, int, int);
   extern int      bind(int, const void*, unsigned int);
   extern int      setsockopt(int, int, int, const void*, unsigned int);
   extern assize   recvfrom(int, void*, ausize, int, void*, unsigned int*);
   /* TCP (loopback only) for the Picker runtime: 50000 RPC + 2767 standby + the IPC server. */
   extern int      connect(int, const void*, unsigned int);
   extern int      listen(int, int);
   extern int      accept(int, void*, unsigned int*);
   extern assize   send(int, const void*, ausize, int);
   extern assize   recv(int, void*, ausize, int);
   extern void    *dlopen(const char*, int);
   extern void    *dlsym(void*, const char*);
   extern char    *getenv(const char*);
   extern int      getpid(void);
   extern void     _exit(int) __attribute__((noreturn));
#  define O_RDONLY   0
#  define O_WRONLY   1
#  define O_RDWR     2
#  define O_CREAT    0100
#  define O_EXCL     0200
#  define O_TRUNC    01000
#  define O_APPEND   02000
#  define O_CLOEXEC  02000000
#  define SEEK_SET   0
#  define SEEK_END   2
#  define PROT_READ  1
#  define PROT_WRITE 2
#  define MAP_SHARED 1
#  define MAP_FAILED ((void*)-1)
#  define CLOCK_MONOTONIC 1
#  define AF_INET    2
#  define SOCK_DGRAM 2
#  define SOCK_STREAM 1
#  define SOCK_NONBLOCK 04000
#  define SOL_SOCKET        1
#  define SO_REUSEADDR      2
#  define SO_ERROR          4
#  define POLLIN            1
#  define POLLOUT           4
#  define SO_RCVTIMEO 20
#  define SO_SNDTIMEO 21
#  define MSG_NOSIGNAL 0x4000
#  define IPPROTO_IP        0
/* Multicast membership is outside this candidate runtime. */
#  define MSG_DONTWAIT 0x40
#  define RTLD_NOW    2
#  define RTLD_GLOBAL 0x100
#endif

/* Monotonic milliseconds. CLOCK_MONOTONIC is system-wide on Linux, so the
 * producer's and the consumer's readings share an epoch and are directly
 * comparable across processes. The frame staleness check depends on that. */
static inline au64 a_now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (au64)t.tv_sec * 1000ull + (au64)(t.tv_nsec / 1000000);
}

/* Monotonic microseconds, for the render-path timings the bench reads out of
 * the STAT block. Same clock as a_now_ms(), so the two are consistent. */
static inline au64 a_now_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (au64)t.tv_sec * 1000000ull + (au64)(t.tv_nsec / 1000);
}

static inline void a_sleep_ms(as64 ms)
{
    struct timespec t;
    t.tv_sec  = (as64)(ms / 1000);
    t.tv_nsec = (as64)((ms % 1000) * 1000000);
    nanosleep(&t, ASCENT_NULL);
}

/* Full read/write that tolerates short transfers. Returns bytes moved. */
static inline assize a_read_full(int fd, void *buf, ausize n)
{
    au8 *p = (au8*)buf; ausize got = 0;
    while (got < n) {
        assize r = read(fd, p + got, n - got);
        if (r <= 0) break;
        got += (ausize)r;
    }
    return (assize)got;
}

#endif
