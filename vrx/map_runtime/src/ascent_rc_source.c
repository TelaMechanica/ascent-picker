/* Passive ttyS2 RX mirror, using the existing shared CRSF/MAVLink decoders. */
#include "ascent_rc_source.h"
#include "ascent_sys.h"
static void feed_sink(void *user, const au8 *p, ausize n) {
    a_rc_source *s=user;
    a_aircraft_rc_feed(&s->decoded,p,n,a_now_ms());
}
static void reset_input(a_rc_source *s) {
    a_aircraft_rc_reset(&s->decoded); s->protocol=0; s->epoch++;
}
void a_rc_source_init(a_rc_source *s, const char *path) {
    a_memset(s,0,sizeof(*s)); a_ring_reader_init(&s->rx,path); reset_input(s);
}
int a_rc_source_attach(a_rc_source *s) {
    if(!a_ring_reader_attach(&s->rx))return 0;
    if(s->writer_epoch && s->writer_epoch!=s->rx.epoch)reset_input(s);
    s->writer_epoch=s->rx.epoch; return 1;
}
int a_rc_source_poll(a_rc_source *s,int channel,int *pwm_us,au32 *age_ms) {
    au64 now=a_now_ms(); *pwm_us=0; if(age_ms)*age_ms=0xffffffffu;
    if((s->last_poll_ms && now-s->last_poll_ms>A_RC_STALE_MS) ||
       (s->rx.hdr && s->rx.hdr->wpos-s->rx.rpos>4096)) {
        int rewound=s->rx.hdr && s->rx.hdr->wpos<s->rx.rpos;
        a_ring_reader_detach(&s->rx);
        /* Our own synchronous RF wait invalidates samples, not controller identity.
         * Retain the source epoch unless writer/protocol/sender really changes. */
        if(rewound)reset_input(s);else a_aircraft_rc_reset(&s->decoded);
    }
    s->last_poll_ms=now;
    if(!s->rx.hdr && !a_rc_source_attach(s)) return 0;
    int restarted=0; a_ring_read(&s->rx,4096,&restarted,feed_sink,s);
    if(restarted) { reset_input(s); s->writer_epoch=s->rx.epoch; return 0; }
    if(channel<1 || channel>16) return 0;
    now=a_now_ms(); int protocol=0, value=0; au64 age=0;
    if(s->decoded.crsf_seen && now-s->decoded.crsf_at<=A_RC_STALE_MS) {
        protocol=1; age=now-s->decoded.crsf_at;
        value=a_rc_tick_to_us(s->decoded.crsf.rc.ch[channel-1]);
    } else if(s->decoded.mav_seen && now-s->decoded.mav_at<=A_RC_STALE_MS) {
        a_mav_parser *m=&s->decoded.mav;
        /* RAW port groups are not silently relabelled as global channels. */
        if(m->rc_port!=0 || channel>m->rc_count || m->rc[channel-1]==0 || m->rc[channel-1]==65535u) return 0;
        protocol=2; age=now-s->decoded.mav_at; value=m->rc[channel-1];
    } else return 0;
    if(protocol!=s->protocol || (protocol==2 &&
        (s->sysid!=s->decoded.mav.rc_sysid || s->compid!=s->decoded.mav.rc_compid))) {
        s->epoch++; s->protocol=protocol;
        s->sysid=s->decoded.mav.rc_sysid; s->compid=s->decoded.mav.rc_compid;
    }
    if(value<800 || value>2200) return 0;
    *pwm_us=value; if(age_ms)*age_ms=(au32)age; return 1;
}
