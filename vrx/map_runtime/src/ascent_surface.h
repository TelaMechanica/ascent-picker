/* ascent_surface.h — the shim's pixel-delivery step, made testable.
 *
 * ============================================================================
 * WHY THIS EXISTS: the frozen-display regression, 2026-09-18
 * ============================================================================
 *
 * On the MAX the engine was demonstrably publishing different pixels per zoom
 * level (two distinct pixel-only SHA-256s were captured), the shim's counters
 * were all advancing - frames_taken, frame_paints, blits - and the physical
 * display stayed frozen on the first rendered map forever.
 *
 * The cause was on the Qt side of our own buffer. The shim acquired the
 * QImage's pixel base ONCE, at build time:
 *
 *     Q_img(img, W, H, fmt);
 *     Bits = Q_bits(img);          // cached for the life of the overlay
 *     ...
 *     memcpy(Bits, Scratch, ...);  // every frame, straight through the cache
 *
 * That pattern is unsound in Qt and only appeared to work in the
 * hardware-proven build because of a second-order effect: that build used
 * QImage::Format_ARGB32, which differs from the raster pixmap's native format,
 * so QRasterPlatformPixmap::createPixmapForImage took its convertToFormat()
 * branch and DEEP COPIED the current pixels on every convertFromImage().
 *
 * Phase 2 changed the format to Format_RGB32 (to avoid a per-pixel source-over
 * blend). That made the source format equal the raster pixmap's native format,
 * so the same code path took `image = sourceImage` instead - a SHALLOW share.
 * The QPixmap, and the QLabel's copy of it, then referenced the very
 * QImageData we were mutating behind Qt's back through the cached pointer, and
 * Qt's copy-on-write and cacheKey machinery was free to conclude that nothing
 * had changed.
 *
 * ============================================================================
 * THE PROPERTY THIS MODULE ENFORCES
 * ============================================================================
 *
 *   Every accepted frame is copied into the surface's CURRENT pixel base,
 *   re-acquired immediately before the copy - never into a base cached from
 *   an earlier frame.
 *
 * Re-acquiring is what makes the copy correct under Qt semantics: QImage::bits()
 * is non-const and calls detach(), so if the image data has become shared with
 * a QPixmap it returns a fresh private buffer rather than the shared one. Doing
 * that every frame means we can never write into a buffer Qt has decided is
 * somebody else's, and can never fail to write into the one it is about to
 * paint.
 *
 * The acquire step is a callback, so the whole thing is host-testable with no
 * Qt at all. tests/test_surface.c drives it with a deliberately hostile
 * acquirer that returns a DIFFERENT buffer on every call - which is exactly
 * what a detaching QImage does - and proves the second of two distinct frames
 * reaches the buffer that would be painted.
 *
 * ============================================================================
 * WHAT THIS MODULE DELIBERATELY DOES NOT DO
 * ============================================================================
 *
 * It does not touch the mmap. The frame is already copied out of the producer
 * mapping into shim-owned memory by a_frame_consumer_take() before it gets
 * here, so no Qt object can ever hold a pointer into producer memory that a
 * rename/remap could replace. That property predates this fix and is kept.
 */
#ifndef ASCENT_SURFACE_H
#define ASCENT_SURFACE_H
#include "ascent_types.h"

/* Returns the CURRENT pixel base of the destination, or NULL. Called once per
 * publish, immediately before the copy. In the shim this is QImage::bits();
 * in the tests it is a double that reallocates on every call. */
typedef au8 *(*a_surface_acquire_fn)(void *user);

typedef struct {
    a_surface_acquire_fn acquire;
    void   *user;
    au32    w, h;            /* pixels */
    ausize  stride;          /* destination row stride in BYTES */
    /* instrumentation */
    au64 acquires;           /* acquire() calls                              */
    au64 writes;             /* successful full-frame copies                 */
    au64 acquire_failures;   /* acquire() returned NULL                      */
    au64 rebinds;            /* acquire() returned a DIFFERENT base than last*/
    au8 *last_base;          /* what the previous acquire returned           */
} a_surface;

void a_surface_init(a_surface *s, a_surface_acquire_fn acquire, void *user,
                    au32 w, au32 h, ausize stride);

/* Re-acquire the destination and copy w*h packed RGBA pixels into it.
 * Returns 1 when the frame was written, 0 when the surface could not be
 * acquired (in which case nothing is written and the caller must not present).
 *
 * `src` must be w*h packed pixels - the shim's own scratch buffer. */
int  a_surface_write(a_surface *s, const au32 *src);

#endif
