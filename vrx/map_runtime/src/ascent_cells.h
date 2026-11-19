/* ascent_cells.h — Picker RC/PWM cell mapping + activation validity.
 *
 * ONE source of truth for how an RC PWM value maps to a Picker cell, which cells are
 * usable on this VRX, and whether the Picker is active at all. Both the headless RC path
 * and ASCENT Control's manual SELECT/UI use identical semantics (the host mirrors this in
 * Python and is cross-validated against it).
 *
 * Rules (operator addendum, hard requirements):
 *  - PWM domain is a fixed 1000..2000 us, divided EVENLY across all CONFIGURED cells.
 *  - A cell is active-valid iff it has an exact BbId AND that BbId is in THIS VRX's T2
 *    candidate library. Validity is never inferred from nickname / Host Fleet DB / cache.
 *  - N = configured count. Picker is ACTIVE only when active_valid >= 2 (multi-aircraft feature).
 *  - Invalid cells retain their PWM region and can never reach an RF command.
 *  - +/-10 us STATEFUL hysteresis around internal boundaries (not tiny valid bands); the
 *    whole mathematical cell stays valid. Debounce is applied on top, by the caller.
 *  - Deterministic low->high PWM ordering (configured order, including unavailable positions).
 *
 * Pure: nothing allocates, no float, no I/O. Everything is integer us.
 */
#ifndef ASCENT_CELLS_H
#define ASCENT_CELLS_H
#include "ascent_types.h"

#define A_CELLS_PWM_LO     1000   /* nominal PWM domain */
#define A_CELLS_PWM_HI     2000
#define A_CELLS_HYST         10   /* +/- us boundary hysteresis */
#define A_CELLS_IN_MIN      900   /* below this => invalid/out-of-range sample (no switch) */
#define A_CELLS_IN_MAX     2100   /* above this => invalid/out-of-range sample (no switch) */
#define A_CELLS_MAX_CFG      36   /* max configured cells (bar display ceiling) */
#define A_CELLS_MIN_ACTIVE    2   /* Picker is a multi-aircraft feature */
#define A_CELL_INVALID       (-1)

/* One configured Picker cell: an exact BbId and whether it is valid on THIS receiver
   (i.e. its BbId is present in this VRX's T2 candidate library). `valid` must be decided
   by the caller from T2 membership, never from nickname/Fleet-DB alone. */
typedef struct {
    au8 bbid[4];
    int valid;      /* 1 iff bbid in this VRX T2 */
} a_cell_cfg;

/* Collect the active-valid cell indices (into `cells`) in configured (low->high PWM) order.
   Writes up to `cap` indices into out[], returns N (active-valid count). */
int a_cells_active_valid(const a_cell_cfg *cells, int n_cfg, int *out, int cap);

/* Picker active (enabled) iff N >= 2. */
static inline int a_cells_picker_active(int active_valid_n)
{ return active_valid_n >= A_CELLS_MIN_ACTIVE; }

/* Recommended EdgeTX center for 1-based cell i over N active cells, rounded to us.
   center_i = 1000 + (1000/N)*(i-0.5). Returns 0 for bad args. */
int a_cells_center_us(int i, int N);
/* Lower/upper mathematical boundary of 1-based cell i over N, rounded to us (display). */
int a_cells_lower_us(int i, int N);
int a_cells_upper_us(int i, int N);

/* Classify a PWM sample into a 0-based active-cell index (0..N-1) with stateful hysteresis.
 *  cur = A_CELL_INVALID (-1): startup / no prior state -> classify by the normal boundaries.
 *  cur in 0..N-1: hold `cur` unless the sample crosses the relevant boundary by > HYST.
 * Returns the new current cell (0..N-1), or A_CELL_INVALID if the sample is out of range
 * (< A_CELLS_IN_MIN or > A_CELLS_IN_MAX) -- the caller must then hold and issue no switch. */
int a_cells_classify(int pwm_us, int N, int cur);

#endif
