/* ascent_picker_ctl.c — Picker controller. See ascent_picker_ctl.h. Thin: calls ascent_cells
 * and the injected engine commit; holds no duplicated activation/PWM/switching logic. */
#include "ascent_picker_ctl.h"

static int bb_eq(const au8 a[4], const au8 b[4])
{ return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3]; }

static int in_t2(const au8 bb[4], const au8 t2[][4], int t2_n)
{ for (int i=0;i<t2_n;i++) if (bb_eq(bb, t2[i])) return 1; return 0; }

void a_pctl_init(a_pctl *c, au32 hold_stable_ms)
{
    a_memset(c, 0, sizeof(*c));
    c->hold_stable_ms = hold_stable_ms;
    c->cur_cell = A_CELL_INVALID;
    c->verified_cell = A_CELL_INVALID;
    c->pending_cell = A_CELL_INVALID;
    c->reason = A_PCTL_DISABLED_MALFORMED;
}

void a_pctl_update_time(a_pctl *c,au64 now_ms) {
    au64 left=c->cooldown_until_ms>now_ms ? c->cooldown_until_ms-now_ms : 0;
    c->cooldown_remaining_ms=(au32)(left>300000 ? 300000 : left);
}

int a_pctl_quiesce(a_pctl *c) {
    if(c->busy || c->faulted) return 0;
    c->quiesced=1;
    return 1;
}

int a_pctl_set_config(a_pctl *c, const au8 cfg_bbids[][4], int n_cfg,
                      const au8 t2[][4], int t2_n)
{
    /* Identical effective config/T2 validity must not reset live state. */
    int same = cfg_bbids && n_cfg == c->n_cfg;
    for (int i = 0; same && i < n_cfg; i++)
        if (!bb_eq(cfg_bbids[i], c->cells[i].bbid) ||
            c->cells[i].valid != (a_sw_target_valid(cfg_bbids[i]) && t2 && in_t2(cfg_bbids[i], t2, t2_n))) same = 0;
    if (same) {
        c->recovery_n = t2_n < A_SW_CANDIDATE_MAX ? t2_n : A_SW_CANDIDATE_MAX;
        if(c->recovery_n>0) a_memcpy(c->recovery,t2,(ausize)c->recovery_n*4);
        return c->enabled;
    }
    au32 hold = c->hold_stable_ms;
    int quiesced=c->quiesced;
    a_pctl_init(c, hold);
    c->quiesced=quiesced;
    if (!cfg_bbids || n_cfg <= 0) { c->reason = A_PCTL_DISABLED_MALFORMED; return 0; }
    if (n_cfg > A_PCTL_MAX_CELLS) n_cfg = A_PCTL_MAX_CELLS;

    c->recovery_n = t2_n < A_SW_CANDIDATE_MAX ? t2_n : A_SW_CANDIDATE_MAX;
    if(c->recovery_n>0 && t2) a_memcpy(c->recovery,t2,(ausize)c->recovery_n*4);
    c->n_cfg = n_cfg;
    for (int i = 0; i < n_cfg; i++) {
        a_memcpy(c->cells[i].bbid, cfg_bbids[i], 4);
        /* validity is defined ONLY as "exact bbid in THIS VRX T2" (never nickname/cache). */
        c->cells[i].valid = (a_sw_target_valid(cfg_bbids[i]) && t2 && in_t2(cfg_bbids[i], t2, t2_n))
                            ? 1 : 0;
    }
    c->active_n = a_cells_active_valid(c->cells, c->n_cfg, c->active_idx, A_PCTL_MAX_CELLS);
    c->enabled  = a_cells_picker_active(c->active_n);
    c->reason   = c->enabled ? A_PCTL_OK : A_PCTL_DISABLED_FEW_VALID;
    return c->enabled;
}

/* Run the engine via the injected commit, record the result, update cur_cell/target. */
static int do_commit(a_pctl *c, const a_pctl_ops *ops, int active_cell,
                     const au8 target[4], a_sw_result *res)
{
    if (c->busy || c->faulted) return 0;
    c->busy = 1;
    c->rc_fresh=0; c->rc_pwm_us=0; /* A synchronous handoff invalidates the input snapshot. */
    c->last_switch_ms=0; c->last_acquire_ms=0;
    c->cooldown_until_ms=0; c->cooldown_remaining_ms=0;
    au64 began=ops->now_ms ? ops->now_ms(ops->ctx) : 0;
    c->has_target = 0; c->verified_cell = A_CELL_INVALID;
    c->has_command = 1;
    a_memcpy(c->commanded_target, target, 4);
    a_memset(&c->running, 0, sizeof c->running);
    c->running.phase = A_SW_COMMAND;
    int rc = ops->commit(ops->ctx, target, &c->running);
    c->last_switch_ms=ops->now_ms ? (au32)(ops->now_ms(ops->ctx)-began) : 0;
    c->last_acquire_ms=c->running.acquire_ms;
    if (rc < 0) { c->running.outcome = A_SW_OUT_EXEC_ERR; c->running.phase = A_SW_FAILED; }
    c->have_last = 1; c->last_outcome = c->running.outcome; c->last_phase = c->running.phase;
    /* Any RF attempt invalidates prior identity. Recovery may link any candidate. */
    c->has_target = 0; c->verified_cell = A_CELL_INVALID;
    if (c->running.outcome == A_SW_OUT_SWITCHED && c->running.phase == A_SW_VERIFIED) {
        a_memcpy(c->cur_target, target, 4); c->has_target = 1;
        c->verified_cell = active_cell;
    }
    if (res) *res = c->running;
    c->pending_cell = A_CELL_INVALID;
    /* A missing/unpowered target is an ordinary failed request, not a global
     * RC lock. Once cleanup is acknowledged, latch the attempted RC position
     * (below) and accept a NEW deliberate pick; never retry the held position.
     * Unknown transport/cleanup failures still cannot authorize another RF op. */
    c->faulted = !c->has_target && !((c->last_outcome == A_SW_OUT_NO_REACQUIRE ||
        c->last_outcome == A_SW_OUT_NO_DROP) && c->running.recovery_ok);
    c->busy = 0;
    c->cooldown_until_ms=(c->has_target && ops->now_ms) ? ops->now_ms(ops->ctx)+c->cooldown_ms : 0;
    a_pctl_update_time(c,ops->now_ms ? ops->now_ms(ops->ctx) : 0);
    return 1;
}

int a_pctl_on_source(a_pctl *c,const a_pctl_ops *ops,int pwm_us,int valid,au64 epoch,a_sw_result *res) {
    if(c->rc_epoch!=epoch) {
        c->rc_epoch=epoch; c->pending_cell=A_CELL_INVALID; c->cur_cell=A_CELL_INVALID;
    }
    return a_pctl_on_rc(c,ops,pwm_us,valid,res);
}

int a_pctl_on_rc(a_pctl *c, const a_pctl_ops *ops, int pwm_us, int valid, a_sw_result *res)
{
    if (!c->enabled || c->busy || c->quiesced) return 0;                         /* fail closed: no RC switch when <2 */
    if (!valid) { c->pending_cell = A_CELL_INVALID; return 0; }                              /* stale/failsafe sample: hold */

    int cell = a_cells_classify(pwm_us, c->n_cfg, c->cur_cell);
    if (cell == A_CELL_INVALID) { c->pending_cell = A_CELL_INVALID; return 0; }              /* out-of-range: hold, no switch */

    if (c->cur_cell == A_CELL_INVALID) {
        /* Startup: ESTABLISH the current cell without switching (addendum §12): a stable
         * startup sample must not fire a switch on its own. Debounced (set-then-check, so
         * hold_stable_ms==0 establishes on the first sample). */
        if (c->pending_cell != cell) { c->pending_cell = cell; c->pending_since = ops->now_ms(ops->ctx); }
        if (ops->now_ms(ops->ctx) - c->pending_since < c->hold_stable_ms) return 0;
        c->cur_cell = cell; c->pending_cell = A_CELL_INVALID;
        return 0;                                      /* established, no switch */
    }

    if (cell == c->cur_cell) { c->pending_cell = A_CELL_INVALID; return 0; } /* same cell: never retrigger */

    /* a different cell: debounce before committing (on top of the cell hysteresis). */
    if (c->pending_cell != cell) { c->pending_cell = cell; c->pending_since = ops->now_ms(ops->ctx); }
    if (ops->now_ms(ops->ctx) - c->pending_since < c->hold_stable_ms) return 0;

    if (!c->cells[cell].valid) {
        c->cur_cell=cell; c->pending_cell=A_CELL_INVALID; return 0;
    }
    const au8 *target = c->cells[cell].bbid;
    if (c->has_target && bb_eq(target,c->cur_target)) {
        c->cur_cell=cell; c->pending_cell=A_CELL_INVALID; return 0;
    }
    int committed=do_commit(c,ops,cell,target,res);
    if(committed) c->cur_cell=cell; /* RC input latch is independent of manual SELECT */
    return committed;
}

int a_pctl_select_cell(a_pctl *c, const a_pctl_ops *ops, int active_cell, a_sw_result *res)
{
    if (!c->enabled || c->busy || c->quiesced) return 0;                               /* fail closed */
    if (active_cell < 0 || active_cell >= c->active_n) return 0;
    const au8 *target = c->cells[c->active_idx[active_cell]].bbid;
    if (c->has_target && bb_eq(target, c->cur_target)) return 0; /* already on it: no retrigger */
    return do_commit(c, ops, c->active_idx[active_cell], target, res);
}

int a_pctl_select_bbid(a_pctl *c, const a_pctl_ops *ops, const au8 bbid[4], a_sw_result *res)
{
    if (!c->enabled || c->busy || c->quiesced) return 0;                               /* fail closed */
    for (int k = 0; k < c->active_n; k++) {
        int ci = c->active_idx[k];
        if (bb_eq(c->cells[ci].bbid, bbid)) {               /* must be an active-valid target */
            if (c->has_target && bb_eq(bbid, c->cur_target)) return 0;
            return do_commit(c, ops, ci, c->cells[ci].bbid, res);
        }
    }
    return 0;                                               /* not active-valid: refuse */
}
