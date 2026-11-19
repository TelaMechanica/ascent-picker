/* Passive controller observation from the existing ttyS2 RX mirror only.
 * One stream, protocol-distinct records. No UART/socket/zoom executor. */
#ifndef ASCENT_AIRCRAFT_RC_H
#define ASCENT_AIRCRAFT_RC_H
#include "ascent_crsf.h"
#include "ascent_mavlink.h"
typedef struct {
    a_crsf_parser crsf;
    a_mav_parser mav;
    au64 crsf_at, mav_at, epoch;
    int crsf_seen, mav_seen;
} a_aircraft_rc;
static inline void a_aircraft_rc_reset(a_aircraft_rc *s) {
    au64 epoch = s->epoch + 1;
    a_memset(s, 0, sizeof(*s)); s->epoch = epoch;
    a_crsf_init(&s->crsf); a_mav_init(&s->mav);
}
static inline void a_aircraft_rc_feed(a_aircraft_rc *s, const au8 *p, ausize n, au64 now) {
    au64 c = s->crsf.rc_updates, m = s->mav.rc_updates;
    a_crsf_feed(&s->crsf, p, n); a_mav_feed(&s->mav, p, n, 0);
    if (s->crsf.rc_updates != c) { s->crsf_at = now; s->crsf_seen = 1; }
    if (s->mav.rc_updates != m) { s->mav_at = now; s->mav_seen = 1; }
}
#endif
