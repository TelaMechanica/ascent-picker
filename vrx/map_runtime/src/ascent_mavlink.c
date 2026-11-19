#include "ascent_mavlink.h"
#include "ascent_geo.h"

/* CRC-16/MCRF4XX, the "X.25" CRC MAVLink uses. Identical accumulation to the
 * reference crc_accumulate(). */
static void crc_acc(au8 data, au16 *acc)
{
    au8 tmp = (au8)(data ^ (au8)(*acc & 0xFF));
    tmp = (au8)(tmp ^ (au8)(tmp << 4));
    *acc = (au16)((*acc >> 8) ^ ((au16)tmp << 8) ^ ((au16)tmp << 3) ^ ((au16)tmp >> 4));
}

au16 a_mav_crc(const au8 *buf, ausize n, au8 crc_extra)
{
    au16 acc = 0xFFFF;
    for (ausize i = 0; i < n; i++) crc_acc(buf[i], &acc);
    crc_acc(crc_extra, &acc);
    return acc;
}

/* msgid -> (CRC_EXTRA, declared payload length incl. extension fields).
 * MAVLink common dialect. See the header note on fail-closed behaviour. */
int a_mav_crc_extra(au32 msgid, au8 *out)
{
    au8 v;
    switch (msgid) {
    case A_MSG_HEARTBEAT:            v = 50;  break;
    case A_MSG_RC_CHANNELS_RAW:      v = 244; break;
    case A_MSG_RC_CHANNELS:          v = 118; break;
    case A_MSG_GPS_RAW_INT:          v = 24;  break;
    case A_MSG_GLOBAL_POSITION_INT:  v = 104; break;
    case A_MSG_MISSION_ITEM:         v = 254; break;
    case A_MSG_MISSION_COUNT:        v = 221; break;
    case A_MSG_MISSION_CLEAR_ALL:    v = 232; break;
    case A_MSG_MISSION_ITEM_INT:     v = 38;  break;
    default: return 0;
    }
    if (out) *out = v;
    return 1;
}

int a_mav_msg_len(au32 msgid, au8 *out)
{
    au8 v;
    switch (msgid) {
    case A_MSG_HEARTBEAT:            v = 9;  break;
    case A_MSG_RC_CHANNELS_RAW:      v = 22; break;
    case A_MSG_RC_CHANNELS:          v = 42; break;
    case A_MSG_GPS_RAW_INT:          v = 52; break; /* 30 base + extensions   */
    case A_MSG_GLOBAL_POSITION_INT:  v = 28; break;
    case A_MSG_MISSION_ITEM:         v = 38; break; /* 37 base + mission_type */
    case A_MSG_MISSION_COUNT:        v = 5;  break; /* 4 base  + mission_type */
    case A_MSG_MISSION_CLEAR_ALL:    v = 3;  break; /* 2 base  + mission_type */
    case A_MSG_MISSION_ITEM_INT:     v = 38; break; /* 37 base + mission_type */
    default: return 0;
    }
    if (out) *out = v;
    return 1;
}

static int msg_slot(au32 msgid)
{
    switch (msgid) {
    case A_MSG_HEARTBEAT:           return 0;
    case A_MSG_GPS_RAW_INT:         return 1;
    case A_MSG_GLOBAL_POSITION_INT: return 2;
    case A_MSG_MISSION_ITEM:        return 3;
    case A_MSG_MISSION_COUNT:       return 4;
    case A_MSG_MISSION_CLEAR_ALL:   return 5;
    case A_MSG_MISSION_ITEM_INT:    return 6;
    default:                        return 7;
    }
}

/* MAV_FRAME values that carry a global lat/lon. */
int a_mav_frame_is_global(au8 f)
{
    return f == 0 || f == 3 || f == 5 || f == 6 || f == 10 || f == 11;
}

/* MAV_CMD values that place the vehicle at a coordinate. DO_* and CONDITION_*
 * items reuse x/y for unrelated parameters and must not become map waypoints. */
int a_mav_cmd_is_nav(au16 c)
{
    return (c >= 16 && c <= 31) || (c >= 82 && c <= 85);
}

void a_mav_init(a_mav_parser *p) { a_memset(p, 0, sizeof(*p)); }
void a_mav_resync(a_mav_parser *p) { p->n = 0; p->synced = 0; }

static void dispatch(a_mav_parser *p, au32 msgid, const au8 *pl, const a_mav_sink *s)
{
    if (!s) return;
    switch (msgid) {
    case A_MSG_HEARTBEAT: {
        /* custom_mode u32 @0, type @4, autopilot @5, base_mode @6. The frame
         * reached here only after a CRC check against this message's own
         * CRC_EXTRA, so the length is the declared one and offset 6 is real. */
        if (s->armed) s->armed(s->user, (pl[6] & A_MAV_MODE_FLAG_SAFETY_ARMED) ? 1 : 0);
        break;
    }
    case A_MSG_GPS_RAW_INT: {
        as32 lat = a_sle32(pl + 8), lon = a_sle32(pl + 12);
        au16 cog = a_le16(pl + 26);
        au8  fix = pl[28], sats = pl[29];
        double dlat = (double)lat / 1e7, dlon = (double)lon / 1e7;
        if (fix < 2 || !a_sane_ll(dlat, dlon)) { p->pos_rejected++; break; }
        if (s->position) s->position(s->user, dlat, dlon, (au16)(cog % 36000u),
                                     cog != 65535, sats, fix, msgid);
        break;
    }
    case A_MSG_GLOBAL_POSITION_INT: {
        as32 lat = a_sle32(pl + 4), lon = a_sle32(pl + 8);
        au16 hdg = a_le16(pl + 26);
        double dlat = (double)lat / 1e7, dlon = (double)lon / 1e7;
        if (!a_sane_ll(dlat, dlon)) { p->pos_rejected++; break; }
        if (s->position) s->position(s->user, dlat, dlon, (au16)(hdg % 36000u),
                                     hdg != 65535, 0, 3, msgid);
        break;
    }
    case A_MSG_MISSION_COUNT:
        if (s->mission_count) s->mission_count(s->user, a_le16(pl), pl[4]);
        break;
    case A_MSG_MISSION_CLEAR_ALL:
        if (s->mission_clear) s->mission_clear(s->user, pl[2]);
        break;
    case A_MSG_MISSION_ITEM:
    case A_MSG_MISSION_ITEM_INT: {
        au16 seq = a_le16(pl + 28), cmd = a_le16(pl + 30);
        au8  frame = pl[34], mtype = pl[37];
        double lat, lon; float alt;
        if (msgid == A_MSG_MISSION_ITEM_INT) {
            lat = (double)a_sle32(pl + 16) / 1e7;
            lon = (double)a_sle32(pl + 20) / 1e7;
        } else {
            lat = (double)a_lef32(pl + 16);
            lon = (double)a_lef32(pl + 20);
        }
        alt = a_lef32(pl + 24);
        if (!a_mav_frame_is_global(frame)) { p->item_rejected_frame++; break; }
        if (!a_mav_cmd_is_nav(cmd))        { p->item_rejected_cmd++;   break; }
        if (mtype != A_MISSION_TYPE_MISSION) { p->item_rejected_type++; break; }
        if (s->mission_item)
            s->mission_item(s->user, seq, cmd, frame, mtype, lat, lon, alt);
        break;
    }
    default: break;
    }
}

/* Try to decode one frame at buf[i]. Returns:
 *   >0  frame consumed, this many bytes
 *    0  need more data (caller must wait, not advance)
 *   -1  not a frame here (caller advances one byte) */
static int try_frame(a_mav_parser *p, au32 i, const a_mav_sink *sink)
{
    const au8 *b = p->buf;
    au32 avail = p->n - i;
    au8  stx = b[i];
    au32 hdr, L, siglen = 0, msgid, total;

    if (stx == A_MAV_STX_V1) {
        if (avail < 8) return 0;
        hdr = 6; L = b[i + 1]; msgid = b[i + 5];
    } else if (stx == A_MAV_STX_V2) {
        if (avail < 12) return 0;
        hdr = 10; L = b[i + 1];
        if (b[i + 2] & 0x01) siglen = 13;         /* MAVLINK_IFLAG_SIGNED */
        msgid = (au32)b[i + 7] | ((au32)b[i + 8] << 8) | ((au32)b[i + 9] << 16);
    } else {
        return -1;
    }
    total = hdr + L + 2 + siglen;
    if (avail < total) return 0;

    au8 extra;
    if (!a_mav_crc_extra(msgid, &extra)) {
        /* Unknown message. We cannot validate it. While synced we trust the
         * framing and skip it; while unsynced we refuse to skip, because a
         * false start would hide a real frame behind it. */
        p->unknown_msg++;
        return p->synced ? (int)total : -1;
    }

    au16 want = a_mav_crc(b + i + 1, hdr - 1 + L, extra);
    au16 got  = (au16)((au16)b[i + hdr + L] | ((au16)b[i + hdr + L + 1] << 8));
    if (want != got) {
        p->crc_fail++;
        p->crc_fail_by_msg[msg_slot(msgid)]++;
        p->crc_fail_last_msgid = msgid;
        p->synced = 0;
        return -1;                                 /* never trust the length */
    }

    p->crc_ok++;
    p->synced = 1;
    if (stx == A_MAV_STX_V1) p->frames_v1++; else p->frames_v2++;
    if (siglen) p->signed_frames++;

    /* Zero-extend. MAVLink v2 removes trailing zero bytes (TRIM_PAYLOAD), so
     * the wire length L may be shorter than the message. v1 never trims, but
     * zero-extending a v1 payload is harmless because the sender guaranteed
     * the full length. Reading a field beyond L therefore yields the zero the
     * sender omitted - which is exactly the pre-fix defect (F-MAV-2). */
    au8 pl[A_MAX_PAYLOAD];
    au32 copy = L < A_MAX_PAYLOAD ? L : A_MAX_PAYLOAD;
    a_memcpy(pl, b + i + hdr, copy);
    a_memset(pl + copy, 0, (ausize)(A_MAX_PAYLOAD - copy));
    /* Every field offset read by dispatch() is < 40, so the 255-byte
     * zero-extended buffer covers all of them unconditionally. */

    /* Official common dialect generated headers: RAW35 has 8 u16 @4,
     * port @20, CRC_EXTRA244, len22; RC65 has 18 u16 @4, count @40,
     * CRC_EXTRA118, len42. v2 omitted trailing zeros remain zero. */
    if ((msgid == A_MSG_RC_CHANNELS || msgid == A_MSG_RC_CHANNELS_RAW) &&
        (stx == A_MAV_STX_V2 || L == (msgid == A_MSG_RC_CHANNELS ? 42u : 22u))) {
        au8 count = msgid == A_MSG_RC_CHANNELS ? pl[40] : 8;
        au8 limit = msgid == A_MSG_RC_CHANNELS ? 18 : 8;
        if (count > limit) count = limit;
        for (au8 j = 0; j < 18; j++)
            p->rc[j] = j < count ? a_le16(pl + 4u + 2u*j) : 65535u;
        p->rc_count = count;
        p->rc_port = msgid == A_MSG_RC_CHANNELS_RAW ? pl[20] : 0;
        p->rc_sysid = b[i + (stx == A_MAV_STX_V2 ? 5 : 3)];
        p->rc_compid = b[i + (stx == A_MAV_STX_V2 ? 6 : 4)];
        p->rc_msgid = msgid;
        p->rc_updates++;
    }
    dispatch(p, msgid, pl, sink);
    return (int)total;
}

static void parse_buffered(a_mav_parser *p, const a_mav_sink *sink)
{
    au32 i = 0;
    while (i < p->n) {
        au8 c = p->buf[i];
        if (c != A_MAV_STX_V1 && c != A_MAV_STX_V2) { i++; p->resync_bytes++; continue; }
        int r = try_frame(p, i, sink);
        if (r == 0) break;                 /* incomplete, keep from here */
        if (r < 0) { i++; p->resync_bytes++; continue; }
        i += (au32)r;
    }
    if (i) {
        au32 left = p->n - i;
        for (au32 k = 0; k < left; k++) p->buf[k] = p->buf[i + k];
        p->n = left;
    }
}

void a_mav_feed(a_mav_parser *p, const au8 *data, ausize n, const a_mav_sink *sink)
{
    if (!p || !data) return;
    p->bytes_in += n;
    while (n) {
        if (p->n >= A_MAV_BUF) {
            /* Only reachable if a candidate header claims a frame longer than
             * the buffer can ever hold, which the 280-byte maximum prevents.
             * Kept so forward progress is unconditional. */
            for (au32 k = 1; k < p->n; k++) p->buf[k - 1] = p->buf[k];
            p->n--; p->overflow_drops++; p->resync_bytes++;
        }
        ausize space = A_MAV_BUF - p->n;
        ausize take  = n < space ? n : space;
        a_memcpy(p->buf + p->n, data, take);
        p->n += (au32)take; data += take; n -= take;
        parse_buffered(p, sink);
    }
}
