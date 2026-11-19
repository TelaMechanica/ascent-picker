/* ascent_picker_ctl.h — the Picker controller: the thin integration layer that routes BOTH
 * the headless RC path and ASCENT Control's manual SELECT through the ONE proven core
 * (ascent_cells for validity/PWM/activation + the switch engine for RF). It holds NO parallel
 * copy of activation/validity/PWM/switching logic -- it calls the core and records the core's
 * last result for status/OSD.
 *
 * Fail-closed, per the operator addendum:
 *   - config with < 2 active-valid cells  => DISABLED: no RC switch, manual SELECT refused,
 *     widget hidden (the host reads `enabled`), status carries the reason.
 *   - malformed config / no cells         => DISABLED.
 *   - out-of-range / stale RC sample      => hold, no switch.
 *   - a PWM value that stays in the current cell => never retriggers a switch.
 *
 * The actual RF switch is performed by the injected commit op (which runs a_switch_execute on
 * the device over loopback). The controller never opens a socket; it is host-testable with a
 * mock commit + clock. Sockets, the ttyS2 RC source, boot, OSD rendering and the IPC server
 * are thin adapters AROUND this controller.
 */
#ifndef ASCENT_PICKER_CTL_H
#define ASCENT_PICKER_CTL_H
#include "ascent_cells.h"
#include "ascent_switch.h"

#define A_PCTL_MAX_CELLS  A_CELLS_MAX_CFG

typedef enum {
    A_PCTL_OK = 0,
    A_PCTL_DISABLED_FEW_VALID,   /* < 2 active-valid cells */
    A_PCTL_DISABLED_MALFORMED    /* no/invalid config */
} a_pctl_reason;

typedef struct {
    void *ctx;
    /* Commit a switch to an exact target BbId; fills *out with the engine result. 0 on ok
       (even for a no-reacquire outcome -- that is a result, not a transport failure), <0 on a
       hard transport error. The controller NEVER switches directly. */
    int  (*commit)(void *ctx, const au8 target[4], a_sw_result *out);
    au64 (*now_ms)(void *ctx);
} a_pctl_ops;

typedef struct {
    a_cell_cfg cells[A_PCTL_MAX_CELLS];       /* configured bbid + valid(vsT2) */
    au8 recovery[A_SW_CANDIDATE_MAX][4];
    int recovery_n;
    au32 cooldown_ms, cooldown_remaining_ms; /* advisory only, never a gate */
    au64 cooldown_until_ms, rc_epoch;
    au32 last_switch_ms, last_acquire_ms;
    int rc_protocol, rc_pwm_us;
    int rc_fresh;
    int rc_enabled;
    int rc_channel;
    int  n_cfg;
    int  active_idx[A_PCTL_MAX_CELLS];        /* indices into cells[], low->high order */
    int  active_n;                            /* N */
    int  enabled;                             /* active_n >= 2 */
    a_pctl_reason reason;

    au32 hold_stable_ms;                      /* debounce (on top of cell hysteresis) */
    int  cur_cell;                            /* 0-based active-cell index, or A_CELL_INVALID */
    au8  cur_target[4];
    int  has_target;                         /* verified identity only */
    int verified_cell;
    int busy;
    int quiesced; /* lifecycle barrier, cleared only by process initialization */
    int faulted; /* unresolved transport/cleanup failure; ordinary recovered miss is not a lock */
    au8 commanded_target[4];
    int has_command;
    a_sw_result running;

    /* debounce tracking */
    int  pending_cell;
    au64 pending_since;

    /* last committed engine result -- REPORTED, not parallel state */
    int  have_last;
    a_sw_outcome last_outcome;
    a_sw_phase   last_phase;
} a_pctl;

void a_pctl_update_time(a_pctl *c, au64 now_ms);

void a_pctl_init(a_pctl *c, au32 hold_stable_ms);
int a_pctl_quiesce(a_pctl *c);

/* (Re)load config: configured BbIds + this VRX's current T2 membership. Recomputes validity
   (bbid in T2), active-valid set and `enabled`. Resets runtime cell/debounce state. Returns
   `enabled`. Fails closed (enabled=0 + reason) on malformed/<2. Handles T2 changes (§11):
   call again after a reconciliation and the engine disables itself if it drops below 2. */
int a_pctl_set_config(a_pctl *c, const au8 cfg_bbids[][4], int n_cfg,
                      const au8 t2[][4], int t2_n);

/* Feed one RC PWM sample. Returns 1 iff a switch was committed (fills *res with the engine
   result), 0 otherwise (disabled / same cell / debouncing / out-of-range / startup-establish).
   Startup (no prior cell) ESTABLISHES the current cell without switching (addendum §12). */
/* Source epoch changes establish a new RC position without issuing RF. */
int a_pctl_on_source(a_pctl *c, const a_pctl_ops *ops, int pwm_us, int valid, au64 epoch, a_sw_result *res);

int a_pctl_on_rc(a_pctl *c, const a_pctl_ops *ops, int pwm_us, int valid, a_sw_result *res);

/* Manual SELECT by 0-based active-cell index -- same engine path as RC. Returns 1 if committed,
   0 if refused (disabled / bad index / already on that target). */
int a_pctl_select_cell(a_pctl *c, const a_pctl_ops *ops, int active_cell, a_sw_result *res);

/* Manual SELECT by exact BbId (must be active-valid on this receiver). Returns 1 if committed. */
int a_pctl_select_bbid(a_pctl *c, const a_pctl_ops *ops, const au8 bbid[4], a_sw_result *res);

#endif
