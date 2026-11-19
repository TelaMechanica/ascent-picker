/* ascent_switch.h — the ONE VRX-local Picker switch engine.
 *
 * This is the single runtime switching state machine (headless-engine invariant #1,#3,#9):
 * both the headless RC path and ASCENT Control's manual SELECT submit a target to THIS engine;
 * there is no second switch implementation on the host side. The engine owns the whole
 * transaction and exposes its phase/outcome outward.
 *
 * It reproduces the hardware-PROVEN fast sequence (docs/PICKER_HEADLESS_ARCHITECTURE_SYNTHESIS):
 *   standby-ON leaving -> SET_CANDIDATES 0x02000004 narrow-to-target-only (release+select)
 *   -> MEASURED drop gate -> [transient re-arm] -> acquire to stable CONNECT -> standby-OFF joined.
 * The tens-of-seconds host-wrapper waits are gone; timing is bounded by the real RF.
 *
 * Invariants enforced here (locked by the operator):
 *  #1 NO_DROP is a first-class failure: the engine NEVER proceeds to re-arm/acquire unless the
 *     old link actually left CONNECT within the measured-drop budget.
 *  #2 The 0x02000002 pair re-arm is retained PROVISIONALLY and isolated behind a single op
 *     (ops->pair_rearm) gated by cfg.rearm_enabled, so a bench proof can remove it cleanly.
 *  #3 The engine owns the transaction; the host reads state, it does not run a second machine.
 *  #4 standby-OFF (un-park the joined unit) occurs ONLY after a verified stable CONNECT.
 *  #5 Recovery widen is ONE-SHOT and explicit; there is never a blind retry (overheat safety).
 *
 * Identity model: the live peer BbId is NOT readable on a normal link (proven), so SUCCESS is a
 * stable CONNECT on the target-narrowed T1 -- the narrow itself is the identity proof. No peer
 * read-back gates success.
 *
 * Pure + host-testable: all device I/O is injected through a_sw_ops; nothing here allocates or
 * opens a socket. The runtime binds a_sw_ops to the real loopback RPC (50000) + standby (2767);
 * tests bind a mock. Frame builders (a_usbpack_build, a_sw_*_frame) are byte-exact ports of the
 * proven host builders (caddx_relay_qos.build_frame / caddx_picker_link / caddx_picker) and are
 * cross-validated against them.
 */
#ifndef ASCENT_SWITCH_H
#define ASCENT_SWITCH_H
#include "ascent_types.h"

/* usbpack RPC command words (AR8030, TCP 127.0.0.1:50000). */
#define A_SW_CMD_GET_STATUS     0x01000000u
#define A_SW_CMD_GET_PAIR       0x01000001u
#define A_SW_CMD_PAIR_MODE      0x02000002u
#define A_SW_CMD_SET_CANDIDATE  0x02000004u

#define A_SW_CANDIDATE_MAX      100
#define A_SW_CANDIDATE_PAYLEN   (2 + A_SW_CANDIDATE_MAX * 4)   /* 402 */
#define A_SW_USBPACK_HDR        18
#define A_SW_GUI_SET_STANDBY    56                             /* 0x38, TCP 127.0.0.1:2767 */

/* STATUS link-state byte @252 (0x01000000). 3-state cross-build. */
#define A_SW_LINK_IDLE     0
#define A_SW_LINK_LOCK     1
#define A_SW_LINK_CONNECT  2

/* Observation ceiling, not an enforced delay. Return immediately after stable
 * CONNECT. Keep the established SOP's 40-second watch instead of aborting a
 * legitimate acquisition at the native adapter's former 12-second cutoff. */
#define A_SW_ACQUIRE_WINDOW_MS 40000u

/* ---- frame builders (pure; return total bytes written, or -1 if out too small) ---- */
au8 a_sw_checksum17(const au8 *h);                /* ~XOR(h[0..16]) & 0xFF */
int a_usbpack_build(au32 cmd, au32 meta1, au32 meta2,
                    const au8 *payload, au32 paylen, au8 *out, au32 outcap);
int a_sw_candidate_frame(const au8 macs[][4], int count, au8 *out, au32 outcap);
int a_sw_pair_frame(int enter, au8 *out, au32 outcap);          /* enter!=0 => ENTER else EXIT */
int a_sw_status_frame(au32 work_id, au8 *out, au32 outcap);
int a_sw_standby_frame(int mode, int sub_chn, int seq, au8 *out, au32 outcap); /* GUI 2767 */

/* ---- the state machine ---- */
typedef enum {
    A_SW_IDLE = 0, A_SW_COMMAND, A_SW_RELEASING, A_SW_REARM,
    A_SW_ACQUIRING, A_SW_VERIFIED, A_SW_FAILED
} a_sw_phase;

typedef enum {
    A_SW_OUT_PENDING = 0,
    A_SW_OUT_SWITCHED,       /* stable CONNECT on the target-narrowed T1 */
    A_SW_OUT_NO_DROP,        /* old link never left CONNECT -> invariant #1 */
    A_SW_OUT_NO_REACQUIRE,   /* dropped, but no stable CONNECT within the window */
    A_SW_OUT_REFUSED,        /* bad target (sentinel/zero) -- no RF issued */
    A_SW_OUT_EXEC_ERR        /* a transport op failed */
} a_sw_outcome;

/* Injected device transport. Every call returns 0 on success, <0 on transport error.
   read_link fills *state with A_SW_LINK_*. Keeps the engine host-testable. */
typedef struct {
    void *ctx;
    int  (*send_standby)(void *ctx, int mode);                    /* 1=ON(park) 0=OFF(unpark) */
    int  (*set_candidates)(void *ctx, const au8 macs[][4], int count); /* 0x02000004 */
    int  (*pair_rearm)(void *ctx);                                /* ISOLATED re-arm (#2) */
    int  (*read_link)(void *ctx, int *state);
    au64 (*now_ms)(void *ctx);
    void (*sleep_ms)(void *ctx, au32 ms);
} a_sw_ops;

typedef struct {
    au8  target[4];
    au8  whitelist[A_SW_CANDIDATE_MAX][4];   /* full T2 set, for the one-shot recovery widen */
    int  whitelist_n;
    int  park_enabled;        /* standby park/unpark around the switch */
    int  rearm_enabled;       /* #2: pair re-arm on/off (one flag removes it after bench B) */
    au32 drop_wait_ms;        /* measured-drop budget; exceeded => NO_DROP */
    au32 connect_stable_ms;   /* CONNECT must hold this long to count as verified */
    au32 acquire_window_ms;   /* max wait for a stable CONNECT */
    au32 poll_ms;             /* link poll interval */
    au32 work_id;             /* usbpack radio index for STATUS reads (primary Ground = 0) */
} a_sw_cfg;

typedef struct {
    a_sw_outcome outcome;
    a_sw_phase   phase;       /* furthest phase reached */
    int  dropped;             /* measured drop observed */
    int  rearmed;             /* pair re-arm invoked */
    int  recovered;           /* one-shot recovery widen performed */
    int  recovery_ok;         /* Ground cleanup completed, standby restore sent; not peer proof */
    int  standby_on_sent;
    int  standby_off_sent;
    au32 acquire_ms;          /* time from narrow to stable CONNECT (on success) */
    int  polls;
} a_sw_result;

/* Run ONE switch transaction to completion. Synchronous (uses ops->now_ms/sleep_ms so tests
   drive a fake clock). The caller serializes transactions and owns the cooldown -- the engine
   performs exactly one attempt and never loops. Returns result->outcome. */
a_sw_outcome a_switch_execute(const a_sw_cfg *cfg, const a_sw_ops *ops, a_sw_result *result);

/* True if a BbId is a usable target (not all-zero, not all-0xFF). */
int a_sw_target_valid(const au8 mac[4]);

#endif
