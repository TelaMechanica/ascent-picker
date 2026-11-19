/* ascent_msp.h — MSP v1 byte-stream parser, MSP_RAW_GPS (msg 106) only.
 *
 * A THIRD own-ship position source for the map engine, used as a FALLBACK when
 * neither MAVLink nor CRSF has a fresh fix. It exists so the glasses can place
 * own-ship from the FC's MSP/OSD telemetry when no CRSF/MAVLink GPS is present.
 *
 * Deliberately modelled on ascent_crsf.h (poll idiom): the last accepted fix is
 * stored in `gps` and `gps_updates` increments only on acceptance, so the engine
 * detects "a new fix" without guessing from timestamps. Every rejection path is
 * counted; nothing is silent.
 *
 * MSP v1 framing differs from CRSF and MAVLink and must not be copied from them:
 *   bytes: '$'(0x24) 'M'(0x4D) dir('<'0x3C or '>'0x3E) size(u8) cmd(u8)
 *          payload[size] crc(u8)
 *   crc  : XOR of size ^ cmd ^ every payload byte  (NOT the CRSF CRC8 poly,
 *          NOT the MAVLink CRC-16)
 *   wire : LITTLE-endian  (CRSF GPS is big-endian)
 * FC telemetry arrives as responses (dir '>'); requests ('<') are accepted too
 * so the parser is direction-agnostic for a mirrored stream.
 *
 * MSP_RAW_GPS payload (Cleanflight/Betaflight v1, little-endian, >= 16 bytes):
 *   0      u8  fix
 *   1      u8  numSat
 *   2..5   i32 lat  degree * 1e7
 *   6..9   i32 lon  degree * 1e7
 *   10..11 u16 altitude  metres          (NO +1000 offset, unlike CRSF)
 *   12..13 u16 speed     cm/s            (CRSF carried km/h*10)
 *   14..15 u16 ground course  DECI-degrees (0.1 deg) -> stored as centidegrees
 *
 * Assembly buffer is a FIXED 256 bytes; input of any size is consumed in bounded
 * chunks. Nothing here allocates. Compiled both freestanding (AArch64 ship) and
 * hosted (tests), so no libc.
 */
#ifndef ASCENT_MSP_H
#define ASCENT_MSP_H
#include "ascent_types.h"

#define A_MSP_BUF         256u
#define A_MSP_DOLLAR      0x24u   /* '$' */
#define A_MSP_M           0x4Du   /* 'M' */
#define A_MSP_DIR_REQ     0x3Cu   /* '<' request (to FC)   */
#define A_MSP_DIR_RSP     0x3Eu   /* '>' response (from FC) */
#define A_MSP_RAW_GPS     106u    /* MSP_RAW_GPS command id */
#define A_MSP_RAW_GPS_MIN  16u    /* minimum payload bytes we decode */
#define A_MSP_MAX_PAYLOAD 250u    /* bounds a claimed frame within the buffer */

typedef struct {
    double lat, lon;      /* WGS84 degrees */
    au16   course_cd;     /* centidegrees 0..35999 (MSP deci-deg * 10) */
    au16   speed_cms;     /* cm/s, as carried on the wire */
    as32   altitude_m;    /* metres (MSP carries metres directly) */
    au8    satellites;
    au8    fix;
} a_msp_gps;

typedef struct {
    au8  buf[A_MSP_BUF];
    au32 n;

    /* Last accepted position. gps_updates increments only on acceptance. */
    a_msp_gps gps;
    au64 gps_updates;

    /* Observability. Every rejection path is counted. frames_crc_fail counts
     * mismatches at an ALIGNED boundary (real line errors); probe_mismatch
     * counts exploratory mismatches while hunting in junk (normal). */
    au64 bytes_in, frames_crc_ok, frames_crc_fail, resync_bytes;
    au64 gps_frames, pos_rejected, len_rejected, overflow_drops, probe_mismatch;
} a_msp_parser;

void a_msp_init(a_msp_parser *p);
/* Drop partial state but keep statistics. Call on detected stream restart. */
void a_msp_resync(a_msp_parser *p);
void a_msp_feed(a_msp_parser *p, const au8 *data, ausize n);
/* XOR checksum over n bytes (the test builder uses it on size+cmd+payload). */
au8  a_msp_crc(const au8 *p, ausize n);

#endif
