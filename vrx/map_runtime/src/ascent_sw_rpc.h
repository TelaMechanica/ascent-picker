/* ascent_sw_rpc.h — bind the switch engine's a_sw_ops to the real VRX loopback transports:
 *   - usbpack RPC  TCP 127.0.0.1:50000  (SET_CANDIDATES, PAIR, STATUS)
 *   - standby      TCP 127.0.0.1:2767   (GUI_CMD_SET_STANDBY_MODE)
 * Thin device adapter: it only moves the a_sw_*_frame bytes the proven engine builds and reads
 * the STATUS link byte back. Envelope/offsets match the proven host reader and are exercised
 * by host socket fixtures. Live RF and device service availability remain bench-gated. */
#ifndef ASCENT_SW_RPC_H
#define ASCENT_SW_RPC_H
#include "ascent_switch.h"

#define A_SW_RPC_PORT      50000
#define A_SW_STANDBY_PORT  2767
#define A_SW_PAIR_DWELL_MS 50       /* proven pair-dwell */

typedef struct { au32 work_id; void (*service)(void *); void *service_ctx;
#ifdef ASCENT_CLASSIC_PAIR_RELEASE
    au8 requested_target[4];
    int has_requested_target, verify_peer;
    int pair_entered; /* exact classic profile: pending transient release, never another owner */
#endif
} a_sw_rpc_ctx;

/* Populate `ops` so a_switch_execute drives the real loopback transports. */
void a_sw_rpc_bind(a_sw_ops *ops, a_sw_rpc_ctx *ctx, au32 work_id);

#endif
