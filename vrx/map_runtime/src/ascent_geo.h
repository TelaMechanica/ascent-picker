/* ascent_geo.h — coordinate validation and a strict, locale-independent
 * decimal parser.
 *
 * Replaces the pre-fix use of strtod(), whose end pointer was discarded so
 * that lat="not-a-number" silently produced latitude 0 and still consumed an
 * object slot. strtod is also locale-dependent; nothing in
 * the engine calls setlocale, but relying on that is fragile. This parser is
 * locale-independent by construction.
 */
#ifndef ASCENT_GEO_H
#define ASCENT_GEO_H
#include "ascent_types.h"

/* Strict decimal parse. Accepts  [+-]?digits[.digits][(e|E)[+-]?digits]
 * Requires at least one digit, and requires that the ENTIRE string (after
 * optional surrounding ASCII whitespace) be consumed.
 * Returns 1 on success and stores the value; returns 0 and leaves *out
 * untouched on any failure (including overflow to infinity and NaN input). */
int a_parse_double(const char *s, ausize len, double *out);

/* WGS84 sanity. Rejects the 0/0 null island used by a GPS-less airframe,
 * rejects out-of-range, and rejects non-finite values. */
int a_sane_ll(double lat, double lon);

/* True when x is finite (no libm dependency). */
int a_finite(double x);

#endif
