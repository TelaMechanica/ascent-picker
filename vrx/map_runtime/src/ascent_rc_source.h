/* Passive VRX ttyS2 RX mirror -> shared CRSF/MAVLink decoders -> one selected channel.
 * No UART owner, stream request or RF executor. Missing/stale selected-channel
 * evidence holds selection. Actual RX traffic and switching remain bench-gated. */
#ifndef ASCENT_RC_SOURCE_H
#define ASCENT_RC_SOURCE_H
#include "ascent_types.h"
#include "ascent_ring.h"
#include "ascent_aircraft_rc.h"

#define A_RC_STALE_MS 500u      /* older than this => stale/failsafe (hold) */

typedef struct {
    a_ring_reader rx;
    a_aircraft_rc decoded;
    au64 last_poll_ms, epoch, writer_epoch;
    int protocol;              /* 0 unavailable, 1 CRSF, 2 MAVLink */
    au8 sysid, compid;

} a_rc_source;

/* CRSF tick (172..1811) -> PWM microseconds (standard mapping: 992 tick = 1500 us). */
static inline int a_rc_tick_to_us(au16 tick) { return ((int)tick - 992) * 5 / 8 + 1500; }

void a_rc_source_init(a_rc_source *s, const char *rx_ring_path);
int  a_rc_source_attach(a_rc_source *s);
/* One passive ring reader. Prefer fresh CRSF; otherwise fresh MAVLink RC65 or
 * RAW35 port0. No channel mixing, no MAVLink stream requests or UART ownership.
 * channel is 1-based (1..16); missing/invalid selected channel fails closed.
 * A protocol/sender/ring reset changes epoch so caller re-establishes RC without RF. */
int a_rc_source_poll(a_rc_source *s, int channel, int *pwm_us, au32 *age_ms);

#endif
