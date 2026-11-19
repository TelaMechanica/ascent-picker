/* ascent_picker_probe.c -- the VTX Picker LAYER producer.
 *
 * Renders the picker bar (cells + icons + accent) into its OWN frame channel,
 * independent of the map engine. The blit shim composites it as a second,
 * independently-placed overlay layer on the same plane as Mapster, so a unit can
 * run map only, picker only, both, or neither.
 *
 * It reads /tmp/ascent_picker_bar (the layout/content control file the VRX runtime
 * writes) and republishes on a slow tick. A missing/stale runtime heartbeat
 * hides the bar, including after an owner crash. Widget Layout controls geometry. The frame is its own channel -- it never touches the map's.
 */
#include "ascent_sys.h"
#include "ascent_frame.h"
#include "ascent_pickerbar.h"
#include "ascent_render.h"

#define PBAR_CTRL    "/tmp/ascent_picker_bar"
#define PICKER_FRAME "/tmp/ascent_picker_frame.bin"   /* OWN channel, not the map's */
#define PICKER_SIZE  "/tmp/ascent_picker_size"        /* "w h", like the map's      */
/* The frame IS the bar: the Widget Layout tab sizes this frame to the bar's
 * on-screen rect, and the bar fills it. Bounds keep a resize inside the shim's
 * picker scratch buffer (A_PICKER_MAX_PX). */
#define PICKER_DEF_W 640u
#define PICKER_DEF_H 96u
#define PICKER_MIN_W 120u
#define PICKER_MIN_H 36u
#define PICKER_MAX_W 1920u
#define PICKER_MAX_H 400u

static a_pbar_store PB;
static char CBUF[A_PBAR_MAX_FILE];
static au32 CurW = PICKER_DEF_W, CurH = PICKER_DEF_H;

/* Two unsigned decimals from "w h". 0 on anything not fully understood. */
static int parse_wh(const char *s, au32 *w, au32 *h)
{
    au32 v[2] = {0, 0}; int got = 0; ausize i = 0;
    while (got < 2) {
        while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') i++;
        if (s[i] < '0' || s[i] > '9') break;
        au32 val = 0;
        while (s[i] >= '0' && s[i] <= '9') { val = val * 10u + (au32)(s[i] - '0'); i++; }
        v[got++] = val;
    }
    if (got != 2 || v[0] == 0 || v[1] == 0) return 0;
    *w = v[0]; *h = v[1];
    return 1;
}

static int clampw(au32 v, au32 lo, au32 hi) { return v < lo ? (int)lo : (v > hi ? (int)hi : (int)v); }

int main(void)
{
    a_frame_producer prod;
    /* instance_id 0 => the transport auto-assigns a UNIQUE id per start
     * (pid<<32 ^ now_ms). That is what lets the shim's consumer detect a
     * producer restart and re-attach instead of compositing the dead
     * instance's last frame forever. A fixed constant here defeats that. */
    if (!a_frame_producer_open_sz(&prod, PICKER_FRAME, 0, CurW, CurH))
        return 1;
    a_pbar_reset(&PB);
    char sb[32];

    for (;;) {
        /* size control: reopen the producer at the requested frame size */
        int sfd = open(PICKER_SIZE, O_RDONLY);
        if (sfd >= 0) {
            assize sn = a_read_full(sfd, sb, sizeof sb - 1);
            close(sfd);
            if (sn > 0) {
                sb[sn] = 0;
                au32 nw, nh;
                if (parse_wh(sb, &nw, &nh)) {
                    nw = (au32)clampw(nw, PICKER_MIN_W, PICKER_MAX_W);
                    nh = (au32)clampw(nh, PICKER_MIN_H, PICKER_MAX_H);
                    if (nw != CurW || nh != CurH) {
                        a_frame_producer np;
                        if (a_frame_producer_open_sz(&np, PICKER_FRAME, 0, nw, nh)) {
                            a_frame_producer_close(&prod);
                            prod = np; CurW = nw; CurH = nh;
                        }
                    }
                }
            }
        }

        PB.show=0;
        int fd = open(PBAR_CTRL, O_RDONLY);
        if (fd >= 0) {
            assize n = a_read_full(fd, CBUF, sizeof CBUF);
            close(fd);
            if (n > 0) a_pbar_parse(&PB, CBUF, (ausize)n);
        }
        if(!a_pbar_owner_fresh(&PB,(au32)a_now_ms())) PB.show=0;
        /* The frame is the bar: fill it, ignoring any geometry in the control file.
         * Placement/size are the size+geom files (Widget Layout tab). */
        PB.gx = 0; PB.gy = 0; PB.gw = 1000; PB.gh = 1000;

        au32 *fb = a_frame_producer_begin(&prod);
        if (fb) {
            ausize px = (ausize)CurW * (ausize)CurH;
            for (ausize i = 0; i < px; i++) fb[i] = 0;   /* transparent canvas */
            a_canvas cv = { fb, CurW, CurH };
            a_pbar_draw(&PB, &cv, CurW, CurH, a_now_ms());
            a_frame_producer_publish(&prod, a_now_ms());
        }
        a_sleep_ms(200);
    }
    return 0;
}

void _start(void) { int r = main(); _exit(r); }
