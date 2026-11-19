#include "ascent_surface.h"

void a_surface_init(a_surface *s, a_surface_acquire_fn acquire, void *user,
                    au32 w, au32 h, ausize stride)
{
    if (!s) return;
    a_memset(s, 0, sizeof(*s));
    s->acquire = acquire;
    s->user    = user;
    s->w = w; s->h = h;
    /* A stride below the packed row width would make the copy overlap rows;
     * refuse it by clamping up, so a misconfigured caller shears rather than
     * corrupting adjacent memory. */
    s->stride = stride < (ausize)w * 4u ? (ausize)w * 4u : stride;
}

int a_surface_write(a_surface *s, const au32 *src)
{
    if (!s || !src || !s->acquire || s->w == 0 || s->h == 0) return 0;

    /* THE FIX. Re-acquire every single time.
     *
     * Caching this base across frames is what froze the display: once the
     * QImage's data became shared with the QPixmap, the cached pointer was no
     * longer the buffer Qt would paint from. QImage::bits() detaches, so
     * asking again is what guarantees we are writing into a buffer this image
     * privately owns. */
    au8 *base = s->acquire(s->user);
    s->acquires++;
    if (!base) { s->acquire_failures++; return 0; }
    if (s->last_base && base != s->last_base) s->rebinds++;
    s->last_base = base;

    for (au32 y = 0; y < s->h; y++)
        a_memcpy(base + (ausize)y * s->stride,
                 src + (ausize)y * s->w,
                 (ausize)s->w * 4u);
    s->writes++;
    return 1;
}
