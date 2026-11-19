#include "ascent_frame.h"
#include "ascent_sys.h"

#define BARRIER() __sync_synchronize()

static void dec_append(char *d, ausize cap, long v)
{
    char t[24]; int k = 0;
    ausize n = a_strnlen(d, cap);
    if (v == 0) t[k++] = '0';
    while (v > 0 && k < 20) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k && n + 1 < cap) d[n++] = t[--k];
    d[n] = 0;
}

/* ------------------------------------------------------------- producer */

int a_frame_producer_open(a_frame_producer *p, const char *path, au64 instance_id)
{
    return a_frame_producer_open_sz(p, path, instance_id, A_FRAME_W, A_FRAME_H);
}

int a_frame_producer_open_sz(a_frame_producer *p, const char *path,
                             au64 instance_id, au32 w, au32 h)
{
    /* Refuse an out-of-range geometry here rather than clamping silently: a
     * producer that cannot publish what it was asked for should fail loudly,
     * not publish something else that every consumer will then believe. */
    if (w < A_FRAME_MIN_W || w > A_FRAME_MAX_W ||
        h < A_FRAME_MIN_H || h > A_FRAME_MAX_H) return 0;

    au64 buf_bytes  = A_FRAME_BUF_BYTES_OF(w, h);
    au64 file_bytes = A_FRAME_FILE_BYTES_OF(w, h);

    a_memset(p, 0, sizeof(*p));
    p->fd = -1;
    a_strlcpy(p->path, path ? path : A_FRAME_PATH, sizeof(p->path));
    a_strlcpy(p->tmp, p->path, sizeof(p->tmp));
    a_strlcpy(p->tmp + a_strnlen(p->tmp, sizeof(p->tmp)), ".tmp.",
              sizeof(p->tmp) - a_strnlen(p->tmp, sizeof(p->tmp)));
    dec_append(p->tmp, sizeof(p->tmp), (long)getpid());

    /* The temp path is private to this producer, so O_TRUNC here is safe:
     * by construction nobody has it mapped. The PUBLISHED path is never
     * opened with O_TRUNC - that was the SIGBUS source. */
    unlink(p->tmp);
    int fd = open(p->tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    if (ftruncate(fd, (as64)file_bytes) != 0) { close(fd); unlink(p->tmp); return 0; }

    void *m = mmap(ASCENT_NULL, (ausize)file_bytes,
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); unlink(p->tmp); return 0; }

    p->fd  = fd;
    p->map = (au8*)m;
    p->hdr = (a_frame_hdr*)m;
    p->instance_id = instance_id ? instance_id
                                 : ((au64)getpid() << 32) ^ a_now_ms();

    a_memset(p->hdr, 0, (ausize)A_FRAME_HDR_BYTES);
    p->hdr->magic       = A_FRAME_MAGIC;
    p->hdr->version     = A_FRAME_VERSION;
    p->hdr->hdr_bytes   = (au32)A_FRAME_HDR_BYTES;
    p->hdr->width       = w;
    p->hdr->height      = h;
    p->hdr->stride      = (au32)A_FRAME_STRIDE_OF(w);
    p->hdr->buffers     = A_FRAME_BUFFERS;
    p->hdr->buf_bytes   = buf_bytes;
    p->hdr->file_bytes  = file_bytes;
    p->hdr->instance_id = p->instance_id;
    p->hdr->seq         = 0;
    p->hdr->active      = 0;
    p->hdr->flags       = A_FF_PRODUCER_LIVE;
    p->hdr->publish_ms  = a_now_ms();
    BARRIER();

    /* Atomic publication. A consumer holding the old inode keeps a valid,
     * fully sized mapping; it discovers the change via instance_id. */
    if (rename(p->tmp, p->path) != 0) {
        munmap(m, (ausize)file_bytes);
        close(fd); unlink(p->tmp);
        p->fd = -1; p->map = ASCENT_NULL; p->hdr = ASCENT_NULL;
        return 0;
    }
    p->w = w; p->h = h;
    p->buf_bytes = buf_bytes;
    p->map_bytes = file_bytes;
    p->ok = 1;
    return 1;
}

au32 *a_frame_producer_begin(a_frame_producer *p)
{
    if (!p || !p->ok || !p->map) return ASCENT_NULL;
    au64 next = p->hdr->seq % A_FRAME_BUFFERS;   /* frame number seq -> index */
    return (au32*)(p->map + A_FRAME_HDR_BYTES + next * p->buf_bytes);
}

void a_frame_producer_publish(a_frame_producer *p, au64 now_ms)
{
    if (!p || !p->ok || !p->map) return;
    au64 f = p->hdr->seq;                 /* the frame we just wrote */
    BARRIER();                            /* pixels before the publish */
    p->hdr->active     = (au32)(f % A_FRAME_BUFFERS);
    p->hdr->publish_ms = now_ms;
    p->hdr->frames     = f + 1;
    BARRIER();
    p->hdr->seq        = f + 1;           /* the publish itself */
    BARRIER();
}

void a_frame_producer_close(a_frame_producer *p)
{
    if (!p) return;
    if (p->map) {
        p->hdr->flags &= ~A_FF_PRODUCER_LIVE;
        BARRIER();
        munmap(p->map, (ausize)p->map_bytes);
    }
    if (p->fd >= 0) close(p->fd);
    p->map = ASCENT_NULL; p->hdr = ASCENT_NULL; p->fd = -1; p->ok = 0;
}

/* ------------------------------------------------------------- consumer */

void a_frame_consumer_init(a_frame_consumer *c, const char *path)
{
    a_memset(c, 0, sizeof(c[0]));
    c->fd = -1;
    a_strlcpy(c->path, path ? path : A_FRAME_PATH, sizeof(c->path));
}

void a_frame_consumer_detach(a_frame_consumer *c)
{
    /* munmap with the length ACTUALLY mapped. Using a compile-time constant
     * here was correct only while every producer shared one geometry; with
     * Phase2E it would unmap the wrong span. */
    if (c->map) munmap(c->map, (ausize)c->map_bytes);
    c->map = ASCENT_NULL; c->hdr = ASCENT_NULL;
    c->instance_id = 0; c->last_seq = 0;
    c->w = 0; c->h = 0; c->buf_bytes = 0; c->map_bytes = 0;
}

/* Phase2E: the geometry is no longer fixed, so this can no longer be an
 * equality test against compile-time constants. It is now a test that the
 * header is INTERNALLY CONSISTENT and within bounds - which is strictly
 * stronger than the old check in the ways that matter, because the old one
 * never verified that stride, buf_bytes and file_bytes agreed with each
 * other; it only verified they matched one hardcoded geometry.
 *
 * Every field here sizes an mmap or a memcpy, so each is treated as input
 * even though our own producer writes it. A header that disagrees with
 * itself is refused rather than reconciled. */
static int hdr_valid(const a_frame_hdr *h)
{
    if (h->magic != A_FRAME_MAGIC) return 0;
    if (h->version != A_FRAME_VERSION) return 0;
    if (h->hdr_bytes != (au32)A_FRAME_HDR_BYTES) return 0;
    if (h->buffers != A_FRAME_BUFFERS) return 0;
    if (h->width  < A_FRAME_MIN_W || h->width  > A_FRAME_MAX_W) return 0;
    if (h->height < A_FRAME_MIN_H || h->height > A_FRAME_MAX_H) return 0;
    if ((au64)h->stride != A_FRAME_STRIDE_OF(h->width)) return 0;
    if (h->buf_bytes  != A_FRAME_BUF_BYTES_OF(h->width, h->height)) return 0;
    if (h->file_bytes != A_FRAME_FILE_BYTES_OF(h->width, h->height)) return 0;
    return 1;
}

int a_frame_consumer_attach(a_frame_consumer *c)
{
    if (c->map) return 1;
    int fd = open(c->path, O_RDONLY);
    if (fd < 0) return 0;

    /* Size check BEFORE mmap. A file that is short - because a producer is
     * mid-creation, or crashed, or an old-format producer truncated it - is
     * refused rather than mapped and faulted on. No struct stat needed. */
    /* Order matters now. Read and validate the header FIRST, because the
     * header is what says how big the file must be; only then size-check and
     * map. The old code checked against a constant, which is exactly the
     * assumption Phase2E removes. */
    as64 sz = lseek(fd, 0, SEEK_END);
    if (sz < (as64)A_FRAME_HDR_BYTES) { close(fd); c->map_failures++; return 0; }

    a_frame_hdr probe;
    if (lseek(fd, 0, SEEK_SET) < 0 ||
        a_read_full(fd, &probe, sizeof(probe)) != (assize)sizeof(probe)) {
        close(fd); c->map_failures++; return 0;
    }
    if (!hdr_valid(&probe)) { close(fd); c->map_failures++; return 0; }

    /* A file shorter than its own header claims is refused, never mapped:
     * that is the SIGBUS guard, and it must use the header's figure. */
    if (sz < (as64)probe.file_bytes) { close(fd); c->map_failures++; return 0; }

    void *m = mmap(ASCENT_NULL, (ausize)probe.file_bytes, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);                       /* the mapping keeps the inode alive */
    if (m == MAP_FAILED) { c->map_failures++; return 0; }

    c->map = (au8*)m;
    c->hdr = (const a_frame_hdr*)m;
    c->instance_id = probe.instance_id;
    c->w = probe.width; c->h = probe.height;
    c->buf_bytes = probe.buf_bytes;
    c->map_bytes = probe.file_bytes;
    c->last_seq = 0;
    c->remaps++;
    return 1;
}

int a_frame_consumer_replaced(a_frame_consumer *c)
{
    c->probe_calls++;
    int fd = open(c->path, O_RDONLY);
    if (fd < 0) return 0;                    /* gone: keep what we have */
    a_frame_hdr probe;
    assize r = a_read_full(fd, &probe, sizeof(probe));
    close(fd);
    if (r != (assize)sizeof(probe)) return 0;
    if (!hdr_valid(&probe)) return 0;
    return probe.instance_id != c->instance_id;
}

int a_frame_consumer_take(a_frame_consumer *c, void *dst, ausize dst_bytes)
{
    if (!c->map || !c->hdr) return A_FRAME_TAKE_DETACHED;
    /* Fail closed when the caller's buffer is too small for the ATTACHED
     * geometry. A consumer that has not yet resized its scratch after a
     * producer changed size gets a clean refusal, not an overrun. */
    if (dst_bytes < (ausize)c->buf_bytes) return A_FRAME_TAKE_DETACHED;

    for (int attempt = 0; attempt < 4; attempt++) {
        au64 s0 = c->hdr->seq;
        BARRIER();
        if (s0 == 0) return A_FRAME_TAKE_NO_NEW;      /* nothing published yet */
        if (s0 == c->last_seq) return A_FRAME_TAKE_NO_NEW;

        au64 idx = (s0 - 1) % A_FRAME_BUFFERS;
        const au8 *src = c->map + A_FRAME_HDR_BYTES + idx * c->buf_bytes;
        a_memcpy(dst, src, (ausize)c->buf_bytes);
        BARRIER();
        au64 s1 = c->hdr->seq;

        /* See the derivation in ascent_frame.h: the index we read is next
         * written by frame s0-1+N, which starts when seq reaches s0+N-2. */
        if (s1 >= s0 && (s1 - s0) <= (au64)(A_FRAME_BUFFERS - 2)) {
            c->last_seq = s0;
            c->frames_taken++;
            return A_FRAME_TAKE_OK;
        }
        c->torn_retries++;
    }
    c->torn_giveups++;
    return A_FRAME_TAKE_TORN;
}

au64 a_frame_consumer_age_ms(const a_frame_consumer *c, au64 now_ms)
{
    if (!c->hdr) return (au64)~0ull;
    au64 pub = c->hdr->publish_ms;
    if (pub == 0 || now_ms < pub) return 0;
    return now_ms - pub;
}
