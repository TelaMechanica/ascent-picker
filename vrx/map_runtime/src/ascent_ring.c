#include "ascent_ring.h"
#include "ascent_sys.h"

#define BARRIER() __sync_synchronize()

/* ---------------------------------------------------------------- writer */

int a_ring_writer_open(a_ring_writer *w, const char *path, au64 epoch)
{
    a_memset(w, 0, sizeof(*w));
    const char *p = path ? path : A_RING_PATH;

    /* O_CREAT without O_TRUNC: a reader may already hold this inode mapped.
     * The size is set only if the file is short, never reduced. */
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    as64 sz = lseek(fd, 0, SEEK_END);
    if (sz < (as64)A_RING_FILE_BYTES) {
        if (ftruncate(fd, (as64)A_RING_FILE_BYTES) != 0) { close(fd); return 0; }
    }
    void *m = mmap(ASCENT_NULL, (ausize)A_RING_FILE_BYTES,
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);                              /* mapping keeps the inode alive */
    if (m == MAP_FAILED) return 0;

    a_ring_writer_bind(w, m, epoch);
    return 1;
}

void a_ring_writer_bind(a_ring_writer *w, void *m, au64 epoch)
{
    a_ring_hdr *h = (a_ring_hdr*)m;
    /* Reuse of an existing inode is deliberate: a reader may already hold it
     * mapped, and recreating the file would strand that reader. A fresh epoch
     * resets the ring in place, which also releases a lock orphaned by a
     * writer that was killed mid-copy. */
    au64 prev = (h->magic == A_RING_MAGIC) ? h->epoch : 0;

    h->magic       = A_RING_MAGIC;
    h->version     = A_RING_VERSION;
    h->hdr_bytes   = (au32)A_RING_HDR_BYTES;
    h->data_bytes  = A_RING_DATA_BYTES;
    h->wpos        = 0;
    h->writes      = 0;
    h->write_bytes = 0;
    h->lock_drops  = 0;
    h->oversize_trims = 0;
    h->wlock       = 0;
    BARRIER();
    h->epoch       = epoch ? epoch : (prev + 1);
    BARRIER();

    w->hdr  = h;
    w->data = (au8*)m + A_RING_HDR_BYTES;
    w->ok   = 1;
}

void a_ring_write(a_ring_writer *w, const void *buf, ausize n)
{
    if (!w || !w->ok || !buf || !n) return;
    a_ring_hdr *h = w->hdr;
    const au8 *src = (const au8*)buf;

    if (n > A_RING_DATA_BYTES) {            /* keep only the freshest tail */
        src += n - A_RING_DATA_BYTES;
        n = A_RING_DATA_BYTES;
        __sync_fetch_and_add(&h->oversize_trims, 1ull);
    }

    /* Try-lock. NEVER waits: a contended sample is dropped. This is the single
     * most important property of the writer - it runs inside the stock daemon. */
    if (__sync_lock_test_and_set(&h->wlock, 1u) != 0u) {
        __sync_fetch_and_add(&h->lock_drops, 1ull);
        return;
    }

    au64 off = h->wpos % A_RING_DATA_BYTES;
    ausize first = (ausize)(A_RING_DATA_BYTES - off);
    if (first > n) first = n;
    a_memcpy(w->data + off, src, first);
    if (n > first) a_memcpy(w->data, src + first, n - first);
    BARRIER();
    h->wpos       += n;
    h->writes     += 1;
    h->write_bytes += n;
    BARRIER();
    __sync_lock_release(&h->wlock);
}

void a_ring_writer_close(a_ring_writer *w)
{
    if (!w || !w->ok) return;
    munmap((void*)w->hdr, (ausize)A_RING_FILE_BYTES);
    w->hdr = ASCENT_NULL; w->data = ASCENT_NULL; w->ok = 0;
}

/* ---------------------------------------------------------------- reader */

void a_ring_reader_init(a_ring_reader *r, const char *path)
{
    a_memset(r, 0, sizeof(*r));
    a_strlcpy(r->path, path ? path : A_RING_PATH, sizeof(r->path));
}

void a_ring_reader_detach(a_ring_reader *r)
{
    if (r->map) munmap(r->map, (ausize)A_RING_FILE_BYTES);
    r->map = ASCENT_NULL; r->hdr = ASCENT_NULL; r->data = ASCENT_NULL;
    r->ok = 0; r->epoch = 0; r->rpos = 0;
}

int a_ring_reader_attach(a_ring_reader *r)
{
    if (r->ok) return 1;
    int fd = open(r->path, O_RDONLY);
    if (fd < 0) { r->attach_failures++; return 0; }
    as64 sz = lseek(fd, 0, SEEK_END);
    if (sz < (as64)A_RING_FILE_BYTES) { close(fd); r->attach_failures++; return 0; }

    void *m = mmap(ASCENT_NULL, (ausize)A_RING_FILE_BYTES, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { r->attach_failures++; return 0; }

    const a_ring_hdr *h = (const a_ring_hdr*)m;
    if (h->magic != A_RING_MAGIC || h->version != A_RING_VERSION ||
        h->data_bytes != A_RING_DATA_BYTES || h->hdr_bytes != (au32)A_RING_HDR_BYTES) {
        munmap(m, (ausize)A_RING_FILE_BYTES);
        r->attach_failures++;
        return 0;
    }
    r->map  = m;
    r->hdr  = h;
    r->data = (const au8*)m + A_RING_HDR_BYTES;
    r->epoch = h->epoch;
    r->rpos  = h->wpos;      /* start live, do not replay history */
    r->ok    = 1;
    return 1;
}

ausize a_ring_read(a_ring_reader *r, ausize max_bytes, int *restarted,
                   void (*sink)(void*, const au8*, ausize), void *user)
{
    if (restarted) *restarted = 0;
    if (!r || !r->ok || !sink) return 0;
    const a_ring_hdr *h = r->hdr;

    /* Epoch first: a restart resets wpos, so computing avail before this
     * check would underflow. */
    au64 ep = h->epoch;
    BARRIER();
    if (ep != r->epoch) {
        r->epoch = ep;
        r->rpos  = h->wpos;
        r->resyncs++;
        if (restarted) *restarted = 1;
        return 0;
    }

    au64 w = h->wpos;
    BARRIER();
    if (w < r->rpos) {                       /* writer rewound without epoch */
        r->rpos = w; r->resyncs++;
        if (restarted) *restarted = 1;
        return 0;
    }
    au64 avail = w - r->rpos;
    if (avail == 0) return 0;

    if (avail > A_RING_DATA_BYTES) {         /* lapped: jump to freshest data */
        r->rpos = w - A_RING_DATA_BYTES / 2;
        avail   = A_RING_DATA_BYTES / 2;
        r->laps++;
        if (restarted) *restarted = 1;
    }
    if (avail > max_bytes) avail = max_bytes;

    au8 tmp[4096];
    ausize delivered = 0;
    while (avail) {
        au64 off = r->rpos % A_RING_DATA_BYTES;
        ausize chunk = (ausize)(A_RING_DATA_BYTES - off);   /* no wrap inside */
        if (chunk > avail) chunk = (ausize)avail;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);

        a_memcpy(tmp, r->data + off, chunk);
        BARRIER();
        /* Verify the writer did not lap us while we were copying. */
        au64 w2 = h->wpos;
        if (h->epoch != r->epoch || w2 < r->rpos || (w2 - r->rpos) > A_RING_DATA_BYTES) {
            r->laps++;
            r->rpos = (w2 >= A_RING_DATA_BYTES / 2) ? w2 - A_RING_DATA_BYTES / 2 : 0;
            if (restarted) *restarted = 1;
            break;                            /* discard the torn chunk */
        }
        sink(user, tmp, chunk);
        r->rpos      += chunk;
        r->bytes_read += chunk;
        delivered    += chunk;
        avail        -= chunk;
    }
    return delivered;
}
