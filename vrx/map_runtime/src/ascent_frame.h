/* ascent_frame.h — shared-frame producer/consumer contract.
 *
 * This replaces the pre-fix transport, which had two proven failure modes
 * (red-team P0-1 / F-FRAME-1):
 *
 *   1. The producer opened the published path with O_TRUNC. A consumer that
 *      had it mmap'd took SIGBUS on the pages beyond the momentary zero
 *      length, and SIGBUS killed GlassesUI. Reproduced.
 *   2. The documented restart procedure rm'd the file. The consumer kept the
 *      orphaned inode mapped and displayed a frozen frame forever, silently -
 *      including a frozen freshness indicator, because the indicator was
 *      drawn INSIDE the frame. Reproduced.
 *
 * The contract now is:
 *
 *  PUBLICATION IS BY RENAME. The producer builds a fully-sized private temp
 *  file, then rename()s it over the published path. rename is atomic; a
 *  consumer holding the previous inode keeps a valid, fully-sized mapping and
 *  can never fault. The published path is NEVER opened with O_TRUNC and is
 *  never ftruncate()d.
 *
 *  INSTANCE ID. Every producer start stamps a unique instance_id. A consumer
 *  that sees no new frames re-reads just the header from the PATH (open,
 *  read 64 bytes, close - no mmap) and remaps when the id changes. That is
 *  how it follows a restart instead of pinning a dead inode.
 *
 *  N-BUFFER SEQLOCK. buffers = 3. Publishing frame F writes index F % 3 and
 *  then sets seq = F + 1 and active = F % 3. A consumer reads seq0 and
 *  active, copies, reads seq1, and accepts the copy only if
 *      seq1 - seq0 <= A_FRAME_BUFFERS - 2   (i.e. <= 1)
 *  Derivation: the index it read, (seq0-1) % N, is next written by frame
 *  seq0-1+N, which the producer STARTS when seq reaches seq0+N-2. So any
 *  larger delta means the copy may have been overwritten mid-flight.
 *  The pre-fix consumer had no post-copy check at all.
 *
 *  STALENESS IS THE CONSUMER'S JOB. publish_ms is written by the producer
 *  from CLOCK_MONOTONIC, which is system-wide on Linux and therefore directly
 *  comparable in the consumer. An indicator drawn inside the frame cannot
 *  report that frames stopped arriving; the consumer must decide that.
 *
 *  SIZE IS VERIFIED BEFORE MAPPING. lseek(SEEK_END) must report at least the
 *  full file size, so a short or still-being-created file is never mapped.
 *  No struct stat is used, which keeps this identical hosted and freestanding.
 */
#ifndef ASCENT_FRAME_H
#define ASCENT_FRAME_H
#include "ascent_types.h"

#define A_FRAME_MAGIC    0x32414D41u   /* 'AMA2' */
#define A_FRAME_VERSION  3u
#define A_FRAME_BUFFERS  3u
/* DEFAULTS, not the contract. Phase2E made width/height runtime values that
 * the producer chooses and publishes in the header; both sides size every
 * allocation from the header, never from these. They remain because the host
 * tests and the default producer entry point use them. */
#define A_FRAME_W        600u
#define A_FRAME_H        340u

/* Hard bounds. A header outside these is refused rather than mapped: the
 * width/height fields size an mmap and a memcpy, so they are attacker-ish
 * input even when the producer is ours. Upper bound is the panel this
 * hardware drives; a map widget larger than the display has no meaning. */
#define A_FRAME_MIN_W    160u
#define A_FRAME_MIN_H    90u
#define A_FRAME_MAX_W    1920u
#define A_FRAME_MAX_H    1080u
#define A_FRAME_PATH     "/tmp/ascent_map_frame.bin"

/* flags */
#define A_FF_PRODUCER_LIVE 0x1u

typedef struct {
    au32 magic;
    au32 version;
    au32 hdr_bytes;
    au32 width;
    au32 height;
    au32 stride;        /* bytes per row; == width*4 */
    au32 buffers;       /* A_FRAME_BUFFERS */
    au32 flags;
    au64 buf_bytes;     /* stride * height */
    au64 file_bytes;    /* total mapping size, header included */
    au64 instance_id;   /* unique per producer start */
    au64 seq;           /* frames published; index of last = (seq-1) % buffers */
    au64 publish_ms;    /* CLOCK_MONOTONIC ms at last publish */
    au64 frames;        /* == seq, kept separate for diagnostics */
    au32 active;        /* (seq-1) % buffers, cached */
    au32 reserved[9];
} a_frame_hdr;

#define A_FRAME_HDR_BYTES  ((au64)sizeof(a_frame_hdr))

/* Size arithmetic for an arbitrary geometry. Every multiplication is done in
 * au64 so a hostile or corrupt header cannot wrap a 32-bit intermediate and
 * produce a small file_bytes for a huge width - which would pass a size check
 * and then overrun on copy. */
#define A_FRAME_STRIDE_OF(w)        ((au64)(w) * 4u)
#define A_FRAME_BUF_BYTES_OF(w,h)   (A_FRAME_STRIDE_OF(w) * (au64)(h))
#define A_FRAME_FILE_BYTES_OF(w,h)  (A_FRAME_HDR_BYTES + \
                                     A_FRAME_BUF_BYTES_OF(w,h) * A_FRAME_BUFFERS)

/* The default geometry's sizes, for the default producer and the host tests. */
#define A_FRAME_BUF_BYTES  A_FRAME_BUF_BYTES_OF(A_FRAME_W, A_FRAME_H)
#define A_FRAME_FILE_BYTES A_FRAME_FILE_BYTES_OF(A_FRAME_W, A_FRAME_H)

/* ------------------------------------------------------------- producer */
typedef struct {
    int   fd;
    au8  *map;
    a_frame_hdr *hdr;
    au64  instance_id;
    char  path[192];
    char  tmp[208];
    int   ok;
    /* Phase2E: the geometry this producer actually publishes, and the sizes
     * derived from it. map_bytes is what was mmap'd and is what must be
     * munmap'd - never a compile-time constant. */
    au32  w, h;
    au64  buf_bytes;
    au64  map_bytes;
} a_frame_producer;

/* Creates the temp file, maps it, and renames it into place. Returns 1 on
 * success. Safe to call while a consumer holds the previous inode.
 * The unsized form publishes A_FRAME_W x A_FRAME_H and exists so the host
 * tests and any default caller are unchanged by Phase2E. */
int   a_frame_producer_open(a_frame_producer *p, const char *path, au64 instance_id);
int   a_frame_producer_open_sz(a_frame_producer *p, const char *path,
                               au64 instance_id, au32 w, au32 h);
/* Returns a writable pointer to the buffer for the NEXT frame, or NULL. */
au32 *a_frame_producer_begin(a_frame_producer *p);
/* Publishes the buffer returned by begin(). */
void  a_frame_producer_publish(a_frame_producer *p, au64 now_ms);
void  a_frame_producer_close(a_frame_producer *p);

/* ------------------------------------------------------------- consumer */
typedef struct {
    int   fd;             /* always -1: the consumer does not hold the fd open */
    au8  *map;
    const a_frame_hdr *hdr;
    au64  instance_id;
    au64  last_seq;
    char  path[192];
    /* Phase2E: the geometry of the mapping currently held. map_bytes is the
     * length passed to mmap and is the ONLY length that may be passed to
     * munmap; buf_bytes sizes the caller's destination. A consumer must
     * re-read these after every attach, because a new producer instance may
     * publish a different geometry. */
    au32  w, h;
    au64  buf_bytes;
    au64  map_bytes;
    /* diagnostics */
    au64  remaps, torn_retries, torn_giveups, probe_calls, map_failures;
    au64  frames_taken;
} a_frame_consumer;

void a_frame_consumer_init(a_frame_consumer *c, const char *path);
/* Attempt to (re)attach. Returns 1 when a valid mapping is held. Cheap and
 * safe to call repeatedly; it is the permanent recovery path, not a one-shot
 * startup loop. */
int  a_frame_consumer_attach(a_frame_consumer *c);
void a_frame_consumer_detach(a_frame_consumer *c);
/* True when the published path now holds a different producer instance. */
int  a_frame_consumer_replaced(a_frame_consumer *c);

#define A_FRAME_TAKE_OK        1
#define A_FRAME_TAKE_NO_NEW    0
#define A_FRAME_TAKE_DETACHED (-1)
#define A_FRAME_TAKE_TORN     (-2)
/* Copies the newest frame into dst, which must be >= c->buf_bytes for the
 * CURRENTLY ATTACHED geometry - not the compile-time default. Returns
 * A_FRAME_TAKE_DETACHED if dst is too small, so a consumer that has not
 * resized its scratch buffer fails closed instead of overrunning it.
 * Performs the seqlock check and retries internally. */
int  a_frame_consumer_take(a_frame_consumer *c, void *dst, ausize dst_bytes);
/* Milliseconds since the producer last published, or ~0 if unknown. */
au64 a_frame_consumer_age_ms(const a_frame_consumer *c, au64 now_ms);

#endif
