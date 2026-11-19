#include "ascent_crsf.h"
#include "ascent_geo.h"

au8 a_crsf_crc8(const au8 *p, ausize n)
{
    au8 crc = 0;
    for (ausize i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x80) ? (au8)((crc << 1) ^ 0xD5) : (au8)(crc << 1);
    }
    return crc;
}

/* Documented CRSF device addresses. Kept as one table so the bench can widen
 * it from a single place if the live ttyS2 stream proves to use another. */
int a_crsf_addr_known(au8 a)
{
    switch (a) {
    case 0x00: /* broadcast            */
    case 0xC8: /* flight controller    */
    case 0xEA: /* radio transmitter    */
    case 0xEC: /* CRSF receiver        */
    case 0xEE: /* CRSF transmitter     */
        return 1;
    default:
        return 0;
    }
}

void a_crsf_init(a_crsf_parser *p)
{
    a_memset(p, 0, sizeof(*p));
    p->addr_policy = A_CRSF_ADDR_STRICT;
}

void a_crsf_resync(a_crsf_parser *p) { p->n = 0; }

void a_crsf_set_addr_policy(a_crsf_parser *p, int policy)
{
    p->addr_policy = (au8)(policy == A_CRSF_ADDR_ANY ? A_CRSF_ADDR_ANY
                                                     : A_CRSF_ADDR_STRICT);
}

static void gps_from_payload(a_crsf_parser *p, au8 addr, const au8 *pl)
{
    /* CRSF GPS payload, 15 bytes, all big-endian:
     *   0..3  int32  latitude   degree * 1e7
     *   4..7  int32  longitude  degree * 1e7
     *   8..9  uint16 groundspeed km/h * 10
     *  10..11 uint16 heading    degree * 100   (centidegrees)
     *  12..13 uint16 altitude   metres + 1000
     *  14     uint8  satellites                                          */
    double lat = (double)a_sbe32(pl)     / 1e7;
    double lon = (double)a_sbe32(pl + 4) / 1e7;

    p->gps_frames++;
    if (!a_sane_ll(lat, lon)) { p->pos_rejected++; return; }

    p->gps.lat              = lat;
    p->gps.lon              = lon;
    p->gps.groundspeed_dkmh = a_be16(pl + 8);
    p->gps.course_cd        = (au16)(a_be16(pl + 10) % 36000u);
    p->gps.altitude_m       = (as32)a_be16(pl + 12) - 1000;
    p->gps.satellites       = pl[14];
    p->gps.addr             = addr;
    p->gps_updates++;
}

/* Unpack 16 x 11-bit little-endian channels from the 22-byte RC_CHANNELS_PACKED
 * payload. Mirrors runtime/caddx_crsf.py decode_channels exactly: a rolling LSB-first
 * bit accumulator, 176 bits = 22 bytes consumed, 11 bits per channel, masked to 0x7FF.
 * Pure and bounded; nothing allocates. */
void a_crsf_unpack_channels(const au8 *pl22, au16 out[A_CRSF_RC_CHANNELS])
{
    au32 bits = 0;
    int  nbits = 0, src = 0;
    for (int ch = 0; ch < (int)A_CRSF_RC_CHANNELS; ch++) {
        while (nbits < 11) { bits |= (au32)pl22[src++] << nbits; nbits += 8; }
        out[ch] = (au16)(bits & 0x7FFu);
        bits >>= 11; nbits -= 11;
    }
}

static void rc_from_payload(a_crsf_parser *p, au8 addr, const au8 *pl)
{
    /* RC_CHANNELS_PACKED payload is exactly 22 bytes (16 x 11 bits). Accepted
     * unconditionally once CRC + address + length have passed: unlike GPS there is
     * no out-of-range sanity reject here -- raw ticks are passed up and the picker's
     * own deadband/hysteresis decides validity. rc_updates marks a fresh frame. */
    p->rc_frames++;
    a_crsf_unpack_channels(pl, p->rc.ch);
    p->rc.addr = addr;
    p->rc_updates++;
}

/* Scan the buffer for complete, CRC-valid frames.
 *
 * An incomplete candidate does NOT stop the scan. A byte that merely looks
 * like a frame start - address plus a plausible length byte - can claim up to
 * 64 bytes that have not arrived yet; stopping there would block every real
 * frame queued behind it until enough bytes happened to arrive. A complete,
 * CRC-valid frame found later in the buffer is far stronger evidence than an
 * incomplete candidate earlier, so the scan continues and, on acceptance,
 * discards everything before the accepted frame.
 *
 * Retention on return is bounded: an incomplete candidate can only sit within
 * the last 64 bytes (it needs i + 2 + L > n with L <= 62), so at most 64 bytes
 * are carried forward. */
static void parse_buffered(a_crsf_parser *p)
{
    au32 i = 0, consumed = 0, keep = 0;
    int  have_keep = 0;

    while (i + 4 <= p->n) {
        au8  addr = p->buf[i];
        au32 L    = p->buf[i + 1];

        if (L < A_CRSF_MIN_LEN || L > A_CRSF_MAX_LEN) {
            p->len_rejected++; p->resync_bytes++; i++; continue;
        }
        if (i + 2 + L > p->n) {                 /* incomplete: remember, keep going */
            if (!have_keep) { keep = i; have_keep = 1; }
            i++; continue;
        }
        if (a_crsf_crc8(p->buf + i + 2, L - 1) != p->buf[i + 1 + L]) {
            if (i == consumed) p->frames_crc_fail++;   /* aligned: real error */
            else               p->probe_mismatch++;    /* hunting in junk     */
            p->resync_bytes++; i++; continue;
        }
        /* CRC is good. Now the address filter (F-CRSF-1). A CRC-valid frame
         * with an unknown address is recorded rather than silently dropped,
         * because that number is what the bench needs to confirm the policy. */
        if (p->addr_policy == A_CRSF_ADDR_STRICT && !a_crsf_addr_known(addr)) {
            p->addr_rejected++; p->addr_reject_last = addr;
            p->resync_bytes++; i++; continue;
        }

        p->frames_crc_ok++;
        if (p->buf[i + 2] == A_CRSF_TYPE_GPS && L >= 17)
            gps_from_payload(p, addr, p->buf + i + 3);
        /* RC_CHANNELS_PACKED: len = type(1) + payload(22) + crc(1) = 24. The 22
         * channel bytes sit at buf+i+3 (after addr, len, type). */
        else if (p->buf[i + 2] == A_CRSF_TYPE_RC_CHANNELS && L >= 24)
            rc_from_payload(p, addr, p->buf + i + 3);

        i += 2 + L;
        consumed = i;
        have_keep = 0;            /* everything before here is now junk */
    }

    /* Decide what to carry forward. */
    au32 start;
    if (have_keep && keep >= consumed) {
        start = keep;
    } else {
        /* No pending candidate. Fewer than 4 bytes cannot be judged, so keep
         * at most the last 3 as a possible frame prefix. */
        start = (p->n >= 3) ? p->n - 3 : 0;
        if (start < consumed) start = consumed;
    }
    if (start > p->n) start = p->n;
    if (start) {
        au32 left = p->n - start;
        for (au32 k = 0; k < left; k++) p->buf[k] = p->buf[start + k];
        p->n = left;
    }
}

void a_crsf_feed(a_crsf_parser *p, const au8 *data, ausize n)
{
    if (!p || !data) return;
    p->bytes_in += n;

    while (n) {
        if (p->n >= A_CRSF_BUF) {
            /* Unreachable while A_CRSF_BUF > max frame, because parse_buffered
             * leaves at most 64 bytes. Kept as a hard guarantee of forward
             * progress rather than an assertion that could wedge the engine. */
            for (au32 k = 1; k < p->n; k++) p->buf[k - 1] = p->buf[k];
            p->n--; p->overflow_drops++; p->resync_bytes++;
        }
        ausize space = A_CRSF_BUF - p->n;
        ausize take  = n < space ? n : space;
        a_memcpy(p->buf + p->n, data, take);
        p->n += (au32)take;
        data += take;
        n    -= take;
        parse_buffered(p);
    }
}
