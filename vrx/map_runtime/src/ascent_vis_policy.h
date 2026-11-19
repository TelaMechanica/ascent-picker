/* ascent_vis_policy.h -- should the map overlay be visible right now?
 *
 * WHY THIS IS ITS OWN FILE. The decision used to live inline in the LD_PRELOAD
 * shim, which runs inside GlassesUI and cannot be unit tested on a host. That
 * is how a rule that is obviously wrong in hindsight -- "a sudden rise in
 * visible widgets means a menu opened" applied while the aircraft is FLYING --
 * survived: there was nowhere to write the case down.
 *
 * Everything here is pure. No clock, no file, no Qt, no allocation. The caller
 * passes what it observed and gets back a decision, so every state including
 * the ones that are hard to produce on a bench is reachable from a test.
 */
#ifndef ASCENT_VIS_POLICY_H
#define ASCENT_VIS_POLICY_H

#include "ascent_types.h"

/* The operator's manual override. An escape hatch, deliberately coarse: it
 * changes nothing about geometry, zoom mapping or the installed package, and it
 * is not a replacement for the policy -- A_VIS_MANUAL_AUTO is the normal state
 * and the one the unit boots in. */
#define A_VIS_MANUAL_AUTO 0
#define A_VIS_MANUAL_SHOW 1
#define A_VIS_MANUAL_HIDE 2

/* A rise this big within one tick is the stock UI appearing, not drift. */
#define A_VIS_OPEN_DELTA_DEF  12u
/* Back to within this of the reference counts as closed again. */
#define A_VIS_CLOSE_SLACK_DEF 4u

/* How long a flight-state reading stays trustworthy. A heartbeat is nominally
 * 1 Hz, so two seconds is two missed beats: long enough not to flap on one lost
 * frame, short enough that a disconnected link stops claiming the aircraft is
 * armed. Past this the reading is UNKNOWN, and unknown falls back to the
 * heuristic -- it never means "armed". */
#define A_VIS_FLIGHT_STALE_MS 2000u

/* The backstop. If the policy has believed the stock UI is open for longer than
 * this, it gives up and resyncs rather than leaving the map hidden forever.
 *
 * WAS 30000. The reported symptom was a map that vanished on arm and came back
 * "after a while"; that while was this. Lowering it to 5 s is the STOPGAP half
 * of the fix and is deliberately independent of the armed-aware half, so either
 * can be reverted without the other. */
#define A_VIS_MAX_HIDE_MS_DEF 5000u

typedef struct {
    /* Observed, by the caller. */
    au64 foreign;          /* visible widgets that are not ours               */
    au64 ref;              /* the resting reference this policy last set      */
    int  stock_open;       /* the policy's previous belief                    */
    au64 open_since_ms;    /* when that belief began; 0 when not open         */
    au64 now_ms;

    /* Flight state, as published by the engine. `known` is 0 when no reading
     * exists at all; `age_ms` ages a reading that does. */
    int  flight_known;
    int  armed;
    au64 flight_age_ms;

    int  manual;           /* A_VIS_MANUAL_*                                  */
    int  baseline_set;     /* 0 while still learning the quiescent level      */

    /* Tunables, so a bench can move them without a rebuild. */
    au64 open_delta;
    au64 close_slack;
    au64 max_hide_ms;
} a_vis_input;

typedef struct {
    int  visible;          /* what the overlay should be                      */
    int  stock_open;       /* the belief to carry into the next tick          */
    au64 ref;              /* the reference to carry into the next tick       */
    int  resync;           /* the backstop fired; the caller should relearn   */
    int  armed_gate;       /* the armed rule, not the heuristic, decided this */
} a_vis_output;

/* Is the published flight reading usable AND positively armed? */
int a_vis_armed_now(const a_vis_input *in);

void a_vis_decide(const a_vis_input *in, a_vis_output *out);

#endif
