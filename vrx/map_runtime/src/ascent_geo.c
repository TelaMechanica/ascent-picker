#include "ascent_geo.h"

int a_finite(double x)
{
    /* Bit inspection: exponent all-ones means Inf or NaN. */
    union { double d; au64 u; } v; v.d = x;
    return ((v.u >> 52) & 0x7FFu) != 0x7FFu;
}

static int is_digit(char c){ return c >= '0' && c <= '9'; }
static int is_ws(char c){ return c==' '||c=='\t'||c=='\r'||c=='\n'; }

int a_parse_double(const char *s, ausize len, double *out)
{
    ausize i = 0;
    int neg = 0, any = 0;
    double mant = 0.0;
    int exp10 = 0;

    if (!s || !out) return 0;
    while (i < len && is_ws(s[i])) i++;
    if (i < len && (s[i]=='+' || s[i]=='-')) { neg = (s[i]=='-'); i++; }

    /* Integer part. Accumulate at most 18 significant digits exactly; beyond
     * that, track the decimal exponent instead of losing precision silently. */
    int sig = 0;
    for (; i < len && is_digit(s[i]); i++) {
        any = 1;
        if (sig < 18) { mant = mant * 10.0 + (double)(s[i] - '0'); sig++; }
        else exp10++;
    }
    if (i < len && s[i] == '.') {
        i++;
        for (; i < len && is_digit(s[i]); i++) {
            any = 1;
            if (sig < 18) { mant = mant * 10.0 + (double)(s[i] - '0'); sig++; exp10--; }
        }
    }
    if (!any) return 0;

    if (i < len && (s[i]=='e' || s[i]=='E')) {
        int eneg = 0, edig = 0; long ev = 0;
        i++;
        if (i < len && (s[i]=='+' || s[i]=='-')) { eneg = (s[i]=='-'); i++; }
        for (; i < len && is_digit(s[i]); i++) {
            edig = 1;
            if (ev < 100000) ev = ev * 10 + (s[i] - '0');
        }
        if (!edig) return 0;                 /* "1e" is not a number */
        exp10 += eneg ? -(int)ev : (int)ev;
    }

    while (i < len && is_ws(s[i])) i++;
    if (i != len) return 0;                  /* trailing garbage -> reject */

    /* Apply the decimal exponent by repeated multiply/divide. The inputs we
     * care about (WGS84 degrees) need |exp10| <= ~20, so this is exact enough
     * and avoids a pow() dependency. Clamp hard to keep it bounded. */
    if (exp10 > 308 || exp10 < -308) return 0;
    double v = mant;
    int k = exp10;
    if (k > 0) { while (k-- > 0) v *= 10.0; }
    else       { while (k++ < 0) v /= 10.0; }

    if (!a_finite(v)) return 0;
    *out = neg ? -v : v;
    return 1;
}

int a_sane_ll(double lat, double lon)
{
    if (!a_finite(lat) || !a_finite(lon)) return 0;
    if (!(lat >= -90.0 && lat <= 90.0)) return 0;
    if (!(lon >= -180.0 && lon <= 180.0)) return 0;
    if (lat == 0.0 && lon == 0.0) return 0;   /* GPS-less airframe reports 0/0 */
    return 1;
}
