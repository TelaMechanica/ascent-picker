/* See ascent_vis_policy.h. Pure: no clock, no file, no Qt, no allocation. */
#include "ascent_vis_policy.h"

int a_vis_armed_now(const a_vis_input *in)
{
    /* Three separate ways to not be armed, and all of them mean the same thing
     * here: fall back to the heuristic. Absent, stale and disarmed are kept
     * distinct in the INPUT so a diagnostic can tell them apart, and collapsed
     * only at this point, where the policy genuinely does not care. */
    if (!in->flight_known) return 0;
    if (in->flight_age_ms > A_VIS_FLIGHT_STALE_MS) return 0;
    return in->armed ? 1 : 0;
}

void a_vis_decide(const a_vis_input *in, a_vis_output *out)
{
    au64 open_delta = in->open_delta ? in->open_delta : A_VIS_OPEN_DELTA_DEF;
    au64 close_slack = in->close_slack ? in->close_slack : A_VIS_CLOSE_SLACK_DEF;
    au64 max_hide = in->max_hide_ms ? in->max_hide_ms : A_VIS_MAX_HIDE_MS_DEF;

    out->resync = 0;
    out->armed_gate = 0;
    out->ref = in->ref;
    out->stock_open = in->stock_open;

    /* 1. The operator's explicit choice wins over everything. It is the escape
     *    hatch for exactly the case where this policy is wrong in a way nobody
     *    predicted, so no inference may override it. */
    if (in->manual == A_VIS_MANUAL_HIDE) { out->visible = 0; out->stock_open = 0; return; }
    if (in->manual == A_VIS_MANUAL_SHOW) { out->visible = 1; out->stock_open = 0; return; }

    /* 2. Still learning the quiescent level. Never hide while we do not yet
     *    know what resting looks like. */
    if (!in->baseline_set) { out->visible = 1; out->stock_open = 0; return; }

    /* 3. ARMED. The heuristic cannot tell a menu from any other batch of
     *    widgets appearing at once, and arming is exactly that -- the stock UI
     *    puts its arm-state furniture up in one go, which is the same shape as
     *    a menu opening. The operator is not browsing menus mid-flight, and a
     *    map that vanishes then is far worse than a map over a menu.
     *
     *    The reference still FOLLOWS the count while armed. Without that, the
     *    arm-time burst would still be sitting above the reference at disarm
     *    and would read as a menu that had been open the whole flight. */
    if (a_vis_armed_now(in)) {
        out->visible = 1;
        out->stock_open = 0;
        out->ref = in->foreign;
        out->armed_gate = 1;
        return;
    }

    /* 4. Not armed, or not known to be. The existing heuristic, unchanged. */
    if (!in->stock_open) {
        if (in->foreign >= in->ref + open_delta) {
            out->stock_open = 1;
        } else {
            out->stock_open = 0;
            out->ref = in->foreign;          /* absorb drift */
        }
    } else {
        if (in->foreign <= in->ref + close_slack) {
            out->stock_open = 0;
            out->ref = in->foreign;
        } else {
            out->stock_open = 1;
        }
    }

    /* 5. The backstop. A belief held this long is more likely to be wrong than
     *    the stock UI is to be open that long. */
    if (out->stock_open && in->open_since_ms &&
        in->now_ms > in->open_since_ms &&
        (in->now_ms - in->open_since_ms) > max_hide) {
        out->resync = 1;
        out->stock_open = 0;
        out->ref = in->foreign;
    }

    out->visible = out->stock_open ? 0 : 1;
}
