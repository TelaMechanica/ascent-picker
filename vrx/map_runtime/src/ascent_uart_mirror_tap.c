/*
 * ascent_uart_mirror_tap.c — AArch64 LD_PRELOAD UART mirror for ar_ldy_gnd.
 *
 * ARCHITECTURE RETAINED DELIBERATELY. This library still never opens, reads,
 * writes, configures or owns /dev/ttyS2. It mirrors bytes the stock process
 * has ALREADY successfully transferred. The red-team review verified that
 * property at binary level and it is preserved: /dev/ttyS2 appears here only
 * as a string compared against readlink() and open() paths.
 *
 * WHAT CHANGED (red-team findings)
 *   F-TAP-2 (P1-1) the 4 MiB capture cap silently ended the live feed after
 *                  91 s to 15 min. The LIVE path is now a fixed-size shared
 *                  memory ring, ONE PER DIRECTION so the two byte streams are
 *                  never interleaved. Disk capture is DIAGNOSTIC ONLY (TX),
 *                  off unless ASCENT_TAP_CAPTURE=1, and separately bounded.
 *   F-TAP-3 (P1-7) close/open/openat/dup are interposed, so a recycled
 *                  descriptor can no longer mirror foreign bytes.
 *   F-TAP-4 (P2)   a descriptor is classified at most once per lifetime, not
 *                  a readlink on every successful read and write forever.
 *   F-TAP-5 (P2)   capture files are O_APPEND and pid-suffixed.
 *   F-TAP-6 (P2)   the interposers never return -1 of their own accord; if
 *                  dlsym is unresolved or fails, the call falls through to a
 *                  direct syscall. errno is saved and restored around all
 *                  mirror work.
 *
 * FAIL-OPEN IS THE CONTRACT. Ring absent, capture absent, dlsym broken, fd
 * table exhausted, ring full, another writer holding the lock - in every
 * combination the stock call still happens exactly once and its return value
 * and errno reach the caller unchanged. The mirror never blocks: ring
 * contention DROPS the sample.
 *
 * Everything on the mirror path is memcpy and atomics. No allocation, and no
 * syscall at all unless diagnostic capture is explicitly enabled.
 *
 * BENCH ONLY until validated on hardware.
 */
#include "ascent_types.h"
#include "ascent_tap_core.h"
#include "ascent_ring.h"

/* Only the interposers may be visible. Everything else stays hidden so this
 * library cannot shadow an unrelated symbol inside the stock daemon. The
 * build also passes -fvisibility=hidden; this is the explicit opt-out. */
#define TAP_EXPORT __attribute__((visibility("default")))

/* ---- direct syscalls: the fall-through path ---------------------------- */
#define SYS_getpid     172
#define SYS_close       57
#define SYS_read        63
#define SYS_write       64
#define SYS_openat      56
#define SYS_lseek       62
#define SYS_ftruncate   46
#define SYS_mmap       222
#define SYS_readlinkat  78
#define SYS_dup         23
#define SYS_dup3        24   /* aarch64 has no dup2; it is dup3(old,new,0) */
#define AT_FDCWD      (-100)

#define O_RDWR     02
#define O_WRONLY   01
#define O_CREAT    0100
#define O_APPEND   02000
#define O_CLOEXEC  02000000
#define SEEK_END    2
#define PROT_READ   1
#define PROT_WRITE  2
#define MAP_SHARED  1

static inline as64 sc(long nr, long a, long b, long c, long d, long e, long f)
{
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc 0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory", "cc");
    return x0;
}
static as64 raw_read  (int fd, void *b, ausize n)        { return sc(SYS_read,  fd,(long)b,(long)n,0,0,0); }
static as64 raw_write (int fd, const void *b, ausize n)  { return sc(SYS_write, fd,(long)b,(long)n,0,0,0); }
static as64 raw_close (int fd)                           { return sc(SYS_close, fd,0,0,0,0,0); }
static as64 raw_openat(const char *p, int fl, int md)    { return sc(SYS_openat, AT_FDCWD,(long)p,fl,md,0,0); }
static as64 raw_lseek (int fd, as64 o, int wh)           { return sc(SYS_lseek, fd,(long)o,wh,0,0,0); }
static as64 raw_ftrunc(int fd, as64 len)                 { return sc(SYS_ftruncate, fd,(long)len,0,0,0,0); }
static as64 raw_getpid(void)                             { return sc(SYS_getpid, 0,0,0,0,0,0); }
static void *raw_mmap (ausize len, int prot, int fd)
{ return (void*)sc(SYS_mmap, 0,(long)len,prot,MAP_SHARED,fd,0); }
static as64 raw_readlink(const char *p, char *b, ausize n)
{ return sc(SYS_readlinkat, AT_FDCWD,(long)p,(long)b,(long)n,0,0); }

extern void *dlsym(void *handle, const char *symbol);
extern char *getenv(const char *);
extern int  *__errno_location(void);
#define RTLD_NEXT ((void *)-1L)

/* ---- state ------------------------------------------------------------- */
typedef assize (*fn_rw)(int, void *, ausize);
typedef assize (*fn_wr)(int, const void *, ausize);
typedef int    (*fn_close)(int);
typedef int    (*fn_open)(const char *, int, ...);
typedef int    (*fn_openat)(int, const char *, int, ...);
typedef int    (*fn_dup)(int);
typedef int    (*fn_dup2)(int, int);
typedef int    (*fn_dup3)(int, int, int);

static fn_rw     real_read;
static fn_wr     real_write;
static fn_close  real_close;
static fn_open   real_open;
static fn_openat real_openat;
static fn_dup    real_dup;
static fn_dup2   real_dup2;
static fn_dup3   real_dup3;

static a_tap_fds      FDS;
static a_ring_writer  RING_TX, RING_RX;
static int  ring_tx_ok, ring_rx_ok;
static int  cap_fd = -1;
static au64 cap_written;
static au64 CAP_LIMIT;                 /* diagnostic capture ceiling */
static volatile int booting = 1;       /* suppress mirroring during init */
static volatile int syms_done;
static au64 st_mirror_calls, st_mirror_bytes, st_cap_short, st_cap_fail;

#define DEFAULT_CAP_LIMIT (4UL*1024UL*1024UL)

/* errno must be exactly what the stock call produced. */
static inline int  err_get(void){ int *e = __errno_location(); return e ? *e : 0; }
static inline void err_set(int v){ int *e = __errno_location(); if (e) *e = v; }

static void resolve_syms(void)
{
    if (syms_done) return;
    syms_done = 1;                     /* set FIRST: dlsym may itself call us */
    real_read   = (fn_rw)    dlsym(RTLD_NEXT, "read");
    real_write  = (fn_wr)    dlsym(RTLD_NEXT, "write");
    real_close  = (fn_close) dlsym(RTLD_NEXT, "close");
    real_open   = (fn_open)  dlsym(RTLD_NEXT, "open");
    real_openat = (fn_openat)dlsym(RTLD_NEXT, "openat");
    real_dup    = (fn_dup)   dlsym(RTLD_NEXT, "dup");
    real_dup2   = (fn_dup2)  dlsym(RTLD_NEXT, "dup2");
    real_dup3   = (fn_dup3)  dlsym(RTLD_NEXT, "dup3");
}

static void dec(char *d, ausize cap, long v)
{
    char t[24]; int k = 0;
    ausize n = 0; while (n + 1 < cap && d[n]) n++;
    if (v == 0) t[k++] = '0';
    while (v > 0 && k < 20) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k && n + 1 < cap) d[n++] = t[--k];
    d[n] = 0;
}

/* Raw syscalls only. Using libc open/close here would recurse straight back
 * through our own interposers during the constructor. */
static int ring_attach_one(a_ring_writer *w, const char *path)
{
    int fd = (int)raw_openat(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    as64 sz = raw_lseek(fd, 0, SEEK_END);
    if (sz < (as64)A_RING_FILE_BYTES) {
        if (raw_ftrunc(fd, (as64)A_RING_FILE_BYTES) < 0) { raw_close(fd); return 0; }
    }
    void *m = raw_mmap((ausize)A_RING_FILE_BYTES, PROT_READ | PROT_WRITE, fd);
    raw_close(fd);
    if ((as64)m < 0 && (as64)m > -4096) return 0;    /* syscall returned -errno */
    if (!m) return 0;
    a_ring_writer_bind(w, m, 0);
    return 1;
}

/* TX and RX get SEPARATE rings. Mixing them into one would interleave two
 * independent byte streams and destroy CRSF framing for both. Each ring can
 * fail to attach independently; the mirror simply goes quiet for that
 * direction and the stock daemon is unaffected either way. */
static void ring_attach(void)
{
    ring_tx_ok = ring_attach_one(&RING_TX, A_RING_PATH_TX);
    ring_rx_ok = ring_attach_one(&RING_RX, A_RING_PATH_RX);
}

static void capture_open(void)
{
    const char *want = getenv("ASCENT_TAP_CAPTURE");
    if (!want || want[0] != '1') return;             /* OFF by default */

    CAP_LIMIT = DEFAULT_CAP_LIMIT;
    const char *lim = getenv("ASCENT_TAP_CAPTURE_BYTES");
    if (lim) {
        au64 v = 0; int any = 0;
        for (int i = 0; lim[i] >= '0' && lim[i] <= '9' && i < 18; i++) {
            v = v * 10 + (au64)(lim[i] - '0'); any = 1;
        }
        if (any && v >= 65536) CAP_LIMIT = v;
    }
    /* pid-suffixed and O_APPEND: a re-exec'd instance can neither truncate
     * nor interleave into another instance's capture (F-TAP-5). */
    char path[128];
    path[0] = 0;
    { const char *p = "/tmp/ascent_ttyS2_tx."; ausize i = 0;
      for (; p[i]; i++) path[i] = p[i];
      path[i] = 0; }
    dec(path, sizeof path, (long)raw_getpid());
    { ausize n = 0; while (path[n]) n++;
      const char *e = ".bin"; for (int i = 0; e[i] && n + 1 < sizeof path; i++) path[n++] = e[i];
      path[n] = 0; }
    cap_fd = (int)raw_openat(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (cap_fd < 0) { cap_fd = -1; st_cap_fail++; }
}

#ifdef ASCENT_NATIVE_UART
/* LD_PRELOAD is inherited by subprocesses. Helpers must not reset the two
 * producer rings; argv[0] remains ar_ldy_gnd even across its vendor memfd exec. */
static int native_ground_process(void)
{
    char name[128];
    int fd=(int)raw_openat("/proc/self/cmdline",0,0);
    if(fd<0)return 0;
    as64 n=raw_read(fd,name,sizeof name-1);raw_close(fd);
    if(n<=0)return 0;
    name[n]=0;
    const char *base=name;
    for(char *p=name;*p;p++)if(*p=='/')base=p+1;
    return a_streq(base,"ar_ldy_gnd");
}
#endif

__attribute__((constructor))
static void tap_init(void)
{
    static const char msg[] =
        "ASCENT UART TAP v2 - mirror only, ring IPC, no UART ownership\n";
    booting = 1;
    a_tap_fds_init(&FDS);
    resolve_syms();
#ifdef ASCENT_NATIVE_UART
    if(!native_ground_process())return;
#endif
    ring_attach();
    capture_open();
    raw_write(2, msg, sizeof(msg) - 1);
    booting = 0;
}

/* ---- the mirror path: no allocation, no blocking, no libc -------------- */
#define DIR_TX 0
#define DIR_RX 1
static void mirror(int dir, const void *buf, ausize n)
{
    int saved = err_get();
    st_mirror_calls++;
    st_mirror_bytes += n;

    if (dir == DIR_TX) { if (ring_tx_ok) a_ring_write(&RING_TX, buf, n); }
    else               { if (ring_rx_ok) a_ring_write(&RING_RX, buf, n); }

    if (dir == DIR_TX && cap_fd >= 0 && cap_written < CAP_LIMIT) {
        ausize want = n;
        if (want > CAP_LIMIT - cap_written) want = (ausize)(CAP_LIMIT - cap_written);
        /* One non-blocking attempt. A short write is recorded and abandoned:
         * retrying would risk delaying the stock daemon, which is never worth
         * a diagnostic byte. */
        as64 w = raw_write(cap_fd, buf, want);
        if (w > 0) { cap_written += (au64)w; if ((ausize)w < want) st_cap_short++; }
        else if (w < 0) st_cap_fail++;
    }
    err_set(saved);
}

/* Classify a descriptor at most once per lifetime (F-TAP-4). */
static int is_target(int fd)
{
    if (a_tap_is_target(&FDS, fd)) return 1;
    if (!a_tap_needs_probe(&FDS, fd)) return 0;

    int saved = err_get();
    char path[40] = "/proc/self/fd/";
    dec(path, sizeof path, (long)fd);
    char link[128];
    as64 n = raw_readlink(path, link, sizeof(link) - 1);
    int hit = 0;
    if (n > 0) { link[n] = 0; hit = a_tap_path_is_target(link); }
    a_tap_note_probe(&FDS, fd, hit);
    err_set(saved);
    if (hit) {
        static const char m[] = "ASCENT UART TAP target " A_TAP_TTY_PATH " acquired\n";
        raw_write(2, m, sizeof(m) - 1);
    }
    return hit;
}

/* ---- interposers -------------------------------------------------------
 * Every one of these performs the stock call EXACTLY ONCE and returns its
 * result verbatim. Mirror work happens only afterwards and cannot change the
 * result or errno. */

TAP_EXPORT assize read(int fd, void *buf, ausize count)
{
    assize n;
    if (!syms_done) resolve_syms();
    n = real_read ? real_read(fd, buf, count) : raw_read(fd, buf, count);
    if (n > 0 && !booting && is_target(fd)) mirror(DIR_RX, buf, (ausize)n);
    return n;
}

TAP_EXPORT assize write(int fd, const void *buf, ausize count)
{
    assize n;
    if (!syms_done) resolve_syms();
    n = real_write ? real_write(fd, buf, count) : raw_write(fd, buf, count);
    if (n > 0 && !booting && is_target(fd)) mirror(DIR_TX, buf, (ausize)n);
    return n;
}

TAP_EXPORT int close(int fd)
{
    if (!syms_done) resolve_syms();
    /* Invalidate BEFORE the close so a racing thread cannot mirror into a
     * descriptor number that is already being recycled. */
    if (!booting) a_tap_note_close(&FDS, fd);
    return real_close ? real_close(fd) : (int)raw_close(fd);
}

TAP_EXPORT int open(const char *path, int flags, ...)
{
    int mode = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, flags);
    if (flags & O_CREAT) mode = __builtin_va_arg(ap, int);
    __builtin_va_end(ap);
    if (!syms_done) resolve_syms();
    int fd = real_open ? real_open(path, flags, mode)
                       : (int)raw_openat(path, flags, mode);
    if (fd >= 0 && !booting) a_tap_note_open(&FDS, fd, path);
    return fd;
}

TAP_EXPORT int openat(int dirfd, const char *path, int flags, ...)
{
    int mode = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, flags);
    if (flags & O_CREAT) mode = __builtin_va_arg(ap, int);
    __builtin_va_end(ap);
    if (!syms_done) resolve_syms();
    int fd = real_openat ? real_openat(dirfd, path, flags, mode)
                         : (int)sc(SYS_openat, dirfd, (long)path, flags, mode, 0, 0);
    /* Only an absolute path can be judged here; a relative one is left
     * unclassified and will be probed once if it is ever used. */
    if (fd >= 0 && !booting && path && path[0] == '/') a_tap_note_open(&FDS, fd, path);
    return fd;
}

/* All three fall through to a raw syscall when dlsym has not resolved. An
 * interposer must NEVER invent a failure the stock call would not have
 * produced - that is F-TAP-6, and returning -1 here would reintroduce it. */
TAP_EXPORT int dup(int oldfd)
{
    if (!syms_done) resolve_syms();
    int fd = real_dup ? real_dup(oldfd) : (int)sc(SYS_dup, oldfd, 0, 0, 0, 0, 0);
    if (fd >= 0 && !booting) a_tap_note_dup(&FDS, oldfd, fd);
    return fd;
}

TAP_EXPORT int dup2(int oldfd, int newfd)
{
    if (!syms_done) resolve_syms();
    int fd;
    if (real_dup2) fd = real_dup2(oldfd, newfd);
    else if (oldfd == newfd) fd = oldfd;          /* dup2 semantics; dup3 would EINVAL */
    else fd = (int)sc(SYS_dup3, oldfd, newfd, 0, 0, 0, 0);
    if (fd >= 0 && !booting) a_tap_note_dup(&FDS, oldfd, fd);
    return fd;
}

TAP_EXPORT int dup3(int oldfd, int newfd, int flags)
{
    if (!syms_done) resolve_syms();
    int fd = real_dup3 ? real_dup3(oldfd, newfd, flags)
                       : (int)sc(SYS_dup3, oldfd, newfd, flags, 0, 0, 0);
    if (fd >= 0 && !booting) a_tap_note_dup(&FDS, oldfd, fd);
    return fd;
}
