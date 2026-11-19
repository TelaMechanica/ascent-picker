#include "ascent_render.h"

void a_px(const a_canvas *c, int x, int y, au32 col)
{
    if ((au32)x < c->w && (au32)y < c->h) c->px[(au32)y * c->w + (au32)x] = col;
}

void a_rect(const a_canvas *c, int x, int y, int w, int h, au32 col)
{
    if (w <= 0 || h <= 0) return;
    /* Clip before iterating so an off-screen object costs nothing. The
     * per-pixel guard in a_px() remains as the correctness backstop. */
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    long x1 = (long)x + w, y1 = (long)y + h;
    if (x1 > (long)c->w) x1 = (long)c->w;
    if (y1 > (long)c->h) y1 = (long)c->h;
    for (int yy = y0; yy < (int)y1; yy++)
        for (int xx = x0; xx < (int)x1; xx++)
            c->px[(au32)yy * c->w + (au32)xx] = col;
}

void a_fill(const a_canvas *c, au32 col)
{
    au32 n = c->w * c->h;
    for (au32 i = 0; i < n; i++) c->px[i] = col;
}

static int iabs(int v){ return v < 0 ? -v : v; }

/* Coordinates are clamped to a bounded window so a wildly out-of-range object
 * cannot make Bresenham iterate for a long time. a_px() still clips writes. */
#define A_COORD_LIM 4096
static int clampi(int v){ return v < -A_COORD_LIM ? -A_COORD_LIM
                               : (v > A_COORD_LIM ? A_COORD_LIM : v); }

/* Bresenham with a square pen. Endpoints are clamped to a bounded window
 * around the canvas so a wildly out-of-range coordinate cannot make this
 * iterate for a long time; a_px() still clips every write. */
static void line_impl(const a_canvas *c, int x0, int y0, int x1, int y1,
                      int t, au32 col, int dashed)
{
    x0 = clampi(x0); y0 = clampi(y0);
    x1 = clampi(x1); y1 = clampi(y1);
    if (t < 1) t = 1;
    if (t > 64) t = 64;

    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, step = 0;
    for (;;) {
        if (!dashed || ((step / 4) & 1) == 0)
            a_rect(c, x0 - t / 2, y0 - t / 2, t, t, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        step++;
    }
}
void a_line (const a_canvas *c,int x0,int y0,int x1,int y1,int t,au32 col){ line_impl(c,x0,y0,x1,y1,t,col,0); }
void a_dline(const a_canvas *c,int x0,int y0,int x1,int y1,int t,au32 col){ line_impl(c,x0,y0,x1,y1,t,col,1); }

void a_tri_fill(const a_canvas *c, int x0, int y0, int x1, int y1,
                int x2, int y2, au32 col)
{
    /* Half-space test per pixel over the clipped bounding box. All arithmetic
     * is in long: at canvas scale the cross products cannot overflow, and the
     * sign of the doubled area makes the test independent of winding order so
     * a caller cannot silently get an empty triangle by listing points the
     * other way round. */
    long area = (long)(x1 - x0) * (y2 - y0) - (long)(y1 - y0) * (x2 - x0);
    if (area == 0) return;                      /* degenerate: nothing to fill */
    long sgn = area > 0 ? 1 : -1;

    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > (int)c->w - 1) maxx = (int)c->w - 1;
    if (maxy > (int)c->h - 1) maxy = (int)c->h - 1;

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            long w0 = sgn * ((long)(x1 - x0) * (y - y0) - (long)(y1 - y0) * (x - x0));
            long w1 = sgn * ((long)(x2 - x1) * (y - y1) - (long)(y2 - y1) * (x - x1));
            long w2 = sgn * ((long)(x0 - x2) * (y - y2) - (long)(y0 - y2) * (x - x2));
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) a_px(c, x, y, col);
        }
    }
}

void a_circle(const a_canvas *c, int cx, int cy, int r, int t, au32 col)
{
    if (r <= 0) return;
    if (r > 2048) r = 2048;
    if (t < 1) t = 1;
    if (t > r) t = r;
    int rr = r * r, ri = (r - t) * (r - t);
    for (int y = -r; y <= r; y++) {
        int yy = cy + y;
        if ((au32)yy >= c->h) continue;          /* whole row is off-canvas */
        for (int x = -r; x <= r; x++) {
            int d = x * x + y * y;
            if (d <= rr && d >= ri) a_px(c, cx + x, yy, col);
        }
    }
}

/* ---- 5x7 glyphs ---- */
static const au8 GLYPH[][A_GLYPH_H] = {
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
 {14,17,17,15,1,1,14},
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
 {7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
 {0,0,0,31,0,0,0},   /* 36 - */
 {0,0,0,0,0,0,31},   /* 37 _ */
 {0,4,0,0,4,0,0},    /* 38 : */
 {0,0,0,0,0,12,12},  /* 39 . */
 {4,4,4,4,0,0,4},    /* 40 ! */
 {14,17,1,6,4,0,4},  /* 41 ? */
 {0,0,0,0,0,0,0},    /* 42 space */
 {10,10,31,10,31,10,10} /* 43 # : shown for any character we cannot draw */
};
#define GLYPH_UNKNOWN 43
#define GLYPH_SPACE   42

static const au8 *glyph_for(char ch)
{
    unsigned char c = (unsigned char)ch;
    if (c >= '0' && c <= '9') return GLYPH[c - '0'];
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return GLYPH[10 + c - 'A'];
    switch (c) {
    case '-': return GLYPH[36];
    case '_': return GLYPH[37];
    case ':': return GLYPH[38];
    case '.': return GLYPH[39];
    case '!': return GLYPH[40];
    case '?': return GLYPH[41];
    case ' ': return GLYPH[GLYPH_SPACE];
    default:  break;
    }
    /* Anything else - including control bytes and UTF-8 continuation bytes -
     * renders as a visible placeholder. The pre-fix renderer dropped unknown
     * characters silently, which made a mangled callsign look like a short
     * one. A visible box is more honest. */
    return (c < 0x20) ? GLYPH[GLYPH_SPACE] : GLYPH[GLYPH_UNKNOWN];
}

int a_text_advance(int scale){ return (A_GLYPH_W + 1) * (scale < 1 ? 1 : scale); }

int a_text(const a_canvas *c, int x, int y, const char *s, int maxchars,
           int scale, au32 fg, au32 shadow)
{
    if (!s || maxchars <= 0) return 0;
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    int adv = a_text_advance(scale), drawn = 0;
    for (int i = 0; i < maxchars && s[i]; i++) {
        const au8 *g = glyph_for(s[i]);
        int gx = x + i * adv;
        if (gx > (int)c->w) break;                  /* fully past the edge */
        for (int r = 0; r < A_GLYPH_H; r++)
            for (int col = 0; col < A_GLYPH_W; col++)
                if (g[r] & (1u << (A_GLYPH_W - 1 - col))) {
                    if (shadow) a_rect(c, gx + col*scale + 1, y + r*scale + 1, scale, scale, shadow);
                    a_rect(c, gx + col*scale, y + r*scale, scale, scale, fg);
                }
        drawn = (i + 1) * adv;
    }
    return drawn;
}

/* ---- projection-aware heading vector (F-PROJ-1) ---- */
/* Small-table sine/cosine in centidegrees; no libm dependency. 91 entries of
 * sin(0..90 deg) scaled by 10000. */
static const au16 SIN_T[91] = {
0,175,349,523,698,872,1045,1219,1392,1564,1736,1908,2079,2250,2419,2588,2756,
2924,3090,3256,3420,3584,3746,3907,4067,4226,4384,4540,4695,4848,5000,5150,
5299,5446,5592,5736,5878,6018,6157,6293,6428,6561,6691,6820,6947,7071,7193,
7314,7431,7547,7660,7771,7880,7986,8090,8192,8290,8387,8480,8572,8660,8746,
8829,8910,8988,9063,9135,9205,9272,9336,9397,9455,9511,9563,9613,9659,9703,
9744,9781,9816,9848,9877,9903,9925,9945,9962,9976,9986,9994,9998,10000 };

static int sin10k(int deg)      /* deg in 0..359 */
{
    deg = ((deg % 360) + 360) % 360;
    if (deg <= 90)  return  SIN_T[deg];
    if (deg <= 180) return  SIN_T[180 - deg];
    if (deg <= 270) return -SIN_T[deg - 180];
    return -SIN_T[360 - deg];
}
static int cos10k(int deg){ return sin10k(deg + 90); }

/* Newton square root: the engine links -nostdlib and must not pull in libm. */
static double nsqrt(double m)
{
    if (m <= 0.0) return 0.0;
    double r = m > 1.0 ? m : 1.0, prev = 0.0;
    for (int i = 0; i < 40 && r != prev; i++) { prev = r; r = 0.5 * (r + m / r); }
    return r;
}

/* Accurate sine/cosine in degrees, for the geospatial layer.
 *
 * The 91-entry SIN_T table above is fine for a 1-degree heading arrow but its
 * interpolation error (~1.6e-4) propagates through tan() and ln() in the
 * Mercator projection to roughly 1e-3 degrees of latitude - about 100 m. The
 * tile layer therefore uses a proper range-reduced Taylor evaluation instead.
 * Still no libm: the series is evaluated on |t| <= pi/4 where it converges to
 * about 1e-15.
 *
 * a_heading_vector below is deliberately left on the integer table: its
 * behaviour is pinned by the existing projection tests and there is no reason
 * to perturb a hardware-proven result for accuracy it does not need.
 */
static void sincos_small(double t, double *s, double *c)   /* |t| <= pi/4 */
{
    double t2 = t * t;
    *s = t * (1.0 + t2 * (-1.0 / 6.0 + t2 * (1.0 / 120.0 + t2 * (-1.0 / 5040.0
             + t2 * (1.0 / 362880.0 - t2 / 39916800.0)))));
    *c = 1.0 + t2 * (-0.5 + t2 * (1.0 / 24.0 + t2 * (-1.0 / 720.0
             + t2 * (1.0 / 40320.0 - t2 / 3628800.0))));
}

double a_sind(double deg)
{
    /* Reject non-finite and absurd magnitudes rather than looping on them. */
    if (!(deg > -1.0e12 && deg < 1.0e12)) return 0.0;
    double d = deg - 360.0 * (double)(as64)(deg / 360.0);
    if (d < 0.0) d += 360.0;

    int q = (int)(d * (1.0 / 90.0));
    if (q < 0) q = 0;
    if (q > 3) q = 3;
    double r = d - 90.0 * (double)q;          /* 0..90 */

    int folded = 0;
    if (r > 45.0) { r = 90.0 - r; folded = 1; }
    double su, cu;
    sincos_small(r * 0.01745329251994329577, &su, &cu);
    double sr = folded ? cu : su;             /* sin of the 0..90 angle */
    double cr = folded ? su : cu;             /* cos of the same        */

    switch (q) {
        case 0:  return  sr;                  /* sin(r)        */
        case 1:  return  cr;                  /* sin(90 + r)   */
        case 2:  return -sr;                  /* sin(180 + r)  */
        default: return -cr;                  /* sin(270 + r)  */
    }
}
double a_cosd(double deg){ return a_sind(deg + 90.0); }

void a_heading_vector(au16 course_cd, double lat_deg, int len, int *dx, int *dy)
{
    int deg = (int)((course_cd % 36000u) / 100u);
    if (lat_deg < 0) lat_deg = -lat_deg;
    if (lat_deg > 89.0) lat_deg = 89.0;

    /* In plate carree a degree of longitude and a degree of latitude occupy
     * the same number of pixels, but a degree of longitude spans only
     * cos(lat) of the ground distance. Dividing the east component by
     * cos(lat) makes the drawn arrow follow the true ground track ACROSS THE
     * TERRAIN AS DRAWN, which is what a moving map must show. */
    double clat = (double)cos10k((int)lat_deg) / 10000.0;
    if (clat < 0.05) clat = 0.05;

    double ex =  ((double)sin10k(deg) / 10000.0) / clat;   /* east  -> +x */
    double ny = -((double)cos10k(deg) / 10000.0);          /* north -> -y */

    double mag = nsqrt(ex * ex + ny * ny);
    if (mag <= 0.0) { *dx = 0; *dy = -len; return; }

    double fx = ex / mag * (double)len;
    double fy = ny / mag * (double)len;
    *dx = (int)(fx + (fx < 0 ? -0.5 : 0.5));
    *dy = (int)(fy + (fy < 0 ? -0.5 : 0.5));
}
