/* Production RC adapter must never relabel buffered history as a fresh input. */
#include "atest.h"
#include "frames.h"
#include "../src/ascent_rc_source.h"
#include "../src/ascent_sys.h"
int main(void) {
    SUITE("Picker local RC freshness");
    char path[128]; snprintf(path,sizeof path,"/tmp/ascent-picker-rc-%d.ring",getpid());
    a_ring_writer writer; CHECK(a_ring_writer_open(&writer,path,1),"writer attaches");
    a_rc_source source; a_rc_source_init(&source,path); CHECK(a_rc_source_attach(&source),"reader attaches");
    int pwm; au16 ch[16]; for(int i=0;i<16;i++) ch[i]=992;
    au8 frame[32]; int n=mk_crsf_rc(frame,0xC8,ch); au32 age;
    a_ring_write(&writer,frame,n);
    CHECK(a_rc_source_poll(&source,5,&pwm,&age),"new RC frame is fresh");
    CHECK_EQI(pwm,1500,"channel preserved");
    ch[4]=1811; n=mk_crsf_rc(frame,0xC8,ch); a_ring_write(&writer,frame,n);
    au64 same_source_epoch=source.epoch;
    source.last_poll_ms=a_now_ms()-600;
    CHECK(!a_rc_source_poll(&source,5,&pwm,&age),"gap discards old queued RC instead of refreshing it");
    a_ring_write(&writer,frame,n);
    CHECK(a_rc_source_poll(&source,5,&pwm,&age),"new input after gap is accepted");
    CHECK_EQI(pwm,a_rc_tick_to_us(1811),"fresh new position decoded");
    CHECK_EQI(source.epoch,same_source_epoch,"post-switch stale discard does not invent a new source or swallow the next RC move");
    /* Writer restart invalidates the last sample even inside its old TTL. */
    a_ring_writer_bind(&writer,writer.hdr,2);
    CHECK(!a_rc_source_poll(&source,5,&pwm,&age),"writer restart invalidates cached RC");

    au8 pl[64], mf[256]; memset(pl,0,sizeof pl);
    for(int j=0;j<18;j++){pl[4+j*2]=0xdc;pl[5+j*2]=5;} pl[40]=18;
    pl[16]=0x29;pl[17]=7; /* CH7=1833 us, not slot7 and not CRSF ticks. */
    n=mk_mav_v2(mf,1,42,1,A_MSG_RC_CHANNELS,pl,42,0);
    a_ring_write(&writer,mf,n);
    CHECK(a_rc_source_poll(&source,7,&pwm,&age),"MAV65 CH7 accepted through actual ring consumer");
    CHECK_EQI(pwm,1833,"MAV microseconds preserved");
    CHECK_EQI(source.protocol,2,"MAV source is identified");
    au64 epoch=source.epoch;
    n=mk_mav_v2(mf,2,43,1,A_MSG_RC_CHANNELS,pl,42,0);a_ring_write(&writer,mf,n);
    CHECK(a_rc_source_poll(&source,7,&pwm,&age),"new MAV sender observed");
    CHECK(source.epoch>epoch,"sender change requires new RC establishment");
    mf[n-1]^=1;source.decoded.mav_at=a_now_ms()-501;a_ring_write(&writer,mf,n);
    CHECK(!a_rc_source_poll(&source,7,&pwm,&age),"bad CRC cannot refresh stale MAV");
    /* Independent RAW source epoch: do not reuse a corrupt/incomplete framing candidate. */
    a_ring_writer_bind(&writer,writer.hdr,3);a_rc_source_poll(&source,7,&pwm,&age);
    memset(pl,0,sizeof pl);pl[16]=0xdc;pl[17]=5;
    n=mk_mav_v1(mf,3,42,1,A_MSG_RC_CHANNELS_RAW,pl,22);a_ring_write(&writer,mf,n);
    CHECK(a_rc_source_poll(&source,7,&pwm,&age),"RAW35 port0 CH7 available");
    CHECK_EQI(pwm,1500,"RAW35 units preserved");
    CHECK(!a_rc_source_poll(&source,9,&pwm,&age),"RAW35 cannot invent CH9");
    pl[20]=1;n=mk_mav_v1(mf,4,42,1,A_MSG_RC_CHANNELS_RAW,pl,22);a_ring_write(&writer,mf,n);
    CHECK(!a_rc_source_poll(&source,7,&pwm,&age),"other RAW ports cannot masquerade as CH7");
    pl[20]=0;pl[16]=255;pl[17]=255;n=mk_mav_v1(mf,5,42,1,A_MSG_RC_CHANNELS_RAW,pl,22);a_ring_write(&writer,mf,n);
    CHECK(!a_rc_source_poll(&source,7,&pwm,&age),"unavailable MAV channel rejected");
    for(int j=0;j<16;j++)ch[j]=992;
    n=mk_crsf_rc(frame,0xc8,ch);a_ring_write(&writer,frame,n);
    CHECK(a_rc_source_poll(&source,7,&pwm,&age),"CRSF remains accepted");
    CHECK_EQI(source.protocol,1,"fresh CRSF takes deterministic precedence");
    CHECK_EQI(pwm,1500,"CRSF ticks converted exactly once");

    memset(pl,0,sizeof pl);for(int j=0;j<18;j++){pl[4+j*2]=0x29;pl[5+j*2]=7;}pl[40]=18;
    n=mk_mav_v2(mf,6,42,1,A_MSG_RC_CHANNELS,pl,42,0);a_ring_write(&writer,mf,n);
    CHECK(a_rc_source_poll(&source,7,&pwm,&age),"both valid protocols accepted without mixing");
    CHECK_EQI(pwm,1500,"fresh CRSF wins over simultaneous valid MAV");
    source.decoded.crsf_at=a_now_ms()-501;
    CHECK(a_rc_source_poll(&source,7,&pwm,&age),"stale CRSF permits fresh MAV fallback");
    CHECK_EQI(pwm,1833,"fallback preserves MAV microseconds");
    a_ring_writer_bind(&writer,writer.hdr,4);a_rc_source_poll(&source,7,&pwm,&age);
    a_ring_write(&writer,mf,7);
    CHECK(!a_rc_source_poll(&source,7,&pwm,&age),"partial MAV cannot create a current channel");
    a_ring_write(&writer,mf+7,n-7);
    CHECK(a_rc_source_poll(&source,7,&pwm,&age),"fragmented MAV completes through actual ring consumer");
    CHECK(!a_rc_source_poll(&source,17,&pwm,&age),"outside configured 16-channel contract refused");
    a_ring_reader_detach(&source.rx); a_ring_writer_close(&writer); unlink(path);
    return atest_report();
}
