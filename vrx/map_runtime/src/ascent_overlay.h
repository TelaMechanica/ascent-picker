/* ascent_overlay.h — the shim's decision logic, separated from its Qt calls.
 *
 * WHY THIS MODULE EXISTS
 * ======================
 * Everything that decides *whether* the overlay should be rebuilt, *whether* a
 * repaint is warranted and *when* a tick is due lives here as pure functions
 * over plain data. The Qt-calling half of the shim
 * (src/ascent_frame_overlay.c) does nothing but carry those decisions out.
 *
 * That split is what makes the red-team's hardest questions testable on the
 * host: "does a parent being replaced 10 000 times create 10 000 widgets?" is
 * a question about this file, and tests/test_overlay.c answers it under ASan
 * without a Qt, a framebuffer or a VRX.
 *
 * TARGET SELECTION - THE FLICKER FIX
 * ==================================
 * The Phase 1 shim re-resolved QApplication::activeWindow() every tick and
 * rebuilt the overlay whenever the pointer changed. That fixed the
 * use-after-free (P0-2) but left three problems that show up as flicker:
 *
 *   1. A QMenu is a top-level widget. When one opens it can BECOME the active
 *      window, so the overlay was rebuilt into the menu and rebuilt again when
 *      it closed - the overlay visibly jumping and two QLabels leaked per
 *      menu interaction.
 *   2. activeWindow() returns NULL transiently during window transitions. The
 *      old code cleared its ready flag on NULL, so the very next tick with a
 *      non-NULL window rebuilt - even when it was the SAME window.
 *   3. Nothing bounded the number of rebuilds over a session.
 *
 * The policy here answers each one:
 *
 *   * A candidate smaller than min_w x min_h is a popup, not a page. It is
 *     REJECTED and the existing overlay is KEPT. A menu opening therefore
 *     causes no rebuild at all. (The main window is 1920x1080; the default
 *     threshold of 640x360 separates them by a wide margin.)
 *   * A NULL candidate produces IDLE: draw nothing this tick, but do not
 *     rebuild either. Only a NULL that PERSISTS for stable_ticks is treated as
 *     "the window is really gone", after which the next valid candidate
 *     rebuilds. A short gap followed by the same window resumes with no
 *     rebuild and no repaint churn.
 *   * A different, large, valid candidate must be observed for stable_ticks
 *     consecutive ticks before it is adopted. A page change costs exactly one
 *     rebuild; a flicker of focus costs none.
 *   * Rebuilds are hard-capped. Past the cap the policy returns IDLE forever
 *     and says so in its counters: the overlay fails INVISIBLE rather than
 *     leaking without limit or touching a possibly-freed QObject.
 *
 * RESIDUAL RISK, STATED
 * =====================
 * Resuming on the same pointer after a short NULL gap assumes the window was
 * not destroyed and a new one allocated at the same address inside that gap.
 * That ABA window is stable_ticks long (3 ticks, ~200 ms at 15 Hz) rather than
 * unbounded as it was before. It cannot be closed from inside an LD_PRELOAD
 * shim without a QObject destruction notification, which would need far more
 * Qt ABI surface than this shim is allowed to carry. B7 in the bench plan is
 * where it gets exercised.
 */
#ifndef ASCENT_OVERLAY_H
#define ASCENT_OVERLAY_H
#include "ascent_types.h"

/* ---- target selection ---------------------------------------------------- */
typedef enum {
    A_OVL_KEEP  = 0,   /* use the existing overlay; draw normally            */
    A_OVL_BUILD = 1,   /* build a new overlay on policy->adopt, then draw    */
    A_OVL_IDLE  = 2    /* no usable target: draw NOTHING, touch nothing      */
} a_ovl_action;

#define A_OVL_STABLE_DEF   3u
#define A_OVL_MIN_W_DEF  640u
#define A_OVL_MIN_H_DEF  360u
#define A_OVL_CAP_DEF     64u

typedef struct {
    void *current;        /* the parent the live overlay is attached to      */
    void *pending;        /* candidate under observation                     */
    void *adopt;          /* set alongside A_OVL_BUILD: the parent to use    */
    au32  pending_ticks;
    au32  null_ticks;
    au32  stable_ticks;
    au32  min_w, min_h;
    au32  rebuild_cap;
    int   have;           /* an overlay exists on `current`                  */

    /* counters, all monotonic - these are what the bench reads out of the log */
    au64 ticks, builds, keeps, idles;
    au64 rej_small, rej_unstable, null_gaps, lost_windows, capped_events;
} a_ovl_policy;

void a_ovl_init(a_ovl_policy *p, au32 stable_ticks, au32 min_w, au32 min_h,
                au32 rebuild_cap);
/* `cand` is QApplication::activeWindow() (may be NULL); cw/ch are its geometry
 * (ignored when cand is NULL). On A_OVL_BUILD the caller must build on
 * p->adopt and then call a_ovl_built(). */
a_ovl_action a_ovl_tick(a_ovl_policy *p, void *cand, int cw, int ch);
/* Confirm a successful build (the policy adopts p->adopt as current). */
void a_ovl_built(a_ovl_policy *p);
/* Report a failed build: the candidate is dropped and will be re-observed. */
void a_ovl_build_failed(a_ovl_policy *p);

/* ---- repaint decision ---------------------------------------------------- */
/* Repainting with content identical to what is already on screen is pure
 * churn: it costs a full pixmap conversion and a framebuffer flush for no
 * visible change. The Phase 1 shim did exactly that at 10 Hz whenever the
 * engine was absent or stale. This decides when a repaint is actually
 * warranted, so an idle overlay costs zero repaints. */
typedef enum {
    A_PAINT_NONE = 0,     /* nothing changed - do not touch the widget       */
    A_PAINT_FRAME,        /* a new frame was taken                           */
    A_PAINT_BANNER        /* a banner state transition                       */
} a_paint_kind;

typedef enum {
    A_PSTATE_INIT = 0,
    A_PSTATE_FRAME,
    A_PSTATE_NO_ENGINE,
    A_PSTATE_STALE
} a_paint_state;

typedef struct {
    int  state;
    au64 last_seq;
    au64 paints, skips, frame_paints, banner_paints, regressions;
} a_paint_policy;

void a_paint_init(a_paint_policy *p);
/* attached: the frame file is mapped. got: a new frame was taken this tick.
 * seq: that frame's sequence number. stale: the producer has gone quiet. */
a_paint_kind a_paint_decide(a_paint_policy *p, int attached, int got,
                            au64 seq, int stale);

/* ---- update cadence ------------------------------------------------------ */
/* The overlay update rate is independent of how often telemetry arrives. A
 * burst of CRSF frames must not become a burst of repaints. */
#define A_HZ_MIN  5u
#define A_HZ_MAX 30u
typedef struct {
    au32 hz;
    au64 period_ms;
    au64 next_ms;
    au64 due, skipped, late;
} a_cadence;

/* Accepts 5, 10, 15 or 30 Hz; anything else snaps to the nearest of those. */
void a_cadence_set(a_cadence *c, au32 hz);
au32 a_cadence_snap(au32 hz);
/* True when a new tick is due at now_ms. Advances the schedule without drift,
 * and resynchronises rather than bursting if it has fallen far behind. */
int  a_cadence_due(a_cadence *c, au64 now_ms);

#endif
