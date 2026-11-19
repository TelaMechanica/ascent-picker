/* ascent_mavlink.h — MAVLink v1/v2 byte-stream parser.
 *
 * Fixes carried here from the red-team review:
 *   F-MAV-1 (P1-5)  no CRC was computed at all, so any datagram containing
 *                   0xFE/0xFD produced waypoints. Now full CRC-16/MCRF4XX
 *                   with the per-message CRC_EXTRA; failures are rejected
 *                   and counted.
 *   F-MAV-2 (P0-3)  MAVLink v2 TRIM_PAYLOAD was not handled, so fixed-length
 *                   guards (n>=37 / n>=28) silently dropped legitimate
 *                   MISSION_ITEM_INT and every GLOBAL_POSITION_INT with
 *                   heading exactly 0. Payloads are now zero-extended to the
 *                   message's declared length before any field is read.
 *   F-MAV-3         `command` and `mission_type` were never examined, so
 *                   DO_ and CONDITION_ items and geofence/rally points were
 *                   rendered as navigation waypoints. Both are now filtered.
 *
 * MAVLink v1 compatibility is preserved deliberately: v1 does not trim, so v1
 * payloads are zero-extended only up to their own wire length, never beyond
 * what the sender actually framed.
 *
 * CRC_EXTRA values are from the MAVLink *common* dialect. A wrong entry fails
 * CLOSED - the message is rejected, never mis-parsed - and is counted in
 * per-message rejection statistics so a dialect mismatch is diagnosable on the
 * bench rather than invisible. See BENCH_TEST_PLAN.md step B4.
 *
 * Nothing here allocates. The reassembly buffer is fixed.
 */
#ifndef ASCENT_MAVLINK_H
#define ASCENT_MAVLINK_H
#include "ascent_types.h"

#define A_MAV_BUF          1024u   /* >= 3x the 280-byte maximum v2 frame */
#define A_MAX_PAYLOAD       255u
#define A_MAV_STX_V1       0xFEu
#define A_MAV_STX_V2       0xFDu

/* Messages this build understands. Everything else is framed and skipped. */
#define A_MSG_HEARTBEAT              0u
/* HEARTBEAT.base_mode bit 7 -- MAV_MODE_FLAG_SAFETY_ARMED. The authoritative
 * armed indication, and the only one this build trusts: CRSF carries a flight
 * mode string, not an armed flag, and inferring one from it would be guessing. */
#define A_MAV_MODE_FLAG_SAFETY_ARMED 0x80u
#define A_MSG_GPS_RAW_INT           24u
#define A_MSG_GLOBAL_POSITION_INT   33u
#define A_MSG_RC_CHANNELS_RAW       35u
#define A_MSG_RC_CHANNELS           65u
#define A_MSG_MISSION_ITEM          39u
#define A_MSG_MISSION_COUNT         44u
#define A_MSG_MISSION_CLEAR_ALL     45u
#define A_MSG_MISSION_ITEM_INT      73u

#define A_MISSION_TYPE_MISSION 0u
#define A_MISSION_TYPE_FENCE   1u
#define A_MISSION_TYPE_RALLY   2u

typedef struct {
    /* Position update. course_valid is 0 when the sender said "unknown"
     * (65535) so the consumer keeps its previous course instead of snapping. */
    void (*position)(void *user, double lat, double lon,
                     au16 course_cd, int course_valid,
                     au8 satellites, au8 fix_type, au32 msgid);
    /* Passive observation only - this build never transmits. */
    void (*mission_count)(void *user, au16 count, au8 mission_type);
    void (*mission_clear)(void *user, au8 mission_type);
    void (*mission_item)(void *user, au16 seq, au16 command, au8 frame,
                         au8 mission_type, double lat, double lon, float alt);
    /* HEARTBEAT armed bit. Called on EVERY heartbeat, armed or not, because the
     * consumer needs to know the state is FRESH as much as it needs the value:
     * a stale "armed" is indistinguishable from a live one otherwise, and the
     * map visibility policy must fall back to its heuristic rather than assume
     * armed. Optional, like the rest of this sink. */
    void (*armed)(void *user, int armed);
    void *user;
} a_mav_sink;

typedef struct {
    au8  buf[A_MAV_BUF];
    au32 n;
    int  synced;            /* set by a CRC-valid known message */

    /* RC is passive observation, never identity authentication. RAW port is
     * retained; fields are microseconds with UINT16_MAX/zero unavailable. */
    au16 rc[18];
    au8 rc_count, rc_sysid, rc_compid, rc_port;
    au32 rc_msgid;
    au64 rc_updates;
    au64 bytes_in, frames_v1, frames_v2, crc_ok, crc_fail;
    au64 unknown_msg, signed_frames, resync_bytes, overflow_drops;
    au64 item_rejected_frame, item_rejected_cmd, item_rejected_type;
    au64 pos_rejected;
    /* Per-message CRC rejections for the six messages we decode, so a wrong
     * CRC_EXTRA entry shows up as "msg N always fails" instead of silence. */
    au64 crc_fail_by_msg[8];
    au32 crc_fail_last_msgid;
} a_mav_parser;

void a_mav_init(a_mav_parser *p);
void a_mav_resync(a_mav_parser *p);
void a_mav_feed(a_mav_parser *p, const au8 *data, ausize n, const a_mav_sink *sink);

/* Exposed for the test harness so it can build correctly-CRCed frames using
 * the same arithmetic the parser validates with. */
au16 a_mav_crc(const au8 *buf, ausize n, au8 crc_extra);
int  a_mav_crc_extra(au32 msgid, au8 *out);
int  a_mav_msg_len(au32 msgid, au8 *out);
/* Navigation-command and frame predicates, exposed for tests. */
int  a_mav_frame_is_global(au8 frame);
int  a_mav_cmd_is_nav(au16 cmd);

#endif
