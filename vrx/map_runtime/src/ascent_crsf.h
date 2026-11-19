/* ascent_crsf.h — CRSF byte-stream parser.
 *
 * Fixes carried here from the red-team review:
 *   F-CRSF-1  the address/sync byte was never checked, so the parser locked
 *             onto false frames on any (length, CRC8) coincidence.
 *   F-TAP-3   foreign bytes reaching the decoder must not become a position;
 *             the address filter is the second line of defence for that.
 *
 * Unchanged and re-verified as correct (review §6.2): frame layout, CRC8
 * polynomial 0xD5 over type+payload, big-endian signed 1e-7 lat/lon, heading
 * at payload offset 10..11 in centidegrees, partial-frame reassembly.
 *
 * Assembly buffer is a FIXED 256 bytes. Input of any size is accepted; it is
 * consumed in bounded chunks. Nothing here allocates.
 */
#ifndef ASCENT_CRSF_H
#define ASCENT_CRSF_H
#include "ascent_types.h"

#define A_CRSF_BUF            256u  /* assembly buffer, >= 4x max frame */
#define A_CRSF_MAX_LEN         62u  /* len byte max: total frame = len+2 <= 64 */
#define A_CRSF_MIN_LEN          2u
#define A_CRSF_TYPE_GPS      0x02u
#define A_CRSF_TYPE_RC_CHANNELS 0x16u  /* RC_CHANNELS_PACKED: 16 x 11-bit, 22-byte payload */
#define A_CRSF_RC_CHANNELS     16u

/* Address-byte policy. STRICT is the default and is the fix for F-CRSF-1.
 * ANY restores the pre-fix behaviour and exists only as a documented rollback
 * for the bench, because the supplied material contains no real capture and
 * therefore does not tell us which address the stock ttyS2 stream uses.
 * See BENCH_TEST_PLAN.md step B2. */
#define A_CRSF_ADDR_STRICT 0
#define A_CRSF_ADDR_ANY    1

typedef struct {
    double lat, lon;         /* WGS84 degrees */
    au16   course_cd;        /* centidegrees, 0..35999 */
    au16   groundspeed_dkmh; /* km/h * 10, as carried on the wire */
    as32   altitude_m;       /* metres AMSL, wire offset of 1000 removed */
    au8    satellites;
    au8    addr;             /* address byte of the frame it came from */
} a_crsf_gps;

/* Last accepted RC channels frame (RC_CHANNELS_PACKED, type 0x16). Values are the
 * raw CRSF ticks (nominally 172..1811, 11-bit 0..2047 range). rc_updates increments
 * only on acceptance so the picker's RC adapter can tell "new frame" from "stale",
 * and freeze (fail-safe) when no fresh frame arrives. This is the headless RC feed. */
typedef struct {
    au16 ch[A_CRSF_RC_CHANNELS]; /* raw CRSF ticks per channel */
    au8  addr;                   /* address byte of the frame it came from */
} a_crsf_rc;

typedef struct {
    au8  buf[A_CRSF_BUF];
    au32 n;
    au8  addr_policy;

    /* Last accepted position. gps_updates increments only on acceptance, so a
     * consumer can detect "no new fix" without guessing from timestamps. */
    a_crsf_gps gps;
    au64 gps_updates;

    /* Last accepted RC channels (see a_crsf_rc). rc_updates increments only on
     * acceptance; rc_frames counts every 0x16 frame seen (incl. malformed). */
    a_crsf_rc rc;
    au64 rc_updates;
    au64 rc_frames;

    /* Observability. Every rejection path is counted; nothing is silent.
     * addr_reject_last records the address byte of a frame that passed CRC but
     * failed the address filter - that is the number the bench needs. */
    au64 bytes_in, frames_crc_ok, frames_crc_fail, resync_bytes;
    au64 gps_frames, pos_rejected, addr_rejected, len_rejected, overflow_drops;
    /* frames_crc_fail counts mismatches at an ALIGNED position - the next
     * frame boundary in an already-synchronised stream - which is the number
     * that means "line errors". probe_mismatch counts exploratory mismatches
     * while hunting for a frame boundary in junk, which is normal and must not
     * be read as line quality. The pre-fix parser conflated the two. */
    au64 probe_mismatch;
    au8  addr_reject_last;
} a_crsf_parser;

void a_crsf_init(a_crsf_parser *p);
/* Drop partial state but keep statistics. Call on detected stream restart. */
void a_crsf_resync(a_crsf_parser *p);
void a_crsf_set_addr_policy(a_crsf_parser *p, int policy);
void a_crsf_feed(a_crsf_parser *p, const au8 *data, ausize n);
au8  a_crsf_crc8(const au8 *p, ausize n);
int  a_crsf_addr_known(au8 a);
/* Pure unpack of a 22-byte RC_CHANNELS_PACKED payload into 16 x 11-bit channels
 * (little-endian bit order, matching caddx_crsf.decode_channels). Allocates nothing;
 * exposed directly so it can be unit-tested without feeding a whole frame. */
void a_crsf_unpack_channels(const au8 *pl22, au16 out[A_CRSF_RC_CHANNELS]);

#endif
