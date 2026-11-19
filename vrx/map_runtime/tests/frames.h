/* frames.h — TEST-SIDE builders for CRSF and MAVLink frames.
 *
 * These are input generators, not production code. They deliberately use the
 * production CRC routines (a_crsf_crc8, a_mav_crc) so that a test frame is
 * accepted for the same arithmetic reason the device would accept it. That
 * means a CRC test proves the parser's framing and validation logic; it does
 * NOT independently prove the CRC_EXTRA table values, which come from the
 * MAVLink common dialect. See PROVEN_STATE.md.
 */
#ifndef ATEST_FRAMES_H
#define ATEST_FRAMES_H
#include "../src/ascent_crsf.h"
#include "../src/ascent_mavlink.h"
#include "../src/ascent_msp.h"

/* ---- CRSF ---- */
static inline int mk_crsf_gps(au8 *f, au8 addr, as32 lat_e7, as32 lon_e7,
                              au16 gs_dkmh, au16 hdg_cd, au16 alt_off, au8 sats)
{
    f[0] = addr; f[1] = 17; f[2] = A_CRSF_TYPE_GPS;
    au8 *p = f + 3;
    p[0]=(au8)(lat_e7>>24); p[1]=(au8)(lat_e7>>16); p[2]=(au8)(lat_e7>>8); p[3]=(au8)lat_e7;
    p[4]=(au8)(lon_e7>>24); p[5]=(au8)(lon_e7>>16); p[6]=(au8)(lon_e7>>8); p[7]=(au8)lon_e7;
    p[8]=(au8)(gs_dkmh>>8); p[9]=(au8)gs_dkmh;
    p[10]=(au8)(hdg_cd>>8); p[11]=(au8)hdg_cd;
    p[12]=(au8)(alt_off>>8); p[13]=(au8)alt_off;
    p[14]=sats;
    f[18] = a_crsf_crc8(f + 2, 16);
    return 19;
}
/* Build an RC_CHANNELS_PACKED (0x16) frame from 16 channel values (11-bit each,
 * 172..1811 nominal). Packs LSB-first into 22 bytes -- the exact inverse of the
 * production a_crsf_unpack_channels -- so a round-trip test proves the unpack. */
static inline int mk_crsf_rc(au8 *f, au8 addr, const au16 ch[16])
{
    f[0] = addr; f[1] = 24; f[2] = A_CRSF_TYPE_RC_CHANNELS;
    au8 *p = f + 3;
    au32 bits = 0; int nbits = 0, dst = 0;
    for (int c = 0; c < 16; c++) {
        bits |= (au32)(ch[c] & 0x7FFu) << nbits; nbits += 11;
        while (nbits >= 8) { p[dst++] = (au8)(bits & 0xFF); bits >>= 8; nbits -= 8; }
    }
    /* 16*11 = 176 bits = 22 bytes exactly; nbits is 0 here. */
    f[3 + 22] = a_crsf_crc8(f + 2, 23);   /* CRC over type+payload = len-1 = 23 */
    return 26;
}
/* An arbitrary non-GPS CRSF frame (link statistics shaped), correctly CRCed. */
static inline int mk_crsf_other(au8 *f, au8 addr, au8 type, au8 paylen)
{
    f[0]=addr; f[1]=(au8)(paylen+2); f[2]=type;
    for (au8 i=0;i<paylen;i++) f[3+i]=(au8)(0x10+i);
    f[3+paylen]=a_crsf_crc8(f+2,(ausize)(paylen+1));
    return paylen+4;
}

/* ---- MSP v1 (MSP_RAW_GPS) ---- */
/* Build a `$M>` MSP_RAW_GPS frame. course is in DECI-degrees on the wire, as a
 * real FC sends it; alt is metres, speed cm/s. Uses the production XOR checksum
 * so the frame is accepted for the same arithmetic reason the device would. */
static inline int mk_msp_raw_gps(au8 *f, au8 dir, au8 fix, au8 sats,
                                 as32 lat_e7, as32 lon_e7,
                                 au16 alt_m, au16 speed_cms, au16 course_ddeg)
{
    f[0] = A_MSP_DOLLAR; f[1] = A_MSP_M; f[2] = dir;
    f[3] = 16;                      /* payload size */
    f[4] = A_MSP_RAW_GPS;           /* cmd */
    au8 *p = f + 5;
    p[0] = fix; p[1] = sats;
    p[2]=(au8)lat_e7;    p[3]=(au8)(lat_e7>>8);  p[4]=(au8)(lat_e7>>16);  p[5]=(au8)(lat_e7>>24);
    p[6]=(au8)lon_e7;    p[7]=(au8)(lon_e7>>8);  p[8]=(au8)(lon_e7>>16);  p[9]=(au8)(lon_e7>>24);
    p[10]=(au8)alt_m;        p[11]=(au8)(alt_m>>8);
    p[12]=(au8)speed_cms;    p[13]=(au8)(speed_cms>>8);
    p[14]=(au8)course_ddeg;  p[15]=(au8)(course_ddeg>>8);
    f[21] = a_msp_crc(f + 3, 18);   /* XOR over size, cmd, 16 payload bytes */
    return 22;                      /* 6 + 16 */
}
/* An arbitrary non-GPS MSP frame (e.g. MSP_STATUS shaped), correctly checksummed. */
static inline int mk_msp_other(au8 *f, au8 dir, au8 cmd, au8 paylen)
{
    f[0]=A_MSP_DOLLAR; f[1]=A_MSP_M; f[2]=dir; f[3]=paylen; f[4]=cmd;
    for (au8 i=0;i<paylen;i++) f[5+i]=(au8)(0x10+i);
    f[5+paylen]=a_msp_crc(f+3,(ausize)(paylen+2));
    return paylen+6;
}

/* ---- MAVLink ---- */
static inline int mk_mav_v1(au8 *f, au8 seq, au8 sysid, au8 compid,
                            au8 msgid, const au8 *pl, au8 L)
{
    au8 extra; if (!a_mav_crc_extra(msgid, &extra)) extra = 0;
    f[0]=A_MAV_STX_V1; f[1]=L; f[2]=seq; f[3]=sysid; f[4]=compid; f[5]=msgid;
    for (au8 i=0;i<L;i++) f[6+i]=pl[i];
    au16 c = a_mav_crc(f+1, (ausize)(5+L), extra);
    f[6+L]=(au8)(c & 0xFF); f[7+L]=(au8)(c >> 8);
    return 8+L;
}
static inline int mk_mav_v2(au8 *f, au8 seq, au8 sysid, au8 compid,
                            au32 msgid, const au8 *pl, au8 L, int signed_frame)
{
    au8 extra; if (!a_mav_crc_extra(msgid, &extra)) extra = 0;
    f[0]=A_MAV_STX_V2; f[1]=L; f[2]=signed_frame?1:0; f[3]=0; f[4]=seq;
    f[5]=sysid; f[6]=compid;
    f[7]=(au8)(msgid & 0xFF); f[8]=(au8)((msgid>>8)&0xFF); f[9]=(au8)((msgid>>16)&0xFF);
    for (au8 i=0;i<L;i++) f[10+i]=pl[i];
    au16 c = a_mav_crc(f+1, (ausize)(9+L), extra);
    f[10+L]=(au8)(c & 0xFF); f[11+L]=(au8)(c >> 8);
    int total = 12+L;
    if (signed_frame) { for (int i=0;i<13;i++) f[total+i]=(au8)(0xA0+i); total += 13; }
    return total;
}
/* MAVLink v2 trailing-zero trimming, exactly as a conforming sender does it.
 * At least one payload byte is always kept. */
static inline au8 mav_trim(const au8 *pl, au8 L)
{
    while (L > 1 && pl[L-1] == 0) L--;
    return L;
}
/* Build a MISSION_ITEM_INT payload (37 base + mission_type at 37). */
static inline void mk_mission_item_int(au8 *pl, au16 seq, au16 cmd, au8 frame,
                                       as32 lat_e7, as32 lon_e7, float alt,
                                       au8 current, au8 autocont, au8 mtype)
{
    for (int i=0;i<38;i++) pl[i]=0;
    *(as32*)(pl+16)=lat_e7; *(as32*)(pl+20)=lon_e7; *(float*)(pl+24)=alt;
    pl[28]=(au8)(seq&0xFF); pl[29]=(au8)(seq>>8);
    pl[30]=(au8)(cmd&0xFF); pl[31]=(au8)(cmd>>8);
    pl[32]=1; pl[33]=1; pl[34]=frame; pl[35]=current; pl[36]=autocont; pl[37]=mtype;
}
static inline void mk_mission_item(au8 *pl, au16 seq, au16 cmd, au8 frame,
                                   float lat, float lon, float alt,
                                   au8 current, au8 autocont, au8 mtype)
{
    for (int i=0;i<38;i++) pl[i]=0;
    *(float*)(pl+16)=lat; *(float*)(pl+20)=lon; *(float*)(pl+24)=alt;
    pl[28]=(au8)(seq&0xFF); pl[29]=(au8)(seq>>8);
    pl[30]=(au8)(cmd&0xFF); pl[31]=(au8)(cmd>>8);
    pl[32]=1; pl[33]=1; pl[34]=frame; pl[35]=current; pl[36]=autocont; pl[37]=mtype;
}
static inline void mk_gps_raw_int(au8 *pl, as32 lat_e7, as32 lon_e7, au16 cog_cd,
                                  au8 fix, au8 sats)
{
    for (int i=0;i<52;i++) pl[i]=0;
    *(as32*)(pl+8)=lat_e7; *(as32*)(pl+12)=lon_e7;
    pl[26]=(au8)(cog_cd&0xFF); pl[27]=(au8)(cog_cd>>8);
    pl[28]=fix; pl[29]=sats;
}
static inline void mk_global_pos_int(au8 *pl, as32 lat_e7, as32 lon_e7, au16 hdg_cd)
{
    for (int i=0;i<28;i++) pl[i]=0;
    *(as32*)(pl+4)=lat_e7; *(as32*)(pl+8)=lon_e7;
    pl[26]=(au8)(hdg_cd&0xFF); pl[27]=(au8)(hdg_cd>>8);
}
#endif
