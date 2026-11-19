/* ascent_ring.h — bounded shared-memory telemetry ring.
 *
 * Replaces the pre-fix live path, which was an ever-growing capture file with
 * a hard 4 MiB cap (red-team P1-1 / F-TAP-2). At the measured 420000 8N1 line
 * rate that cap is reached in 91 s at full duty and about 15 minutes at 10%
 * duty, after which the tap stopped mirroring SILENTLY and the map froze the
 * aircraft at its last position while still looking live. It also inherited
 * F-TAP-5: a fixed O_TRUNC path shared by any re-exec'd process.
 *
 * This is a fixed-size ring, mapped once, with no growth and no syscalls on
 * the mirror path at all.
 *
 * WRITER CONTRACT (inside ar_ldy_gnd - the stock daemon):
 *   - Never blocks. Never allocates. Never makes a syscall after attach.
 *   - Contention is resolved by DROPPING the sample, never by waiting:
 *     a failed try-lock increments lock_drops and returns immediately.
 *     Dropping mirrored telemetry is always preferable to delaying the stock
 *     daemon by even one scheduler slice.
 *   - If attach failed for any reason the writer is inert; the stock read()
 *     and write() still return exactly what the real calls returned.
 *
 * READER CONTRACT (the map engine):
 *   - Attaches read-only. Starts at the current write position, so it never
 *     replays stale history on attach.
 *   - Detects writer restart by `epoch` and resynchronises the byte-stream
 *     parser rather than splicing two streams together.
 *   - Detects being lapped and skips forward to the freshest window, counting
 *     the event. Falling behind costs data, never correctness.
 *
 * OVERFLOW POLICY: newest-wins. The writer overwrites the oldest unread bytes.
 * The reader observes this as a lap and resynchronises. CRSF is a
 * self-synchronising CRC-checked byte stream, so a lap costs at most the
 * frames spanning the discontinuity.
 *
 * KNOWN LIMIT, stated rather than hidden: the try-lock serialises writers, so
 * bytes cannot interleave. If a writer is SIGKILLed while holding the lock the
 * ring stops accepting new data (mirroring ceases; the stock daemon is
 * unaffected) until the next writer attach resets the header. The reader sees
 * this as a frozen wpos and reports staleness.
 */
#ifndef ASCENT_RING_H
#define ASCENT_RING_H
#include "ascent_types.h"

#define A_RING_MAGIC      0x474E5241u   /* 'ARNG' */
#define A_RING_VERSION    1u
#define A_RING_DATA_BYTES 262144u       /* 256 KiB == ~6.2 s at 420000 8N1 */
/* One ring PER DIRECTION. A single ring would interleave the two byte streams
 * and destroy CRSF framing, so TX and RX are never mixed. The engine reads TX
 * by default and ASCENT_CRSF_RING selects otherwise. */
#define A_RING_PATH_TX    "/tmp/ascent_ttyS2_tx.ring"
#define A_RING_PATH_RX    "/tmp/ascent_ttyS2_rx.ring"
#define A_RING_PATH       A_RING_PATH_TX

typedef struct {
    au32 magic, version, hdr_bytes, data_bytes;
    au64 epoch;          /* changes on every writer attach */
    au64 wpos;           /* total bytes ever written; index = wpos % data     */
    au64 writes;         /* mirrored write() calls                            */
    au64 write_bytes;
    au64 lock_drops;     /* samples dropped because another writer held the lock */
    au64 oversize_trims; /* writes larger than the ring; only the tail is kept */
    au32 wlock;
    au32 reserved[11];
} a_ring_hdr;

#define A_RING_HDR_BYTES  ((au64)sizeof(a_ring_hdr))
#define A_RING_FILE_BYTES (A_RING_HDR_BYTES + (au64)A_RING_DATA_BYTES)

/* ---------------------------------------------------------------- writer */
typedef struct { a_ring_hdr *hdr; au8 *data; int ok; } a_ring_writer;

/* Creates or attaches the ring and stamps a fresh epoch. Returns 1 on
 * success. On failure the struct is inert and a_ring_write() is a no-op. */
int  a_ring_writer_open(a_ring_writer *w, const char *path, au64 epoch);
/* Initialise the header on a mapping the caller already established. The
 * preload tap uses this because it must reach the ring with raw syscalls: any
 * libc open/close inside it would recurse through its own interposers. */
void a_ring_writer_bind(a_ring_writer *w, void *mapping, au64 epoch);
void a_ring_write(a_ring_writer *w, const void *buf, ausize n);
void a_ring_writer_close(a_ring_writer *w);

/* ---------------------------------------------------------------- reader */
typedef struct {
    const a_ring_hdr *hdr;
    const au8 *data;
    void *map;
    au64  rpos, epoch;
    au64  laps, bytes_read, resyncs, attach_failures;
    int   ok;
    char  path[192];
} a_ring_reader;

void a_ring_reader_init(a_ring_reader *r, const char *path);
int  a_ring_reader_attach(a_ring_reader *r);
void a_ring_reader_detach(a_ring_reader *r);

/* Drains up to max_bytes into sink(). `restarted` is set to 1 when a writer
 * restart or a lap was detected, meaning the caller must resynchronise its
 * byte-stream parser. Returns bytes delivered. */
ausize a_ring_read(a_ring_reader *r, ausize max_bytes, int *restarted,
                   void (*sink)(void *user, const au8 *p, ausize n), void *user);

#endif
