/* ascent_cells.c — Picker RC/PWM cell mapping + activation validity. See ascent_cells.h. */
#include "ascent_cells.h"

int a_cells_active_valid(const a_cell_cfg *cells, int n_cfg, int *out, int cap)
{
    int n = 0;
    if (!cells) return 0;
    for (int i = 0; i < n_cfg; i++) {
        if (cells[i].valid) {
            if (out && n < cap) out[n] = i;
            n++;
        }
    }
    return n;
}

/* center_i = 1000 + (1000/N)*(i-0.5), rounded. Integer: 1000 + (1000*(2i-1)+N)/(2N). */
int a_cells_center_us(int i, int N)
{
    if (N < 1 || i < 1 || i > N) return 0;
    return 1000 + (1000 * (2 * i - 1) + N) / (2 * N);
}

int a_cells_lower_us(int i, int N)
{
    if (N < 1 || i < 1 || i > N) return 0;
    return 1000 + ((i - 1) * 1000 + N / 2) / N;
}

int a_cells_upper_us(int i, int N)
{
    if (N < 1 || i < 1 || i > N) return 0;
    return 1000 + (i * 1000 + N / 2) / N;
}

int a_cells_classify(int pwm, int N, int cur)
{
    if (N < 1) return A_CELL_INVALID;
    if (pwm < A_CELLS_IN_MIN || pwm > A_CELLS_IN_MAX) return A_CELL_INVALID;

    int p = pwm;                               /* clamp mild overshoot into the domain */
    if (p < A_CELLS_PWM_LO) p = A_CELLS_PWM_LO;
    if (p > A_CELLS_PWM_HI) p = A_CELLS_PWM_HI;

    /* Raw boundary classification: cell = floor((p-1000)*N/1000), [lower,upper), top incl. */
    int raw = (p - A_CELLS_PWM_LO) * N / 1000;
    if (raw >= N) raw = N - 1;
    if (raw < 0)  raw = 0;

    if (cur < 0 || cur >= N) return raw;       /* startup: normal boundary, establish state */
    if (raw == cur) return cur;                /* same cell: no change */

    /* Stateful +/-HYST hysteresis at the immediate boundary of the current cell. All integer
     * via scaling by N so the fractional boundaries (e.g. 1333.33) compare exactly. */
    long pN = (long)p * N;
    if (raw > cur) {
        long upperN = (long)A_CELLS_PWM_LO * N + (long)(cur + 1) * 1000;  /* upper_cur * N */
        if (pN > upperN + (long)A_CELLS_HYST * N) return raw;             /* crossed up */
        return cur;                                                       /* within hyst: hold */
    } else {
        long lowerN = (long)A_CELLS_PWM_LO * N + (long)cur * 1000;        /* lower_cur * N */
        if (pN < lowerN - (long)A_CELLS_HYST * N) return raw;             /* crossed down */
        return cur;                                                       /* within hyst: hold */
    }
}
