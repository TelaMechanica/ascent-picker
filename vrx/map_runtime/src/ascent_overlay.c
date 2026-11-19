#include "ascent_overlay.h"

/* ========================================================================== *
 * Target selection
 * ========================================================================== */

void a_ovl_init(a_ovl_policy *p, au32 stable_ticks, au32 min_w, au32 min_h,
                au32 rebuild_cap)
{
    if (!p) return;
    a_memset(p, 0, sizeof(*p));
    p->stable_ticks = stable_ticks ? stable_ticks : A_OVL_STABLE_DEF;
    p->min_w        = min_w ? min_w : A_OVL_MIN_W_DEF;
    p->min_h        = min_h ? min_h : A_OVL_MIN_H_DEF;
    p->rebuild_cap  = rebuild_cap ? rebuild_cap : A_OVL_CAP_DEF;
}

a_ovl_action a_ovl_tick(a_ovl_policy *p, void *cand, int cw, int ch)
{
    if (!p) return A_OVL_IDLE;
    p->ticks++;
    p->adopt = ASCENT_NULL;

    /* ---- no active window ------------------------------------------------
     * Do NOT draw: if this is NULL because the window was destroyed, the
     * overlay went with it and touching it is the use-after-free this design
     * exists to prevent. Do NOT rebuild either: a transition produces a NULL
     * for a tick or two and rebuilding on every one of them is the churn that
     * shows up as flicker. Only a SUSTAINED NULL is treated as a real loss. */
    if (!cand) {
        p->null_ticks++;
        if (p->null_ticks == 1) p->null_gaps++;
        if (p->have && p->null_ticks >= p->stable_ticks) {
            p->have = 0;
            p->current = ASCENT_NULL;
            p->lost_windows++;
        }
        p->pending = ASCENT_NULL;
        p->pending_ticks = 0;
        p->idles++;
        return A_OVL_IDLE;
    }
    p->null_ticks = 0;

    /* ---- popup filter -----------------------------------------------------
     * A QMenu, tooltip or combo popup is a top-level widget and can become the
     * active window. It is far smaller than a page. Rejecting it by geometry
     * keeps the overlay exactly where it is, which is why opening a menu now
     * costs zero rebuilds and zero leaked widgets. */
    if (cw < 0 || ch < 0 || (au32)cw < p->min_w || (au32)ch < p->min_h) {
        p->rej_small++;
        p->pending = ASCENT_NULL;
        p->pending_ticks = 0;
        if (p->have) { p->keeps++; return A_OVL_KEEP; }
        p->idles++;
        return A_OVL_IDLE;
    }

    /* ---- already attached to this window ---------------------------------- */
    if (p->have && cand == p->current) {
        p->pending = ASCENT_NULL;
        p->pending_ticks = 0;
        p->keeps++;
        return A_OVL_KEEP;
    }

    /* ---- a different window: require persistence ---------------------------
     * Focus can flick to another window and straight back. Adopting on the
     * first sighting turns that into two rebuilds; requiring the candidate to
     * survive stable_ticks turns it into none. */
    if (cand != p->pending) {
        p->pending = cand;
        p->pending_ticks = 1;
    } else {
        p->pending_ticks++;
    }
    if (p->pending_ticks < p->stable_ticks) {
        p->rej_unstable++;
        if (p->have) { p->keeps++; return A_OVL_KEEP; }
        p->idles++;
        return A_OVL_IDLE;
    }

    /* ---- hard cap ---------------------------------------------------------
     * Abandoning a QLabel leaks it when its parent is still alive, and we
     * cannot safely find out whether it is. The leak is therefore bounded by
     * refusing to build past the cap: the overlay stops updating and the
     * counters say why. Failing invisible beats both a crash and an unbounded
     * leak in the pilot-facing process. */
    if (p->builds >= p->rebuild_cap) {
        p->capped_events++;
        p->idles++;
        return A_OVL_IDLE;
    }

    p->adopt = cand;
    return A_OVL_BUILD;
}

void a_ovl_built(a_ovl_policy *p)
{
    if (!p || !p->adopt) return;
    p->current = p->adopt;
    p->adopt = ASCENT_NULL;
    p->have = 1;
    p->builds++;
    p->pending = ASCENT_NULL;
    p->pending_ticks = 0;
}

void a_ovl_build_failed(a_ovl_policy *p)
{
    if (!p) return;
    /* The candidate is not adopted and `have` is not set, so the next tick
     * starts observing again from scratch. A failing build must not consume
     * the rebuild budget, or a transient allocation failure would permanently
     * disable the overlay. */
    p->adopt = ASCENT_NULL;
    p->have = 0;
    p->current = ASCENT_NULL;
    p->pending = ASCENT_NULL;
    p->pending_ticks = 0;
}

/* ========================================================================== *
 * Repaint decision
 * ========================================================================== */

void a_paint_init(a_paint_policy *p)
{
    if (!p) return;
    a_memset(p, 0, sizeof(*p));
    p->state = A_PSTATE_INIT;
}

a_paint_kind a_paint_decide(a_paint_policy *p, int attached, int got,
                            au64 seq, int stale)
{
    if (!p) return A_PAINT_NONE;

    /* Not attached: the engine is not running. Paint the banner once, on the
     * transition into that state, and then nothing until something changes. */
    if (!attached) {
        if (p->state == A_PSTATE_NO_ENGINE) { p->skips++; return A_PAINT_NONE; }
        p->state = A_PSTATE_NO_ENGINE;
        p->last_seq = 0;
        p->paints++; p->banner_paints++;
        return A_PAINT_BANNER;
    }

    /* A new frame always wins: it is new pixels by definition. */
    if (got) {
        /* A sequence that moves backwards means the producer was replaced.
         * Counted, because it is a useful restart signal at the bench. */
        if (p->state == A_PSTATE_FRAME && seq < p->last_seq) p->regressions++;
        p->state = A_PSTATE_FRAME;
        p->last_seq = seq;
        p->paints++; p->frame_paints++;
        return A_PAINT_FRAME;
    }

    /* No new frame. Only a CHANGE in staleness is worth a repaint. */
    if (stale) {
        if (p->state == A_PSTATE_STALE) { p->skips++; return A_PAINT_NONE; }
        p->state = A_PSTATE_STALE;
        p->paints++; p->banner_paints++;
        return A_PAINT_BANNER;
    }

    /* Attached, no new frame, not stale: the producer is simply publishing
     * slower than we poll. Nothing on screen has changed. */
    p->skips++;
    return A_PAINT_NONE;
}

/* ========================================================================== *
 * Cadence
 * ========================================================================== */

au32 a_cadence_snap(au32 hz)
{
    /* The four supported rates. Anything else snaps to the nearest, so a typo
     * in the environment cannot produce a 200 Hz repaint loop. */
    static const au32 ALLOWED[4] = { 5u, 10u, 15u, 30u };
    au32 best = ALLOWED[1];          /* 10 Hz is the default */
    au32 bestd = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++) {
        au32 d = hz > ALLOWED[i] ? hz - ALLOWED[i] : ALLOWED[i] - hz;
        if (d < bestd) { bestd = d; best = ALLOWED[i]; }
    }
    return best;
}

void a_cadence_set(a_cadence *c, au32 hz)
{
    if (!c) return;
    au32 h = a_cadence_snap(hz);
    c->hz = h;
    c->period_ms = 1000ull / (au64)h;
    c->next_ms = 0;
}

int a_cadence_due(a_cadence *c, au64 now_ms)
{
    if (!c || c->period_ms == 0) return 0;
    if (c->next_ms == 0) { c->next_ms = now_ms + c->period_ms; c->due++; return 1; }
    if (now_ms < c->next_ms) { c->skipped++; return 0; }

    /* Advance without drift. If we are more than one whole period late -
     * a stalled GUI thread, a long garbage collection, a suspended process -
     * resynchronise to now instead of firing a burst of catch-up ticks. That
     * burst is exactly what makes an overlay stutter after a stall. */
    au64 behind = now_ms - c->next_ms;
    if (behind >= c->period_ms) { c->late++; c->next_ms = now_ms + c->period_ms; }
    else                        { c->next_ms += c->period_ms; }
    c->due++;
    return 1;
}
