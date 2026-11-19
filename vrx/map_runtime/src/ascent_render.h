/* ascent_render.h — drawing primitives, glyphs and tactical symbols.
 *
 * Factored out of the engine so the host tests can drive it under ASan and
 * prove that no label, symbol or off-screen object can write outside the
 * frame buffer. Every primitive terminates at a_px(), which bounds-checks.
 *
 * Also carries the projection fix, red-team F-PROJ-1:
 *   The base map is EPSG:4326 plate carree, so one degree of longitude and
 *   one degree of latitude occupy the SAME number of pixels. True ground
 *   track and displayed track therefore differ by 1/cos(latitude). The
 *   pre-fix arrow was drawn from a circular 16-entry table at true course, so
 *   it disagreed with the terrain it sat on by up to 6.8 degrees at 38 N and
 *   19.5 degrees at 60 N. a_heading_vector() builds the vector in MAP PIXEL
 *   space instead.
 */
#ifndef ASCENT_RENDER_H
#define ASCENT_RENDER_H
#include "ascent_types.h"

typedef struct {
    au32 *px;
    au32  w, h;
} a_canvas;

/* ---- primitives (all clipped) ---- */
void a_px    (const a_canvas *c, int x, int y, au32 col);
void a_rect  (const a_canvas *c, int x, int y, int w, int h, au32 col);
void a_line  (const a_canvas *c, int x0, int y0, int x1, int y1, int t, au32 col);
void a_dline (const a_canvas *c, int x0, int y0, int x1, int y1, int t, au32 col);
void a_circle(const a_canvas *c, int cx, int cy, int r, int t, au32 col);
void a_fill  (const a_canvas *c, au32 col);
/* Filled triangle, clipped, winding-agnostic. Added for the aircraft marker:
 * the three a_line() calls that used to draw it produced an outline, and an
 * outline at a small size reads as a smudge rather than a heading. */
void a_tri_fill(const a_canvas *c, int x0, int y0, int x1, int y1,
                int x2, int y2, au32 col);

/* ---- text ---- */
#define A_GLYPH_W 5
#define A_GLYPH_H 7
/* Returns the advance in pixels for one character at the given scale. */
int  a_text_advance(int scale);
/* Draws at most maxchars characters. Returns the pixel width drawn. */
int  a_text(const a_canvas *c, int x, int y, const char *s, int maxchars,
            int scale, au32 fg, au32 shadow);

/* ---- tactical symbol ---- */

/* ---- projection ----
 * Unit heading vector in MAP PIXEL space, scaled to `len` pixels.
 * course_cd is centidegrees clockwise from north; lat_deg is the current
 * latitude, which sets the plate-carree anisotropy. */
void a_heading_vector(au16 course_cd, double lat_deg, int len, int *dx, int *dy);

/* ---- trig for the geospatial layer ----
 * Phase 2 needs sine and cosine at better than the 1-degree resolution the
 * heading table gives. These interpolate linearly between the SAME 91-entry
 * table, so the build still has exactly one trig table and still links no
 * libm. Worst-case error is under 1.6e-4 absolute, which is far below the
 * accuracy of anything it feeds (scale bars and degree-length series).
 * a_heading_vector is deliberately NOT changed: its behaviour is pinned by
 * the existing projection tests. */
double a_sind(double deg);
double a_cosd(double deg);

#endif
