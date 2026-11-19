/* ascent_frame_overlay.c — the GlassesUI blit shim (LD_PRELOAD).
 *
 * Still deliberately tiny: no SQLite, no tile decode, no telemetry, no
 * geospatial work, no map state. It maps the shared frame read-only and blits
 * it into the already-proven QImage -> QPixmap -> QLabel path.
 *
 * IT NEVER TOUCHES /dev/fb0. There is no framebuffer path in this file, in the
 * engine, or anywhere else in the tree: everything reaches the display through
 * Qt's own linuxfb paint path. BUILD_ALL.sh enforces that in the ELF.
 *
 * ---------------------------------------------------------------------------
 * PHASE 1 FIXES CARRIED HERE (unchanged)
 * ---------------------------------------------------------------------------
 *  P0-2 / F-SHIM-1  The QLabel was parented to activeWindow() ONCE and held as
 *                   a raw pointer forever. The target is now re-resolved every
 *                   tick and a raw pointer is never carried across a parent
 *                   change.
 *  P0-1 / F-FRAME-1 The shim mapped the frame file once and never looked
 *                   again. It now follows producer replacement by instance id
 *                   and draws its OWN staleness banner.
 *  F-SHIM-4         The attach loop is permanent, not a one-shot startup loop.
 *  SIGBUS guard     A SIGBUS inside the blit is trapped, the mapping dropped,
 *                   and the overlay re-attaches instead of killing GlassesUI.
 *
 * ---------------------------------------------------------------------------
 * PHASE 2: FLICKER HARDENING
 * ---------------------------------------------------------------------------
 * Every decision below lives in src/ascent_overlay.c and is tested on the host
 * by tests/test_overlay.c. This file only carries the decisions out.
 *
 *  1. PERSISTENT OVERLAY. The Phase 1 shim rebuilt whenever activeWindow()
 *     changed. A QMenu is a top-level widget and can BECOME the active window,
 *     so opening a menu rebuilt the overlay into the menu and rebuilt again on
 *     close - visible flicker plus two leaked QLabels per interaction. The
 *     target policy now rejects popup-sized candidates outright, requires a
 *     new window to persist before adopting it, and treats a short NULL gap as
 *     "skip this tick", not "rebuild".
 *
 *  2. GUI-THREAD ONLY. Every Qt call still happens inside the queued slot. The
 *     helper thread does nothing but post. NEW: at most A_MAX_PENDING posts
 *     may be outstanding. Phase 1 posted unconditionally every 100 ms, so a
 *     stalled GUI thread accumulated tasks that then all ran back to back -
 *     a burst of repaints exactly when the UI was already struggling.
 *
 *  3. CONFIGURABLE CADENCE, independent of telemetry. 5/10/15/30 Hz.
 *
 *  4. NO REPAINT WITHOUT NEW PIXELS. Phase 1 re-blitted, re-converted and
 *     re-set the pixmap every tick even when the engine was absent and the
 *     banner was identical. An idle overlay now costs zero repaints.
 *
 *  5. MINIMAL INVALIDATION. The label is fixed-size and explicitly positioned,
 *     and the only invalidation requested is on the label itself - never the
 *     parent, never the window. See FLICKER_RENDER_PATH_ANALYSIS.md for what
 *     that does and does not prove about linuxfb.
 *
 *  6. OPAQUE COMPOSITION. The frame is opaque by contract, so the QImage is
 *     declared RGB32 rather than ARGB32: Qt then does a straight copy instead
 *     of a per-pixel source-over blend against whatever is already in the
 *     backing store. ASCENT_OVERLAY_FMT=argb32 restores the Phase 1 format.
 *
 *  7. Z-ORDER RECOVERY. raise() is re-issued at a low, bounded rate so that a
 *     GlassesUI widget raised above us does not hide the map permanently.
 *
 * KNOWN, BOUNDED TRADE-OFF (unchanged in kind, now capped): when the target
 * window really does change we allocate a new QLabel and stop referring to the
 * old one. If the old parent is still alive the old label leaks. Touching a
 * possibly-freed QObject to clean it up is the very defect P0-2 fixed. Phase 2
 * bounds it: popups and transients no longer trigger rebuilds at all, and
 * a_ovl_policy refuses to build past a hard cap.
 */
#include "ascent_types.h"
#include "ascent_sys.h"
#include "ascent_vis_policy.h"
#include "ascent_frame.h"
#include "ascent_render.h"
#include "ascent_overlay.h"
#include "ascent_surface.h"

#define SHIM_EXPORT __attribute__((visibility("default")))

/* ---- Qt ABI ------------------------------------------------------------- */
#define QT_QUEUED_CONNECTION 2
#define QT_WA_TRANSPARENT_FOR_MOUSE_EVENTS 51
#define QT_NO_FOCUS 0
#define QT_ALIGN_CENTER 0x84
#define QIMAGE_FORMAT_RGB32  4
#define QIMAGE_FORMAT_ARGB32 5

struct SlotBase;
typedef void (*SlotFn)(int, struct SlotBase*, void*, void**, au8*);
struct SlotBase { volatile int ref; int pad; SlotFn impl; };
struct Task { struct SlotBase base; int kind; volatile int called; };

typedef void *(*fn_active)(void);
typedef void *(*fn_new)(ausize);
typedef void  (*fn_label_ctor)(void*, void*, au32);
typedef void  (*fn_attr)(void*, int, au8);
typedef void  (*fn_focus)(void*, int);
typedef void  (*fn_align)(void*, au32);
typedef void  (*fn_fixed)(void*, int, int);
typedef void  (*fn_move)(void*, int, int);
typedef void  (*fn_void1)(void*);
typedef void  (*fn_img_ctor)(void*, int, int, int);
typedef au8  *(*fn_bits)(void*);
typedef int   (*fn_dim)(void*);
typedef au8   (*fn_pm_conv)(void*, const void*, au32);
typedef void  (*fn_setpix)(void*, const void*);
typedef int   (*fn_invoke)(void*, struct SlotBase*, int, void*);
typedef const int *(*fn_geom)(void*);          /* const QRect& -> 4 ints */
/* Phase2C. Qt passes QPoint/QSize/QRect by const reference, so each of these
 * takes a pointer to a small POD int array:
 *     QPoint {x, y}            2 ints
 *     QSize  {w, h}            2 ints
 *     QRect  {x1, y1, x2, y2}  4 ints   (x2/y2 INCLUSIVE)
 * These are the forms the device actually exports. The (int,int) overloads
 * they replace are INLINE in Qt's headers and have never existed as symbols
 * in any Qt build - which is why every Phase2/2B run logged
 * "QWidget::move NOT resolved". */
typedef void  (*fn_move_pt)(void*, const int*);   /* QWidget::move(const QPoint&)      */
typedef void  (*fn_resize_sz)(void*, const int*); /* QWidget::resize(const QSize&)     */
typedef void  (*fn_setgeom)(void*, const int*);   /* QWidget::setGeometry(const QRect&)*/
typedef void *(*fn_w0)(void);                     /* QApplication::active*Widget()     */

static fn_active   Q_active;
static fn_new      Q_new;
static fn_label_ctor Q_label;
static fn_attr     Q_attr;
static fn_focus    Q_focus;
static fn_align    Q_align;
static fn_fixed    Q_fixed;
static fn_move     Q_move;
static fn_void1    Q_show, Q_raise, Q_update, Q_img_dtor, Q_pm_ctor;
static fn_img_ctor Q_img;
static fn_bits     Q_bits;
static fn_pm_conv  Q_pmconv;
static fn_setpix   Q_setpix;
static fn_invoke   Q_invoke;
static fn_geom     Q_geom;                     /* OPTIONAL - see resolve()    */
static fn_void1    Q_hide;                     /* OPTIONAL - z-order fallback */
/* Phase2C optional set. None is hard-required; each is probed and logged. */
static fn_move_pt   Q_move_pt;                 /* OPTIONAL - Phase2D position */
static fn_resize_sz Q_resize_sz;               /* OPTIONAL - Phase2E resize   */
static fn_setgeom   Q_setgeom;                 /* OPTIONAL - Phase2D preferred*/
static fn_w0        Q_popup;                   /* OPTIONAL - menu detection   */
static fn_w0        Q_modal;                   /* OPTIONAL - menu detection   */

/* OPTIONAL - MENU-KEY detection (the GlassesUI root menu creates no new widgets,
 * so the widget-count detector above is blind to it; but GlassesUI logs every key
 * it receives through qDebug, which we can see in-process via a Qt message
 * handler). QtMessageHandler is void(*)(QtMsgType, const QMessageLogContext&,
 * const QString&); we take the enum as int and the two references as opaque
 * pointers. QString::utf16() gives the text as null-terminated UTF-16. */
typedef const unsigned short *(*fn_utf16)(const void*);
typedef void (*QtMsgHandler)(int, const void*, const void*);
typedef QtMsgHandler (*fn_installmsg)(QtMsgHandler);
typedef void (*fn_msgout)(int, const void*, const void*);
static fn_utf16     Q_utf16;                   /* OPTIONAL - QString::utf16()  */
static fn_installmsg Q_installmsg;             /* OPTIONAL - install our handler */
static fn_msgout    Q_msgout;                  /* OPTIONAL - default output tail */
static QtMsgHandler PrevMsgHandler;            /* chain so vendor logging stays  */

/* RTLD_NEXT. The file deliberately avoids _GNU_SOURCE, so spell it out; it
 * is ((void*)-1) on every glibc/musl target this builds for. */
#define A_RTLD_NEXT ((void*)-1)
static void vis_bind(void);   /* defined with the interposers below */

/* ---- Phase2C-rev: visibility interposition ------------------------------
 * Gate 3 (2026-10-03) DISPROVED the popup/modal mechanism on hardware.
 * GlassesUI shows its menu as a PLAIN QWidget via show()/setVisible(true):
 * activePopupWidget() and activeModalWidget() both correctly returned null
 * while the menu was demonstrably open, and the app links no QStackedWidget.
 *
 * So stop asking Qt who is active and observe the call itself. GlassesUI
 * imports show(), hide() and setVisible(bool); we define them, the dynamic
 * linker binds its calls to ours because LD_PRELOAD precedes libQt5Widgets,
 * and we chain to the real implementation via RTLD_NEXT.
 *
 * THESE RUN ON GlassesUI'S GUI THREAD. They must be lock-free, allocation
 * free and must ALWAYS chain, or the stock UI freezes. No logging here. */
typedef void (*fn_setvis)(void*, int);
static fn_void1  Real_show, Real_hide;
static fn_setvis Real_setvis;
/* The three QWidget destructors. D1 and D2 are ALIASES in this device's
 * libQt5Widgets (both at 0x1be354, verified in its .dynsym), but they are
 * two separate dynamic symbols, so a caller that binds one is NOT routed
 * through an interposer installed on the other. Both must be taken.
 * D0 is the deleting destructor: it chains to D1 and then frees. */
static fn_void1  Real_dtor0, Real_dtor1, Real_dtor2;
/* QWidget::isVisibleTo(const QWidget*). Qt implements it as
 *   if (!ancestor) return isVisible();
 * so passing NULL is an exact visibility query, and it is EXPORTED, which
 * isVisible() itself is not (that one is inline in the headers).
 *
 * This is what makes the count correct rather than inferred. Qt clears
 * WA_WState_Visible on the children of a widget being hidden even though it
 * calls hide() on none of them, so a sweep sees the children go away and the
 * count returns to the resting level by itself. */
typedef int (*fn_isvisto)(void*, void*);
static fn_isvisto Real_isvisto;
/* Deallocation. Sweeping dereferences tracked pointers, so a freed widget must
 * never still be in the set. The three destructors catch what Qt routes
 * through them and measured almost nothing (vis_dtor_calls=2 over a session),
 * so they are not sufficient on their own. Every heap widget is released
 * through one of these two, whatever its class. */
typedef void (*fn_del1)(void*);
typedef void (*fn_del2)(void*, ausize);
static fn_del1 Real_delete;
static fn_del2 Real_delete_sized;

/* Widgets that became visible AFTER the overlay attached. Everything already
 * on screen at attach time is background (the main window included) and must
 * not count, or stock_ui would latch true forever. */
/* The stock Channel menu alone brings ~118 widgets (measured: baseline 7 ->
 * 64 tracked with 54 overflow drops). A bound that overflows is WORSE than no
 * tracking: the dropped show()s have no slot, their later hide()s match
 * nothing, and the count can never fall back to the resting level - the map
 * latches hidden forever. Sized for the largest stock screen with headroom,
 * and overflow now self-heals rather than wedging. */
#define A_VIS_MAX 512
/* The resting level is the MINIMUM the foreign count reaches over this window,
 * sampled in one-second buckets. A single learned-once baseline was wrong in
 * both directions: learned during startup it sat at 10 while the settled UI
 * rests at 15, which latched the map hidden with no way back, and
 * downward-only adaptation could never raise it. A rolling minimum needs no
 * adaptation policy at all - it rises when the UI genuinely rests higher and
 * falls the moment it rests lower.
 *
 * The window is the one real trade: stock UI held open longer than this is
 * absorbed into the resting level and stops being detected. 30s is longer
 * than menu and settings interactions on this device and short enough that a
 * mis-learned level corrects itself quickly. */
/* EDGE detection, not level detection.
 *
 * Level detection cannot survive this UI. Measured over seven menu cycles the
 * resting count climbed 14 -> 64: each cycle leaves about seven widgets that
 * Qt still reports VISIBLE after the menu is gone, so any resting level - a
 * learned baseline or a rolling minimum - lags a monotonically climbing count,
 * `foreign > rest + margin` stays true, and the map latches hidden. That is
 * exactly the "if I go in and out of the menu too many times it disappears"
 * report.
 *
 * The menu itself is unmistakable as a TRANSITION: opening it adds tens of
 * widgets within one tick (peak 101 against a resting 64), while the drift is
 * a few per cycle. So hide on a sudden rise and show when the count comes back
 * down near where it was, and let the reference follow the drift while
 * nothing is open. Slow drift can then never accumulate into a false hide. */
#define A_VIS_OPEN_DELTA  12u    /* a rise this big in one tick is stock UI   */
#define A_VIS_CLOSE_SLACK 4u     /* back to within this of the reference      */

static void *VisSet[A_VIS_MAX];
/* Entries are APPEND-ORDERED and VisTop is the high-water mark, so the
 * arrival order of show() calls is preserved. That order is the whole
 * mechanism - see vis_del(). Mutated only from the Qt GUI thread (widget
 * calls are illegal from any other), read from the shim thread. */
static int VisTop;
static au64 VisShowCalls, VisHideCalls, VisSetVisCalls;
static au64 VisOverflow, VisForeign, VisPeak;
static int  VisArmed;            /* set once the overlay owns a label        */

/* ---- Phase2D: live X/Y positioning --------------------------------------
 * Position has been dead code since it was written: the only call site was
 *   if (Q_move) Q_move(lab, Cfg_x, Cfg_y);
 * and Q_move names QWidget::move(int,int), which is INLINE in qwidget.h and
 * therefore exported by no Qt build. Phase2C resolved the real exported form,
 * QWidget::setGeometry(const QRect&), and only reported it. Phase2D uses it.
 *
 * WIDTH AND HEIGHT ARE NOT CHOSEN HERE. The label is under setFixedSize() at
 * the ATTACHED producer's geometry (FW x FH) and the geometry we submit
 * carries exactly those. Phase2E made that a runtime value; Phase2D still
 * only moves the widget, it never decides how big it is. */
static au64 GeomApplies, GeomClamps, GeomRejects, GeomReads, GeomParseFail;
static int  GeomX, GeomY;        /* last geometry ACTUALLY submitted to Qt    */
static int  GeomHave;
static int  GeomReqX, GeomReqY, GeomReqHave;  /* last PRE-clamp request */
static au32 Cfg_scrw, Cfg_scrh;  /* display surface used for bounds checking  */
static int  Cfg_allow_clip;
static au64 GeomNextPoll;
static const char *GP = "/tmp/ascent_overlay_geom";
/* Phase2F: the shim publishes what it ACTUALLY resolved and is currently
 * doing, so the transport can report runtime truth instead of a constant
 * compiled months earlier. ascent_runtime_bridge previously hardcoded
 * supports_widget_position:false and supports_menu_aware_overlay:false, both
 * of which Phase2C/2D disproved on hardware - stale by construction, and
 * guaranteed to go stale again at the next phase. The bridge now reads this
 * and treats its absence or staleness as "unknown", never as "true". */
static const char *SP = "/tmp/ascent_overlay_status";
static au64 NextStatus, StatusWrites, StatusFails;
static void geom_apply(int x, int y);   /* defined with the Phase2D helpers */
static au64 VisBindFail;         /* real Qt entry points failed to bind      */
static int  RealBound;           /* vis_bind() has run (CAS-guarded, once)   */
static au64 VisArmedAt;          /* ms when the overlay armed the tracker    */
static au64 VisBaseline;         /* resting count of foreign-visible widgets */
static int  VisBaselineSet;
static int  VisResync;           /* set on overflow; forces a clean relearn   */
static au64 VisAdapts;           /* how often it did                         */
static au64 VisResyncs;          /* how many times that happened             */
static au64 StockSince;          /* when stock_ui last went true, 0 if false  */
static au64 VisStuckRecoveries;  /* hidden too long -> forced relearn         */
static au64 VisCloseEdges;       /* hides seen while above the baseline      */
static au64 VisDtorCalls;        /* QWidget destructions seen                */
static au64 VisForgets;          /* destructions that freed a tracked slot   */
static au64 VisSweeps;           /* visibility sweeps run                    */
static au64 VisSweptOut;         /* entries a sweep found no longer visible  */
static au64 VisFreed;            /* tracked widgets caught being deallocated */
static au64 VisSweepAt;          /* next time a sweep may run                */
static au64 VisRef;              /* count with no stock UI open; edge datum  */
/* GlassesUI keeps a couple of ordinary OSD widgets visible at rest and churns
 * show/hide on them constantly (measured: ~3.6k setVisible calls in 25s, with
 * the net count rock steady at 2 over 8s). "Any foreign widget visible" is
 * therefore ALWAYS true and latched the map hidden forever. What distinguishes
 * the stock menu is that it pushes the count ABOVE that resting level, so the
 * baseline is learned once, shortly after arming, and compared against. */
#define A_VIS_BASELINE_MS 2500u
/* How far above the resting level counts as the stock UI, and how the resting
 * level is allowed to move.
 *
 * MEASURED ON HARDWARE: opening the stock menu adds ~85 widgets. Ordinary OSD
 * churn drifts by under 10. The first version of this compared `> baseline`
 * with NO margin, and a +7 drift hid the map with no menu anywhere near -- the
 * operator saw their map vanish for no reason. A margin of 16 separates the two
 * populations with room on both sides.
 *
 * The baseline was also a SINGLE sample taken 2.5 s after arming. If that
 * instant caught the UI mid-churn it latched high (37 observed, against 1-7 in
 * other runs on the same build), and everything downstream inherited it. It now
 * follows the resting level downward and slowly upward, so a bad sample heals
 * instead of persisting for the session. */
/* With the subtree pop in vis_del() the count is EXACT: it returns to the
 * resting level on close, every time, measured as overflow=0 and
 * close_edges=0 across a session. The old margin of 16 existed only to
 * tolerate the drift that no longer happens, and it was too coarse to see a
 * smaller stock page - the SETTINGS screen raises the count by less than 16,
 * so it was never detected and the map drew over it. Sized now for jitter
 * only, not for leaked widgets. */
#define A_VIS_MENU_MARGIN 3u
/* How many newer entries a widget must have before hiding it is read as
 * a subtree close rather than one widget going away. The stock menu
 * brings ~85 children; ordinary churn brings a handful. */
/* How often the visibility sweep may run, on the GUI thread. The stock UI
 * does not appear and vanish faster than this, and it bounds the cost. */
#define A_VIS_SWEEP_MS 200u
#define A_VIS_ADAPT_MS    3000u

/* How long the map may stay hidden by menu detection before the detector is
 * treated as wrong rather than the menu as open.
 *
 * The baseline deliberately cannot climb while `stock_ui` is true, so that a
 * held-open menu cannot absorb itself and become invisible. The cost is that
 * if the foreign-widget count is elevated for any OTHER reason -- Qt does not
 * deliver hide() for children, so the bookkeeping leaks -- `stock_ui` latches
 * true, the baseline can never recover, and the map is hidden FOREVER.
 *
 * Observed on the bench 2026-10-04: a deployed map did not appear, status read
 * map_hidden=1 with menu_hides=3 against menu_shows=2, and pressing MENU
 * (which forces a resync) brought it back.
 *
 * THIS IS A TRADE AND THE NUMBER MATTERS. The first attempt used 30 s, and
 * an operator reported the map hiding only "sometimes" afterwards: a menu
 * genuinely left open past the timeout gets the map back on top of it, which
 * is the opposite failure and a visible regression.
 *
 * Two minutes is chosen so ordinary menu use never reaches it, while a leaked
 * count -- which is permanent -- still heals without a power cycle. The bias
 * stated above is unchanged and still decides the direction: a stale "stock
 * UI present" hides the map FOREVER, a stale "clear" costs a frame of overlap
 * that the next detection corrects. Forever is the worse one.
 *
 * ASCENT_OVERLAY_MAX_HIDE_MS overrides it at the bench without a rebuild. */
/* Backstop only. Edge detection should never need it, so a wrong hide must
 * not persist for two minutes the way the level-based scheme required. */
/* Was 30000. The reported symptom was a map that vanished on arm and came
 * back "after a while"; that while was this. The authoritative value now
 * lives in ascent_vis_policy.h so the policy and its tests cannot disagree
 * with the shim about it. This is the STOPGAP half of the visibility fix
 * and is deliberately independent of the armed-aware half -- either can be
 * reverted without the other. */
#define A_VIS_MAX_HIDE_MS A_VIS_MAX_HIDE_MS_DEF

static void *App;
static void *Label, *Image, *Pixmap;
static a_surface Surface;
static int  MapHidden;            /* the overlay is hidden for a stock dialog */
/* Row stride of the QImage we allocate, in bytes.
 *
 * This WAS QImage::bytesPerLine(). That symbol - _ZNK6QImage13bytesPerLineEv -
 * does not exist in the target's libQt5Gui.so.5.15.10, confirmed on device
 * with nm -D, readelf -Ws and objdump -T, and requiring it was the same defect
 * that stopped the engine starting (see src/ascent_imghdr.h). It was added by
 * the Phase 1 corrective pass for F-SHIM-2/3; the hardware-proven POC shim did
 * not use it.
 *
 * width*4 is EXACT for Qt's 32-bit formats: QImageData::create computes
 * ((w*32+31)>>5)<<2, which for depth 32 is exactly w*4 with no padding. It is
 * also memory-SAFE regardless of that: writing h rows at w*4 touches h*w*4
 * bytes of a buffer Qt sized at bytes_per_line*h >= w*4*h, so a hypothetical
 * padded build could only shear the overlay, never overflow it. */
/* Phase2E: the overlay's geometry is whatever the attached producer
 * publishes, not a compile-time constant. FW/FH track the geometry the
 * CURRENT overlay widgets were built for; they change only when a new
 * producer instance appears with a different size. */
static au32   FW = A_FRAME_W, FH = A_FRAME_H;
static au64   FBufBytes = A_FRAME_BUF_BYTES;
static ausize ScratchBytes;
static au64   GeomChanges, ScratchGrows;
#define A_IMG_STRIDE ((ausize)FW * 4u)

static a_frame_consumer FC;
static au32 *Scratch;
static a_ovl_policy   OVL;
static a_paint_policy PAINT;
static a_cadence      CAD;

/* ---- Picker: a SECOND, independent overlay layer ------------------------- *
 * Multiple widgets live in the one overlay by the shim owning one layer per
 * widget: a layer is {frame channel, QLabel, geometry}. The map above is layer
 * 0. This is layer 1: its own frame channel (/tmp/ascent_picker_frame.bin, our
 * picker producer), its own QLabel parented to the SAME window, its own position
 * (/tmp/ascent_picker_geom). Qt stacks the two child labels, so they coexist on
 * one plane, each placed and toggled independently (map only / picker only /
 * both / none). It deliberately reuses NONE of the map's streaming-specific
 * machinery (cadence, menu-fade, geometry-follow) -- a fleet bar is slow and
 * fixed-size -- so it cannot perturb the proven map path. The generalisation to
 * N widgets is an array of these; two are hardcoded here to keep the diff small
 * and the map path byte-identical. */
#define PICKER_FRAME_PATH "/tmp/ascent_picker_frame.bin"
#define PICKER_GEOM_PATH  "/tmp/ascent_picker_geom"
/* The picker frame follows the producer (the Widget Layout tab sizes it), so
 * PFW/PFH are variables, not constants. The scratch buffer is sized to the max
 * the producer will ever publish (bar-shaped, display-wide), and a take into it
 * fails safe if a frame somehow exceeds it. */
#define A_PICKER_MAX_PX   (1920u * 400u)
/* No picker frame for this long + a changed instance_id == the producer was
 * replaced; re-attach. The producer publishes every ~200 ms, so 1 s is well
 * clear of normal jitter and never trips during steady operation. */
#define A_PICKER_STALE_MS 1000u
static au32  PFW = A_FRAME_W, PFH = A_FRAME_H;
static void *PLabel, *PImage, *PPixmap;
static a_surface PSurface;
static au32  PScratchBuf[A_PICKER_MAX_PX];
static a_frame_consumer PFC;
static int   PFCInit, PBuilt, PHidden;
static void *PParent;
static int   PGeomX, PGeomY, PGeomHave, PGeomReqX, PGeomReqY, PGeomReqHave;
static au64  PNextGeomPoll;
static au64  PBlits, PBuildFail, PTakes;

/* configuration, all env-overridable */
static au32 Cfg_hz      = 15;
static int  Cfg_x       = 0, Cfg_y = 0;
static au32 Cfg_minw    = A_OVL_MIN_W_DEF, Cfg_minh = A_OVL_MIN_H_DEF;
static au32 Cfg_stable  = A_OVL_STABLE_DEF;
static au32 Cfg_cap     = A_OVL_CAP_DEF;
/* PROVEN default. Phase 2 changed this to RGB32 to avoid a per-pixel
 * source-over blend; on hardware that froze the display (see ascent_surface.h).
 * ARGB32 is what the hardware-proven shim used. ASCENT_OVERLAY_FMT=rgb32 opts
 * back in, and is now safe because the pixel base is re-acquired every frame -
 * but it is not the default and has never been shown to work on a device. */
static au32 Cfg_fmt     = QIMAGE_FORMAT_ARGB32;
/* MENU FADE. While a GlassesUI menu is active the map is faded (not hidden) so the
 * menu underneath shows through -- the overlay Label is a CHILD widget, so Qt's
 * raster engine alpha-composites it over the parent (the menu), which only works
 * in ARGB32. "Active" means a menu key arrived within Cfg_menu_fade_ms; the
 * window mirrors GlassesUI's own menu-hide timeout so the map restores exactly
 * when the menu goes away. Fade is forgiving: a stray key just dims the map for a
 * moment instead of making it vanish. */
static au32 Cfg_menu_fade_ms = 4000;     /* ASCENT_MENU_FADE_MS: 0 disables fade */
static au32 Cfg_fade_alpha   = 38;       /* ASCENT_MENU_FADE_ALPHA: ~15% of 255  */
static volatile au64 LastMenuKeyMs;      /* set by the Qt message handler (any thread) */
static int  MenuFade;                    /* current fade state (GUI thread only) */
static au64 MenuFades, MenuFadeEnds, MenuKeysSeen;   /* stats */
static au32 *FadeBuf; static ausize FadeBufBytes;    /* Scratch, alpha-scaled     */
/* 0 = the PROVEN behaviour: raise() once when the overlay is built, never
 * again. Phase 2's 0.5 Hz re-raise put the map back on top of the stock menu
 * every two seconds, which is the reported z-order defect. Re-enable only with
 * evidence that the overlay actually loses z-order. */
static au64 Cfg_raise_ms = 0;
static au64 Cfg_stats_ms = 30000;
static au64 Cfg_stale_ms = 1500;
static au64 Cfg_max_hide_ms = A_VIS_MAX_HIDE_MS;

/* instrumentation */
static au64 Ticks, Blits, Faults, Raises, Posts, PostsDropped, BuildFail;
static au64 PresentSkipped, Hides, Shows;
/* Phase2C menu-detection instrumentation. */
static au64 PopupDetects, ModalDetects, MenuFalsePositives;
static const char *MenuMech = "none";
static au64 NextRaise, NextStats;

/* At most this many queued slot invocations may be outstanding. Phase 1 had no
 * bound, so a stalled GUI thread accumulated work that later ran in a burst. */
#define A_MAX_PENDING 2
static volatile int Pending;

static const char *LP = "/tmp/ascent_frame_overlay.log";
static void logs(const char *s)
{
    int fd = open(LP, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    ausize n = a_strnlen(s, 400);
    if (write(fd, s, n) < 0) {}
    if (write(fd, "\n", 1) < 0) {}
    close(fd);
}
static void logu(const char *a, au64 v)
{
    char b[256];
    a_strlcpy(b, a, sizeof b);
    ausize l = a_strnlen(b, sizeof b);
    char t[24]; int k = 0;
    if (v == 0) t[k++] = '0';
    while (v && k < 20) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k && l + 1 < sizeof b) b[l++] = t[--k];
    b[l] = 0;
    logs(b);
}

/* The Qt message handler. GlassesUI logs every key it receives via qDebug, e.g.
 * "Qt::Key_Left"; those messages pass through here in-process, with no latency and
 * no log-file buffering. We note the time of any key that names a key event
 * ("Key_") -- that is the menu being driven -- and chain to the previous handler
 * so the vendor's logging is completely unchanged. Runs on whatever thread emitted
 * the message; LastMenuKeyMs is a 64-bit aligned store, read on the GUI thread. */
static void ascent_msg_handler(int type, const void *ctx, const void *qstr)
{
    if (Q_utf16 && qstr) {
        const unsigned short *u = Q_utf16(qstr);
        if (u) {
            for (int i = 0; u[i]; i++) {
                if (u[i] == 'K' && u[i+1] == 'e' && u[i+2] == 'y' && u[i+3] == '_') {
                    LastMenuKeyMs = a_now_ms();
                    MenuKeysSeen++;
                    break;
                }
            }
        }
    }
    if (PrevMsgHandler) PrevMsgHandler(type, ctx, qstr);
    else if (Q_msgout)  Q_msgout(type, ctx, qstr);   /* keep vendor output alive */
}

/* ---- configuration ------------------------------------------------------- */
static au32 env_u32(const char *name, au32 dflt, au32 lo, au32 hi)
{
    const char *e = getenv(name);
    if (!e || !e[0]) return dflt;
    au64 v = 0; int any = 0;
    for (const char *s = e; *s; s++) {
        if (*s < '0' || *s > '9') { any = 0; break; }
        any = 1;
        v = v * 10u + (au64)(*s - '0');
        if (v > 0xFFFFFFFFull) { any = 0; break; }
    }
    if (!any) return dflt;
    if ((au32)v < lo) return lo;
    if ((au32)v > hi) return hi;
    return (au32)v;
}

static void load_config(void)
{
    Cfg_hz     = a_cadence_snap(env_u32("ASCENT_OVERLAY_HZ", 15, 1, 1000));
    /* Tunable at the bench: this is a trade between "the map is stuck
     * hidden" and "the map sits on top of a menu left open". */
    Cfg_max_hide_ms = env_u32("ASCENT_OVERLAY_MAX_HIDE_MS",
                              A_VIS_MAX_HIDE_MS, 5000u, 600000u);
    Cfg_x      = (int)env_u32("ASCENT_OVERLAY_X", 0, 0, 8192);
    Cfg_y      = (int)env_u32("ASCENT_OVERLAY_Y", 0, 0, 8192);
    Cfg_minw   = env_u32("ASCENT_OVERLAY_MINW", A_OVL_MIN_W_DEF, 0, 8192);
    Cfg_minh   = env_u32("ASCENT_OVERLAY_MINH", A_OVL_MIN_H_DEF, 0, 8192);
    Cfg_stable = env_u32("ASCENT_OVERLAY_STABLE", A_OVL_STABLE_DEF, 1, 240);
    Cfg_cap    = env_u32("ASCENT_OVERLAY_CAP", A_OVL_CAP_DEF, 1, 100000);
    Cfg_raise_ms = env_u32("ASCENT_OVERLAY_RAISE_MS", 0, 0, 3600000);
    Cfg_stats_ms = env_u32("ASCENT_OVERLAY_STATS_MS", 30000, 0, 3600000);
    Cfg_stale_ms = env_u32("ASCENT_OVERLAY_STALE_MS", 1500, 200, 60000);
    /* Bounds for Phase2D. Prefer what Qt was told; allow an explicit override;
     * fall back to the panel this hardware actually ships. */
    Cfg_scrw = env_u32("ASCENT_OVERLAY_SCRW", 0, 64, 16384);
    Cfg_scrh = env_u32("ASCENT_OVERLAY_SCRH", 0, 64, 16384);
    if (!Cfg_scrw || !Cfg_scrh) {
        const char *qp = getenv("QT_QPA_PLATFORM");
        au32 w = 0, h = 0;
        if (qp) {
            int i = 0;
            while (qp[i]) {
                if (qp[i]=='s'&&qp[i+1]=='i'&&qp[i+2]=='z'&&qp[i+3]=='e'&&qp[i+4]=='=') {
                    int j = i + 5;
                    while (qp[j] >= '0' && qp[j] <= '9') w = w*10 + (au32)(qp[j++]-'0');
                    if (qp[j] == 'x') {
                        j++;
                        while (qp[j] >= '0' && qp[j] <= '9') h = h*10 + (au32)(qp[j++]-'0');
                    }
                    break;
                }
                i++;
            }
        }
        if (w >= 64 && h >= 64) { Cfg_scrw = w; Cfg_scrh = h; }
        else { Cfg_scrw = 1920; Cfg_scrh = 1080; }
    }
    Cfg_allow_clip = (int)env_u32("ASCENT_OVERLAY_ALLOW_CLIP", 0, 0, 1);
    {
        const char *f = getenv("ASCENT_OVERLAY_FMT");
        Cfg_fmt = (f && f[0] == 'r') ? QIMAGE_FORMAT_RGB32 : QIMAGE_FORMAT_ARGB32;
    }
    Cfg_menu_fade_ms = env_u32("ASCENT_MENU_FADE_MS", 4000, 0, 60000);
    Cfg_fade_alpha   = env_u32("ASCENT_MENU_FADE_ALPHA", 38, 0, 255);
}

/* ---- SIGBUS guard -------------------------------------------------------- */
#define SIGBUS_NO 7
extern int  __sigsetjmp(void *, int);
extern void siglongjmp(void *, int) __attribute__((noreturn));
typedef void (*sighandler_t)(int);
extern sighandler_t signal(int, sighandler_t);

static long JB[64] __attribute__((aligned(16)));
static volatile int Armed;
static sighandler_t PrevBus;

static void on_sigbus(int s)
{
    if (Armed) { Armed = 0; Faults++; siglongjmp(JB, 1); }
    if (PrevBus && PrevBus != (sighandler_t)0 && PrevBus != (sighandler_t)1) { PrevBus(s); return; }
    signal(s, (sighandler_t)0);          /* SIG_DFL */
}

/* ---- Qt symbol resolution ------------------------------------------------ */
static int rs(const char *n, void **p){ *p = dlsym((void*)0 /*RTLD_DEFAULT*/, n); return *p != 0; }
#define R(v,n) do{ if(!rs(n,(void**)&v)){ logs("SHIM FAIL resolve " n); return 0; } }while(0)

static int resolve(void)
{
    R(Q_active,"_ZN12QApplication12activeWindowEv");
    R(Q_new,   "_Znwm");
    R(Q_label, "_ZN6QLabelC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE");
    R(Q_attr,  "_ZN7QWidget12setAttributeEN2Qt15WidgetAttributeEb");
    R(Q_focus, "_ZN7QWidget14setFocusPolicyEN2Qt11FocusPolicyE");
    R(Q_align, "_ZN6QLabel12setAlignmentE6QFlagsIN2Qt13AlignmentFlagEE");
    R(Q_fixed, "_ZN7QWidget12setFixedSizeEii");
    R(Q_show,  "_ZN7QWidget4showEv");
    R(Q_raise, "_ZN7QWidget5raiseEv");
    R(Q_update,"_ZN7QWidget6updateEv");
    R(Q_img,   "_ZN6QImageC1EiiNS_6FormatE");
    R(Q_img_dtor,"_ZN6QImageD1Ev");
    R(Q_bits,  "_ZN6QImage4bitsEv");
    R(Q_pm_ctor,"_ZN7QPixmapC1Ev");
    R(Q_pmconv,"_ZN7QPixmap16convertFromImageERK6QImage6QFlagsIN2Qt19ImageConversionFlagEE");
    R(Q_setpix,"_ZN6QLabel9setPixmapERK7QPixmap");
    R(Q_invoke,"_ZN11QMetaObject16invokeMethodImplEP7QObjectPN9QtPrivate15QSlotObjectBaseEN2Qt14ConnectionTypeEPv");

    /* OPTIONAL, and deliberately non-fatal.
     * move() only makes the overlay position deterministic; without it the
     * label lands wherever the parent puts it, which is Phase 1 behaviour.
     * geometry() drives the popup filter; without it every candidate is
     * treated as large enough and we fall back to the stability window alone.
     * Neither is worth refusing to start over. */
    /* Legacy lookups, RETAINED so a bench log still shows them failing and the
     * record stays honest. Both name INLINE Qt functions, so both have always
     * been NULL and always will be. Phase2C supersedes them below. */
    rs("_ZN7QWidget4moveEii", (void**)&Q_move);
    rs("_ZNK7QWidget8geometryEv", (void**)&Q_geom);
    /* OPTIONAL z-order fallback. When a stock dialog or menu takes the active
     * window we hide the map and restore it on return, so the stock UI is
     * never obscured. Without this symbol we simply never re-raise, which is
     * the hardware-proven behaviour and already leaves later-shown widgets on
     * top. Nothing is hard-required. */
    rs("_ZN7QWidget4hideEv", (void**)&Q_hide);

    /* ---- Phase2C: the forms the device actually exports -----------------
     * Verified against the device's own /usr/lib/libQt5Widgets.so.5.15.10
     * (RK3568 rootfs, 19.2.14) AND against GlassesUI's own import table -
     * GlassesUI already calls move(QPoint), resize(QSize), setGeometry(QRect)
     * and installEventFilter, so these are bound in this very process.
     *
     * Every one is optional. Phase2C RESOLVES and REPORTS them; it calls only
     * the menu detectors. Position is Phase2D, resize is Phase2E. */
    rs("_ZN7QWidget4moveERK6QPoint",            (void**)&Q_move_pt);
    rs("_ZN7QWidget6resizeERK5QSize",           (void**)&Q_resize_sz);
    rs("_ZN7QWidget11setGeometryERK5QRect",     (void**)&Q_setgeom);
    rs("_ZN12QApplication17activePopupWidgetEv",(void**)&Q_popup);
    rs("_ZN12QApplication17activeModalWidgetEv",(void**)&Q_modal);

    /* Phase2C-rev. RTLD_NEXT, NOT RTLD_DEFAULT: we now EXPORT these three
     * names ourselves, so a default lookup would find the shim and recurse.
     * Q_show/Q_hide are re-pointed at the real implementations for the same
     * reason - the overlay must never trip its own visibility accounting. */
    vis_bind();   /* idempotent; normally already done by an interposed call */
    if (Real_show) Q_show = Real_show;
    if (Real_hide) Q_hide = Real_hide;

    logs(Q_hide ? "SHIM QWidget::hide resolved (map yields to stock dialogs)"
                : "SHIM QWidget::hide NOT resolved - map will not be hidden for dialogs");

    /* Geometry capability probe. Reported, not used, in Phase2C. */
    logs(Q_move_pt   ? "SHIM CAP move(QPoint) resolved"
                     : "SHIM CAP move(QPoint) NOT resolved");
    logs(Q_setgeom   ? "SHIM CAP setGeometry(QRect) resolved - Phase2D path available"
                     : "SHIM CAP setGeometry(QRect) NOT resolved");
    logs(Q_resize_sz ? "SHIM CAP resize(QSize) resolved - Phase2E path available"
                     : "SHIM CAP resize(QSize) NOT resolved");
    /* The two legacy names, for the record. Expected: both NOT resolved. */
    logs(Q_move ? "SHIM legacy move(int,int) resolved (unexpected)"
                : "SHIM legacy move(int,int) NOT resolved (expected: inline in Qt headers)");

    /* Menu detection mechanism, decided once and named in the log so a bench
     * run never has to guess which path was active.
     *
     * Why this replaces the activeWindow() heuristic: a Qt popup does NOT
     * become the active window, so activeWindow() could never see a QMenu.
     * That is why Phase2B measured map_hides=0 with the menu visibly on top.
     * GlassesUI builds its menus as QMenu subclasses and its dialogs as modal
     * QDialog (exec()), which is exactly what these two APIs report. */
    if (Real_show || Real_setvis) {
        MenuMech = "visibility-interposition (show/hide/setVisible)";
    } else if (Q_popup && Q_modal) {
        MenuMech = "popup+modal";
        logs("SHIM menu_detection_mechanism=popup+modal (QMenu and modal QDialog)");
    } else if (Q_popup) {
        MenuMech = "popup";
        logs("SHIM menu_detection_mechanism=popup (QMenu only; modal undetected)");
    } else if (Q_modal) {
        MenuMech = "modal";
        logs("SHIM menu_detection_mechanism=modal (modal QDialog only; popups undetected)");
    } else {
        MenuMech = "activeWindow-fallback";
        logs("SHIM menu_detection_mechanism=activeWindow-fallback "
             "(neither detector resolved; Phase2B behaviour, expect menu_hides=0)");
    }
    if (Q_geom) logs("SHIM QWidget::geometry resolved (popup filter active)");
    else {
        logs("SHIM WARN QWidget::geometry NOT resolved - popup filter DISABLED, "
             "falling back to the stability window only");
        OVL.min_w = 0; OVL.min_h = 0;
    }
    /* MENU-KEY detector (OPTIONAL). Installs a Qt message handler so the root
     * menu -- which creates no widgets the visibility detector can see -- is
     * caught through the key events GlassesUI already logs. Qt 5.15 arm64 mangled
     * names, verified on the device. If either is missing, fade is simply off and
     * everything else is unchanged. */
    rs("_Z22qInstallMessageHandlerPFv9QtMsgTypeRK18QMessageLogContextRK7QStringE",
       (void**)&Q_installmsg);
    rs("_ZNK7QString5utf16Ev", (void**)&Q_utf16);
    rs("_Z17qt_message_output9QtMsgTypeRK18QMessageLogContextRK7QString",
       (void**)&Q_msgout);
    if (Cfg_menu_fade_ms && Q_installmsg && Q_utf16) {
        PrevMsgHandler = Q_installmsg(ascent_msg_handler);
        logs("SHIM menu-key fade detector installed (qInstallMessageHandler)");
    } else {
        logs("SHIM menu-key fade detector NOT active "
             "(symbols missing or ASCENT_MENU_FADE_MS=0)");
    }

    logs("SHIM Qt symbols resolved");
    return 1;
}

/* Candidate geometry. Returns 1 and fills w/h when known. */
static int cand_size(void *w, int *cw, int *ch)
{
    if (!Q_geom || !w) { *cw = 1 << 20; *ch = 1 << 20; return 0; }
    const int *r = Q_geom(w);        /* QRect {x1,y1,x2,y2} */
    if (!r) { *cw = 1 << 20; *ch = 1 << 20; return 0; }
    long x1 = r[0], y1 = r[1], x2 = r[2], y2 = r[3];
    long ww = x2 - x1 + 1, hh = y2 - y1 + 1;
    if (ww < 0) ww = 0;
    if (hh < 0) hh = 0;
    if (ww > (1 << 20)) ww = 1 << 20;
    if (hh > (1 << 20)) hh = 1 << 20;
    *cw = (int)ww; *ch = (int)hh;
    return 1;
}

/* The surface acquirer: the CURRENT pixel base of the live QImage.
 *
 * QImage::bits() is non-const and detaches, so calling it per frame is what
 * guarantees the buffer we write is one this image privately owns rather than
 * one it has since shared with the QPixmap. Caching the first result is what
 * froze the display on hardware. */
static au8 *surface_acquire(void *user)
{
    (void)user;
    if (!Image || !Q_bits) return ASCENT_NULL;
    return Q_bits(Image);
}

/* Build a fresh overlay under `parent`. The previous one is abandoned, never
 * dereferenced: see the header note. */
static int build_overlay(void *parent)
{
    void *lab = Q_new(4096), *img = Q_new(4096), *pm = Q_new(4096);
    if (!lab || !img || !pm) return 0;
    Q_label(lab, parent, 0);
    Q_attr(lab, QT_WA_TRANSPARENT_FOR_MOUSE_EVENTS, 1);
    Q_focus(lab, QT_NO_FOCUS);
    Q_align(lab, QT_ALIGN_CENTER);
    Q_fixed(lab, (int)FW, (int)FH);
    /* Explicit position: it makes the invalidated rectangle deterministic and
     * documentable, which is what FLICKER_RENDER_PATH_ANALYSIS.md reasons
     * about. A parent with a layout may still override it. */
    /* Phase2D: position through the exported QRect form. Q_move names the
     * inline move(int,int) and has always been NULL; it is probed and logged
     * only so the record stays honest. Label is assigned just below, and
     * geom_apply() needs it, so the initial placement happens after that. */
    if (Q_move) Q_move(lab, Cfg_x, Cfg_y);   /* never taken: inline symbol */
    Q_img(img, (int)FW, (int)FH, (int)Cfg_fmt);
    /* Probe once so a broken image fails the build rather than every frame.
     * The value is NOT kept: a_surface re-acquires before every copy. */
    if (!Q_bits(img)) return 0;
    if (!Scratch) return 0;
    Q_pm_ctor(pm);
    Q_show(lab);
    /* raise() ONCE, exactly as the hardware-proven shim did. A periodic
     * re-raise is what put the map above the stock menu. */
    Q_raise(lab);

    Label = lab; Image = img; Pixmap = pm;
    /* Phase2C-rev: everything visible right now is background - the stock main
     * window above all. Arm only from here, so the tracker counts widgets that
     * appear LATER. Without this, stock_ui would latch true at startup. */
    VisArmed = 1;
    VisArmedAt = a_now_ms();
    /* Phase2D initial placement from env, bounds-checked like any later one. */
    geom_apply(Cfg_x, Cfg_y);
    MapHidden = 0;
    /* Stride is FW*4: exact for Qt's 32-bit formats, and memory-safe
     * regardless (see ascent_surface.h / QT_SYMBOL_SURFACE.md). */
    a_surface_init(&Surface, surface_acquire, ASCENT_NULL,
                   FW, FH, A_IMG_STRIDE);
    return 1;
}

/* The banner is drawn into the PACKED scratch buffer, never directly into the
 * QImage. a_canvas assumes a packed stride, and - more importantly since the
 * frozen-display fix - the QImage's pixel base is not valid outside
 * a_surface_write(). Scratch is the only stable pixel source. */
static void banner(const char *msg, au32 col)
{
    a_canvas cv = { Scratch, FW, FH };
    a_rect(&cv, 0, 0, (int)FW, 22, 0xC0000000);
    a_text(&cv, 6, 3, msg, 40, 2, col, 0xFF000000);
}

/* Push whatever is in Scratch to the screen. The ONLY place that touches the
 * widget, and the only place that requests invalidation.
 *
 * Order matters and is the whole fix: re-acquire the destination, copy into
 * it, and only then hand the image to the pixmap. If the acquire fails we do
 * NOT present, because presenting would re-show whatever Qt still holds - the
 * exact frozen-frame symptom. */
static void publish_pixels(void)
{
    const au32 *src = Scratch;
    /* While a menu is active, publish an alpha-scaled copy so the menu underneath
     * shows through. Scaled into FadeBuf, never in place, so the pristine Scratch
     * survives for the next frame and for banner/re-publish. ARGB32 only (RGB32
     * ignores the alpha byte, so there would be nothing to scale). */
    if (MenuFade && FadeBuf && FadeBufBytes >= ScratchBytes
            && Cfg_fmt == QIMAGE_FORMAT_ARGB32) {
        ausize n = (ausize)FW * (ausize)FH;
        au32 a = Cfg_fade_alpha & 0xFFu;
        for (ausize i = 0; i < n; i++) {
            au32 px = Scratch[i];
            au32 sa = ((px >> 24) * a) / 255u;
            FadeBuf[i] = (px & 0x00FFFFFFu) | (sa << 24);
        }
        src = FadeBuf;
    }
    if (!a_surface_write(&Surface, src)) { PresentSkipped++; return; }
    if (Q_pmconv(Pixmap, Image, 0)) {
        Q_setpix(Label, Pixmap);
        /* setPixmap already schedules an update; Qt coalesces both into one
         * paint event, so this costs a call and no extra repaint. */
        Q_update(Label);
        Blits++;
    }
}

/* Guarded take. The SIGBUS guard lives in its own function so each
 * __sigsetjmp has its own scope - two of them in one function makes every
 * later local a potential clobber victim, which -Werror=clobbered rightly
 * refuses.
 *
 * Returns 1 when a new frame landed in Scratch, 0 otherwise. On SIGBUS the
 * mapping is dropped and 0 is returned; the next tick re-attaches. */
static int guarded_take(au64 *seq_out)
{
    Armed = 1;
    if (__sigsetjmp(JB, 1) == 0) {
        int ok = (a_frame_consumer_take(&FC, Scratch, ScratchBytes)
                  == A_FRAME_TAKE_OK);
        Armed = 0;
        if (ok && seq_out) *seq_out = FC.last_seq;
        return ok;
    }
    Armed = 0;
    a_frame_consumer_detach(&FC);
    logs("SHIM SIGBUS trapped in blit; mapping dropped, will re-attach");
    return 0;
}

static void emit_stats(au64 now)
{
    logu("SHIM STAT ticks=",        Ticks);
    logu("SHIM STAT blits=",        Blits);
    logu("SHIM STAT paint_skips=",  PAINT.skips);
    logu("SHIM STAT frame_paints=", PAINT.frame_paints);
    logu("SHIM STAT banner_paints=",PAINT.banner_paints);
    logu("SHIM STAT seq_regress=",  PAINT.regressions);
    logu("SHIM STAT ovl_builds=",   OVL.builds);
    logu("SHIM STAT ovl_keeps=",    OVL.keeps);
    logu("SHIM STAT ovl_idles=",    OVL.idles);
    logu("SHIM STAT ovl_popup_rej=",OVL.rej_small);
    logu("SHIM STAT ovl_unstable=", OVL.rej_unstable);
    logu("SHIM STAT ovl_nullgaps=", OVL.null_gaps);
    logu("SHIM STAT ovl_lost=",     OVL.lost_windows);
    logu("SHIM STAT ovl_capped=",   OVL.capped_events);
    logu("SHIM STAT build_fail=",   BuildFail);
    logu("SHIM STAT raises=",       Raises);
    logu("SHIM STAT present_skipped=", PresentSkipped);
    logu("SHIM STAT surf_acquires=",   Surface.acquires);
    logu("SHIM STAT surf_writes=",     Surface.writes);
    logu("SHIM STAT surf_rebinds=",    Surface.rebinds);
    logu("SHIM STAT surf_acq_fail=",   Surface.acquire_failures);
    /* Phase2C canonical names. map_hides/map_shows are kept as aliases so the
     * existing VRX_RETEST.md greps still work; they are the same counters. */
    logu("SHIM STAT menu_hides=",           Hides);
    logu("SHIM STAT menu_shows=",           Shows);
    logu("SHIM STAT menu_fades=",           MenuFades);
    logu("SHIM STAT menu_fade_ends=",       MenuFadeEnds);
    logu("SHIM STAT menu_keys_seen=",       MenuKeysSeen);
    logu("SHIM STAT menu_fade_active=",     (au64)MenuFade);
    logu("SHIM STAT popup_detects=",        PopupDetects);
    logu("SHIM STAT modal_detects=",        ModalDetects);
    logu("SHIM STAT menu_false_positives=", MenuFalsePositives);
    logu("SHIM STAT vis_show_calls=",       VisShowCalls);
    logu("SHIM STAT vis_hide_calls=",       VisHideCalls);
    logu("SHIM STAT vis_setvisible_calls=", VisSetVisCalls);
    logu("SHIM STAT vis_foreign_visible=",  VisForeign);
    logu("SHIM STAT vis_foreign_peak=",     VisPeak);
    logu("SHIM STAT vis_overflow=",         VisOverflow);
    logu("SHIM STAT vis_bind_fail=",        VisBindFail);
    logu("SHIM STAT vis_baseline=",         VisBaseline);
    logu("SHIM STAT vis_resyncs=",          VisResyncs);
    logu("SHIM STAT vis_dtor_calls=",       VisDtorCalls);
    logu("SHIM STAT vis_forgets=",          VisForgets);
    logu("SHIM STAT vis_sweeps=",           VisSweeps);
    logu("SHIM STAT vis_swept_out=",        VisSweptOut);
    logu("SHIM STAT vis_freed=",            VisFreed);
    logu("SHIM STAT vis_stuck_recoveries=", VisStuckRecoveries);
    logu("SHIM STAT vis_close_edges=",      VisCloseEdges);
    logu("SHIM STAT vis_adapts=",           VisAdapts);
    logu("SHIM STAT vis_menu_margin=",      (au64)A_VIS_MENU_MARGIN);
    logu("SHIM STAT geom_applies=",         GeomApplies);
    logu("SHIM STAT geom_clamps=",          GeomClamps);
    logu("SHIM STAT geom_rejects=",         GeomRejects);
    logu("SHIM STAT geom_reads=",           GeomReads);
    logu("SHIM STAT geom_parse_fail=",      GeomParseFail);
    logu("SHIM STAT geom_req_x=",           (au64)(unsigned)GeomReqX);
    logu("SHIM STAT geom_req_y=",           (au64)(unsigned)GeomReqY);
    logu("SHIM STAT geom_x=",               (au64)(unsigned)GeomX);
    logu("SHIM STAT geom_y=",               (au64)(unsigned)GeomY);
    logu("SHIM STAT geom_scrw=",            Cfg_scrw);
    logu("SHIM STAT geom_scrh=",            Cfg_scrh);
    logu("SHIM STAT frame_w=",              (au64)FW);
    logu("SHIM STAT frame_h=",              (au64)FH);
    logu("SHIM STAT geom_changes=",         GeomChanges);
    logu("SHIM STAT scratch_grows=",        ScratchGrows);
    logu("SHIM STAT scratch_bytes=",        (au64)ScratchBytes);
    logu("SHIM STAT status_writes=",        StatusWrites);
    logu("SHIM STAT status_fails=",         StatusFails);
    logu("SHIM STAT map_hides=",       Hides);
    logu("SHIM STAT map_shows=",       Shows);
    logu("SHIM STAT posts=",        Posts);
    logu("SHIM STAT posts_dropped=",PostsDropped);
    logu("SHIM STAT cad_late=",     CAD.late);
    logu("SHIM STAT sigbus=",       Faults);
    logu("SHIM STAT frames_taken=", FC.frames_taken);
    logu("SHIM STAT remaps=",       FC.remaps);
    logu("SHIM STAT torn_retries=", FC.torn_retries);
    logu("SHIM STAT torn_giveups=", FC.torn_giveups);
    (void)now;
}

/* ---- the tick, always on the Qt GUI thread ------------------------------- */
/* Append "<key>=<v>\n" to a bounded buffer. No libc printf: this library is
 * built -nostdlib and resolves everything from the host process. */
static ausize kv(char *b, ausize off, ausize cap, const char *k, au64 v)
{
    ausize l = a_strnlen(k, 64);
    if (off + l + 24 >= cap) return off;
    for (ausize i = 0; i < l; i++) b[off++] = k[i];
    b[off++] = '=';
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v && n < 20) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) b[off++] = t[--n];
    b[off++] = '\n';
    return off;
}

static ausize kvs(char *b, ausize off, ausize cap, const char *k, const char *v)
{
    ausize l = a_strnlen(k, 64), m = a_strnlen(v, 96);
    if (off + l + m + 3 >= cap) return off;
    for (ausize i = 0; i < l; i++) b[off++] = k[i];
    b[off++] = '=';
    for (ausize i = 0; i < m; i++) b[off++] = v[i];
    b[off++] = '\n';
    return off;
}

/* Rewrite the status file in place. Truncating a file nothing mmaps is safe -
 * this is plain text read with open/read/close, not the frame transport. */
static void status_publish(au64 now)
{
    if (now < NextStatus) return;
    NextStatus = now + 2000;
    char b[768]; ausize o = 0;
    o = kv (b, o, sizeof b, "status_version", 1);
    /* Capabilities: what the dynamic linker actually gave us, not a guess. */
    o = kv (b, o, sizeof b, "cap_position",   Q_setgeom ? 1 : 0);
    o = kv (b, o, sizeof b, "cap_resize",     (Q_setgeom || Q_resize_sz) ? 1 : 0);
    o = kv (b, o, sizeof b, "cap_menu_aware", (Real_show || Real_setvis) ? 1 : 0);
    o = kvs(b, o, sizeof b, "menu_mechanism", MenuMech);
    /* Live state. */
    o = kv (b, o, sizeof b, "frame_w",   (au64)FW);
    o = kv (b, o, sizeof b, "frame_h",   (au64)FH);
    o = kv (b, o, sizeof b, "x",         (au64)(unsigned)GeomX);
    o = kv (b, o, sizeof b, "y",         (au64)(unsigned)GeomY);
    o = kv (b, o, sizeof b, "attached",  FC.map ? 1 : 0);
    o = kv (b, o, sizeof b, "map_hidden", MapHidden ? 1 : 0);
    o = kv (b, o, sizeof b, "screen_w",  Cfg_scrw);
    o = kv (b, o, sizeof b, "screen_h",  Cfg_scrh);
    o = kv (b, o, sizeof b, "geom_applies", GeomApplies);
    o = kv (b, o, sizeof b, "menu_hides",   Hides);
    o = kv (b, o, sizeof b, "menu_shows",   Shows);
    o = kv (b, o, sizeof b, "blits",        Blits);
    o = kv (b, o, sizeof b, "vis_stuck_recoveries", VisStuckRecoveries);
    int fd = open(SP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { StatusFails++; return; }
    ausize w = 0;
    while (w < o) {
        assize r = write(fd, b + w, o - w);
        if (r <= 0) break;
        w += (ausize)r;
    }
    close(fd);
    StatusWrites++;
}

/* Grow the scratch buffer to hold one frame of the attached geometry. Never
 * shrinks: a smaller producer after a larger one keeps the bigger buffer,
 * which costs memory but removes a free/realloc from the path that the SIGBUS
 * handler can longjmp out of. Returns 0 if the buffer could not be provided,
 * in which case the caller must not take a frame. */
static int scratch_ensure(au64 bytes)
{
    if (Scratch && ScratchBytes >= (ausize)bytes) return 1;
    au32 *n = (au32*)malloc((ausize)bytes);
    if (!n) { logs("SHIM FAIL scratch grow"); return 0; }
    a_memset(n, 0, (ausize)bytes);
    if (Scratch) free(Scratch);
    Scratch = n;
    ScratchBytes = (ausize)bytes;
    ScratchGrows++;
    /* The fade output buffer tracks Scratch's size. A failure here only disables
     * fade (publish falls back to the pristine Scratch), never the overlay. */
    {
        au32 *fn2 = (au32*)malloc((ausize)bytes);
        if (fn2) { if (FadeBuf) free(FadeBuf); FadeBuf = fn2; FadeBufBytes = (ausize)bytes; }
    }
    return 1;
}

/* ---- Phase2D helpers ----------------------------------------------------
 * Qt's QRect is {x1,y1,x2,y2} with x2/y2 INCLUSIVE, not {x,y,w,h}. Getting
 * that wrong yields a widget one pixel too large in each axis, which is
 * exactly the sort of thing that looks like it works. */
static void a_qrect(int *r, int x, int y, int w, int h)
{
    r[0] = x; r[1] = y; r[2] = x + w - 1; r[3] = y + h - 1;
}

/* Clamp so the widget stays wholly on the display surface. Bounds come from
 * what Qt was actually told (QT_QPA_PLATFORM=linuxfb:size=WxH) rather than
 * from a product-name lookup or a guess. The fb0 sysfs attributes are not
 * readable on this unit, which is why the bridge reports resolution_known
 * false. Returns 1 if the request had to be altered. */
static int geom_clamp(int *x, int *y)
{
    int maxx, maxy, ox = *x, oy = *y;
    int w = (int)FW, h = (int)FH;
    if (Cfg_allow_clip) {
        /* Explicit opt-in: allow partial off-screen, but never fully gone. */
        if (*x < -(w - 1)) *x = -(w - 1);
        if (*y < -(h - 1)) *y = -(h - 1);
        if (*x > (int)Cfg_scrw - 1) *x = (int)Cfg_scrw - 1;
        if (*y > (int)Cfg_scrh - 1) *y = (int)Cfg_scrh - 1;
    } else {
        maxx = (int)Cfg_scrw - w; maxy = (int)Cfg_scrh - h;
        if (maxx < 0) maxx = 0;
        if (maxy < 0) maxy = 0;
        if (*x < 0) *x = 0;
        if (*y < 0) *y = 0;
        if (*x > maxx) *x = maxx;
        if (*y > maxy) *y = maxy;
    }
    return (*x != ox) || (*y != oy);
}

/* Submit a position. GUI thread only - tick() is delivered there by
 * QMetaObject::invokeMethodImpl with a queued connection, which is the same
 * path the proven hide/show/raise calls already use. */
static void geom_apply(int x, int y)
{
    int r[4];
    if (!Label || !Q_setgeom) { GeomRejects++; return; }
    /* Idempotency is on the REQUEST, not the applied value. Counting clamps
     * before any idempotency test re-counted a clamped value resting in the
     * poll file at every 4 Hz read (observed: 173 "clamps" for 6 real moves);
     * testing after the clamp instead hid a genuine clamp whenever it landed
     * on the position already held. Keying on the request gets both right:
     * one count per distinct request, and a request that had to be altered is
     * recorded even when the resulting position does not move. */
    int clamped;
    if (GeomReqHave && x == GeomReqX && y == GeomReqY) return;
    GeomReqX = x; GeomReqY = y; GeomReqHave = 1;
    clamped = geom_clamp(&x, &y);
    if (clamped) GeomClamps++;
    if (GeomHave && x == GeomX && y == GeomY) return;   /* already there */
    a_qrect(r, x, y, (int)FW, (int)FH);
    Q_setgeom(Label, r);
    GeomX = x; GeomY = y; GeomHave = 1; GeomApplies++;
    logu("SHIM geom applied x=", (au64)(unsigned)x);
    logu("SHIM geom applied y=", (au64)(unsigned)y);
}

/* Provisional live input: one small file holding "<x> <y>".
 * This deliberately mirrors the already-proven zoom mechanism
 * (/tmp/ascent_map_zoom, polled by the engine) rather than inventing a second
 * style. It is NOT the product control plane - Phase2F replaces it with the
 * versioned runtime transport. It exists because Phase2D's own acceptance
 * criteria require the map to move with no GlassesUI restart. */
static void geom_poll(au64 now)
{
    char b[64];
    int fd, n, i, sign, val, got, v[2];
    if (now < GeomNextPoll) return;
    GeomNextPoll = now + 250;
    fd = open(GP, O_RDONLY);
    if (fd < 0) return;
    n = (int)read(fd, b, sizeof b - 1);
    close(fd);
    GeomReads++;
    if (n <= 0) { GeomParseFail++; return; }
    b[n] = 0;
    i = 0; got = 0;
    while (got < 2) {
        while (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r') i++;
        sign = 1;
        if (b[i] == '-') { sign = -1; i++; }
        if (b[i] < '0' || b[i] > '9') break;
        val = 0;
        while (b[i] >= '0' && b[i] <= '9') { val = val * 10 + (b[i] - '0'); i++; }
        v[got++] = sign * val;
    }
    if (got != 2) { GeomParseFail++; return; }
    geom_apply(v[0], v[1]);
}

/* ---- flight state and the manual override -------------------------------
 * Both mirror geom_poll above: a tiny file under /tmp, polled on the shim
 * thread, parsed defensively, absent means "not known" rather than an error.
 *
 * FLIGHT STATE is published by the map engine on every MAVLink heartbeat as
 * "<armed> <monotonic_ms>". The timestamp is the load-bearing half: a reading
 * older than two heartbeats is UNKNOWN, and unknown falls back to the menu
 * heuristic. It never means armed. That is what stops a disconnected link from
 * leaving the map pinned on top of the stock UI forever.
 *
 * THE MANUAL OVERRIDE is the operator's escape hatch for the case where this
 * policy is wrong in a way nobody predicted. It changes visibility and nothing
 * else -- not geometry, not the zoom mapping, not the installed package. AUTO
 * is the resting state and the one the unit boots in, because a missing file
 * parses as 0 and 0 is A_VIS_MANUAL_AUTO. */
static const char *FSP = "/tmp/ascent_flight_state";
static const char *MVP = "/tmp/ascent_overlay_visible";
static int  FlightKnown, FlightArmed;
static au64 FlightAtMs, FlightNextPoll, FlightReads, FlightParseFail;
static int  ManualVis;                      /* A_VIS_MANUAL_* */
static au64 ManualNextPoll, ManualReads, ArmedSuppressions;

/* Read up to n-1 bytes of a small file. Returns the length, or -1. */
static int read_small(const char *path, char *b, int n)
{
    int fd, r;
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    r = (int)read(fd, b, (ausize)(n - 1));
    close(fd);
    if (r < 0) return -1;
    b[r] = 0;
    return r;
}

static void flight_poll(au64 now)
{
    char b[64];
    int n, i, got;
    au64 v[2];
    if (now < FlightNextPoll) return;
    FlightNextPoll = now + 250;
    n = read_small(FSP, b, (int)sizeof b);
    if (n <= 0) {
        /* No file at all. The engine may not be running, or may not have seen
         * a heartbeat yet. Both are "not known", and neither is an error. */
        FlightKnown = 0;
        return;
    }
    FlightReads++;
    i = 0; got = 0;
    while (got < 2) {
        while (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r') i++;
        if (b[i] < '0' || b[i] > '9') break;
        v[got] = 0;
        while (b[i] >= '0' && b[i] <= '9') { v[got] = v[got] * 10 + (au64)(b[i] - '0'); i++; }
        got++;
    }
    if (got != 2) { FlightParseFail++; FlightKnown = 0; return; }
    FlightArmed = v[0] ? 1 : 0;
    FlightAtMs = v[1];
    FlightKnown = 1;
}

static void manual_poll(au64 now)
{
    char b[16];
    int n;
    if (now < ManualNextPoll) return;
    ManualNextPoll = now + 250;
    n = read_small(MVP, b, (int)sizeof b);
    if (n <= 0) { ManualVis = A_VIS_MANUAL_AUTO; return; }
    ManualReads++;
    /* One character is enough and keeps the parse unambiguous: anything that is
     * not an explicit show or hide is AUTO, so a truncated or garbled write
     * returns control to the policy rather than wedging the map. */
    if (b[0] == 's' || b[0] == 'S' || b[0] == '1') ManualVis = A_VIS_MANUAL_SHOW;
    else if (b[0] == 'h' || b[0] == 'H' || b[0] == '2') ManualVis = A_VIS_MANUAL_HIDE;
    else ManualVis = A_VIS_MANUAL_AUTO;
}

/* ---- Phase2C-rev helpers ------------------------------------------------
 * Lock-free membership over a fixed array. Called from the GUI thread; read
 * from the shim thread. Pointer-sized CAS is all the synchronisation needed:
 * a miss costs one tick of accuracy, never correctness of the chain-through. */
static int vis_is_ours(void *w)
{
    return w == ASCENT_NULL || w == Label || w == OVL.current;
}

static void vis_add(void *w)
{
    int i;
    if (!VisArmed || vis_is_ours(w)) return;
    for (i = 0; i < VisTop; i++)             /* already tracked? */
        if (VisSet[i] == w) return;
    if (VisTop < A_VIS_MAX) {
        au64 n;
        VisSet[VisTop] = w;
        __sync_synchronize();                /* slot visible before the count */
        VisTop++;
        n = __sync_add_and_fetch(&VisForeign, 1);
        if (n > VisPeak) VisPeak = n;
        return;
    }
    VisOverflow++;   /* more than A_VIS_MAX at once: report, never block */
    VisResync = 1;   /* accounting is now untrustworthy - rebuild it */
}

static void vis_del(void *w)
{
    int i, j;
    if (!VisArmed || vis_is_ours(w)) return;
    for (i = 0; i < VisTop; i++) {
        if (VisSet[i] != w) continue;

        /* Qt does NOT call hide() on the children of a widget being hidden -
         * they keep their own visible flag and simply stop rendering. Measured
         * on this firmware: opening the stock menu raises the count by 85 and
         * closing it delivers exactly ONE hide, the menu root's. The other 85
         * stayed in the set forever, the count could never fall back to the
         * resting level, and the map latched hidden. Every mechanism layered
         * on top of that - the close-edge resync, the overflow resync, the
         * 120s stuck recovery - was compensating for this one fact.
         *
         * Qt shows a parent before its children, so a subtree occupies a
         * CONTIGUOUS RUN of the arrival order starting at its root. Hiding the
         * root therefore retires the run from i to the top. That is the whole
         * fix: the count returns to exactly its pre-menu level on close, with
         * no relearning, no resync and no timeout involved.
         *
         * A widget hidden individually, out of stack order, also drops
         * whatever was shown after it. That direction of error is the safe
         * one - it UNDERCOUNTS, which shows the map; overcounting is what hid
         * it - and the next show() restores the entry. */
        /* Retire exactly this widget. Its children are NOT inferred: Qt
         * clears their visible attribute without calling hide(), and
         * vis_sweep() sees that within A_VIS_SWEEP_MS and removes them for
         * real. Guessing the subtree from arrival order was tried and failed
         * in both directions - it latched the map hidden when it under-removed
         * and stopped the map hiding at all when it over-removed. */
        for (j = i; j + 1 < VisTop; j++) VisSet[j] = VisSet[j + 1];
        VisSet[--VisTop] = ASCENT_NULL;
        __sync_sub_and_fetch(&VisForeign, 1);
        return;
    }
}

/* A widget being DESTROYED is not a widget being hidden.
 *
 * This is the leak that latched the map hidden. Qt delivers no hide() when a
 * visible widget is destroyed - the object simply ceases to exist - so every
 * destroyed-while-visible widget kept its slot and its +1 on the count
 * forever. Measured on this firmware over one session: foreign count stuck at
 * 94 against a learnt baseline of 30, peak 512 (the whole table), and 1102
 * overflows. The count could never fall back to the resting level, so
 * stock_ui stayed true and the map stayed hidden until the 120s backstop.
 *
 * Deliberately NOT vis_del(): that function treats a drop from above the
 * baseline as the stock menu closing and forces a relearn. A destruction
 * carries no such meaning - it is bookkeeping, and inventing a close edge
 * from it would resync the tracker on ordinary widget churn. */
static void vis_forget(void *w)
{
    int i;
    if (!VisArmed || vis_is_ours(w)) return;
    for (i = 0; i < VisTop; i++) {
        if (VisSet[i] != w) continue;
        /* Close the gap rather than leaving a hole: the arrival order is what
         * vis_del() relies on, and a hole would make a later subtree pop
         * retire the wrong span. */
        for (; i + 1 < VisTop; i++) VisSet[i] = VisSet[i + 1];
        VisSet[--VisTop] = ASCENT_NULL;
        __sync_sub_and_fetch(&VisForeign, 1);
        VisForgets++;
        return;
    }
}

/* Ask Qt which tracked widgets are still visible, and drop the rest.
 *
 * This replaces every attempt to INFER the count from show/hide ordering.
 * Qt calls hide() on no child of a widget being hidden, so a menu close
 * delivered one hide for 85 widgets and the count could never fall back -
 * the map latched hidden. Inferring a subtree from arrival order fixed that
 * and broke the opposite way, truncating the stack on ordinary out-of-order
 * hides until the resting count collapsed and nothing was ever detected.
 *
 * Qt does clear the visible attribute on those children, so asking is exact
 * where inferring was not. Cost is one call per tracked widget, a few hundred
 * at worst, throttled to A_VIS_SWEEP_MS.
 *
 * SAFETY: this dereferences pointers we do not own, so it runs only on the Qt
 * GUI thread (widget state is illegal to touch from any other), and only with
 * deallocation interposed so a freed widget cannot still be in the set. If
 * either guarantee is missing the sweep does not run at all. */
static void vis_sweep(au64 now)
{
    int i, out = 0;
    if (!VisArmed || !Real_isvisto) return;
    if (!Real_delete && !Real_delete_sized) return;   /* cannot prove liveness */
    if (now < VisSweepAt) return;
    VisSweepAt = now + A_VIS_SWEEP_MS;
    VisSweeps++;
    for (i = 0; i < VisTop; ) {
        void *w = VisSet[i];
        if (w != ASCENT_NULL && Real_isvisto(w, ASCENT_NULL)) { i++; continue; }
        { int j; for (j = i; j + 1 < VisTop; j++) VisSet[j] = VisSet[j + 1]; }
        VisSet[--VisTop] = ASCENT_NULL;
        out++;
    }
    if (out) {
        __sync_sub_and_fetch(&VisForeign, (au64)out);
        VisSweptOut += (au64)out;
    }
}

/* ---- the interposed Qt entry points ------------------------------------
 * The build is -fvisibility=hidden -ffunction-sections --gc-sections, so
 * these need BOTH an explicit default visibility (to be exported at all, and
 * thus to interpose) AND `used` (or gc-sections drops them: nothing in this
 * translation unit calls them - only the dynamic linker will).
 * Confirmed the hard way: without the attribute they vanished from the
 * symbol table entirely, static symtab included.
 * If the real symbol is somehow missing we must still not crash the host,
 * so every one of these is a no-op rather than a null call in that case. */
#define A_INTERPOSE __attribute__((visibility("default"), used))

/* The interposers are live from the instant this library is mapped, but
 * resolve() does not run until ~12s later (400x25ms QApplication hunt plus a
 * 2s settle). GlassesUI shows its whole UI inside that window. Binding the
 * real functions in resolve() therefore swallowed every startup show() call:
 * blank OSD, null activeWindow(), overlay never attached. Bind on FIRST CALL
 * instead - every object is already mapped by then, so the lookup succeeds.
 * Measured, not theorised: this is exactly what happened on hardware. */
static void vis_bind(void)
{
    if (RealBound) return;
    if (!__sync_bool_compare_and_swap(&RealBound, 0, 1)) return;
    Real_show   = (fn_void1) dlsym(A_RTLD_NEXT, "_ZN7QWidget4showEv");
    Real_hide   = (fn_void1) dlsym(A_RTLD_NEXT, "_ZN7QWidget4hideEv");
    Real_setvis = (fn_setvis)dlsym(A_RTLD_NEXT, "_ZN7QWidget10setVisibleEb");
    Real_dtor0  = (fn_void1) dlsym(A_RTLD_NEXT, "_ZN7QWidgetD0Ev");
    Real_dtor1  = (fn_void1) dlsym(A_RTLD_NEXT, "_ZN7QWidgetD1Ev");
    Real_dtor2  = (fn_void1) dlsym(A_RTLD_NEXT, "_ZN7QWidgetD2Ev");
    Real_isvisto = (fn_isvisto)dlsym(A_RTLD_NEXT, "_ZNK7QWidget11isVisibleToEPKS_");
    Real_delete  = (fn_del1)  dlsym(A_RTLD_NEXT, "_ZdlPv");
    Real_delete_sized = (fn_del2)dlsym(A_RTLD_NEXT, "_ZdlPvm");
    if (!Real_isvisto) VisBindFail++;
    if (!Real_show || !Real_hide || !Real_setvis) VisBindFail++;
    if (!Real_dtor0 || !Real_dtor1 || !Real_dtor2) VisBindFail++;
}
A_INTERPOSE void _ZN7QWidget4showEv(void *w);
A_INTERPOSE void _ZN7QWidget4showEv(void *w)
{
    vis_bind();
    VisShowCalls++;
    vis_add(w);
    if (Real_show) Real_show(w);
}

A_INTERPOSE void _ZN7QWidget4hideEv(void *w);
A_INTERPOSE void _ZN7QWidget4hideEv(void *w)
{
    vis_bind();
    VisHideCalls++;
    vis_del(w);
    if (Real_hide) Real_hide(w);
}

A_INTERPOSE void _ZN7QWidget10setVisibleEb(void *w, int on);
A_INTERPOSE void _ZN7QWidget10setVisibleEb(void *w, int on)
{
    vis_bind();
    VisSetVisCalls++;
    if (on) vis_add(w); else vis_del(w);
    vis_sweep(a_now_ms());
    if (Real_setvis) Real_setvis(w, on);
}

/* Deallocation. Drop the pointer BEFORE the memory goes back, so the sweep can
 * never reach it. Deliberately not gated on VisArmed: a widget freed before we
 * armed is still a pointer that must not survive in the table if it is somehow
 * there, and the scan is skipped entirely when the table is empty. */
static void vis_free_hook(void *p)
{
    int i;
    if (VisTop <= 0 || p == ASCENT_NULL) return;
    for (i = 0; i < VisTop; i++) {
        if (VisSet[i] != p) continue;
        for (; i + 1 < VisTop; i++) VisSet[i] = VisSet[i + 1];
        VisSet[--VisTop] = ASCENT_NULL;
        __sync_sub_and_fetch(&VisForeign, 1);
        VisFreed++;
        return;
    }
}

A_INTERPOSE void _ZdlPv(void *p);
A_INTERPOSE void _ZdlPv(void *p)
{
    vis_bind();
    vis_free_hook(p);
    if (Real_delete) Real_delete(p);
    else             free(p);     /* glibc operator delete is free(); never leak */
}

A_INTERPOSE void _ZdlPvm(void *p, ausize n);
A_INTERPOSE void _ZdlPvm(void *p, ausize n)
{
    vis_bind();
    vis_free_hook(p);
    if (Real_delete_sized) Real_delete_sized(p, n);
    else if (Real_delete)  Real_delete(p);
    else                   free(p);
}

/* Destruction. Free the slot FIRST, then chain: once the real destructor has
 * run the pointer is dangling, and the table must not be left holding it even
 * if the chain-through is what fails.
 *
 * If the real symbol did not bind we deliberately do nothing rather than call
 * through a null pointer. For show/hide a missed call costs a frame; here it
 * would leak the widget. That is the lesser harm of the two, and VisBindFail
 * reports it rather than letting it pass silently. */
A_INTERPOSE void _ZN7QWidgetD2Ev(void *w);
A_INTERPOSE void _ZN7QWidgetD2Ev(void *w)
{
    vis_bind();
    VisDtorCalls++;
    vis_forget(w);
    if (Real_dtor2) Real_dtor2(w);
}

A_INTERPOSE void _ZN7QWidgetD1Ev(void *w);
A_INTERPOSE void _ZN7QWidgetD1Ev(void *w)
{
    vis_bind();
    VisDtorCalls++;
    vis_forget(w);
    if (Real_dtor1) Real_dtor1(w);
}

A_INTERPOSE void _ZN7QWidgetD0Ev(void *w);
A_INTERPOSE void _ZN7QWidgetD0Ev(void *w)
{
    vis_bind();
    VisDtorCalls++;
    vis_forget(w);
    if (Real_dtor0) Real_dtor0(w);
}

/* ---- picker layer: self-contained, isolated from the map path ------------ */
static au8 *psurface_acquire(void *user)
{
    (void)user;
    return (PImage && Q_bits) ? Q_bits(PImage) : ASCENT_NULL;
}

static int build_picker(void *parent)
{
    void *lab = Q_new(4096), *img = Q_new(4096), *pm = Q_new(4096);
    if (!lab || !img || !pm) return 0;
    Q_label(lab, parent, 0);
    Q_attr(lab, QT_WA_TRANSPARENT_FOR_MOUSE_EVENTS, 1);
    Q_focus(lab, QT_NO_FOCUS);
    Q_align(lab, QT_ALIGN_CENTER);
    Q_fixed(lab, (int)PFW, (int)PFH);
    Q_img(img, (int)PFW, (int)PFH, (int)Cfg_fmt);
    if (!Q_bits(img)) return 0;
    Q_pm_ctor(pm);
    Q_show(lab);
    Q_raise(lab);
    PLabel = lab; PImage = img; PPixmap = pm;
    a_surface_init(&PSurface, psurface_acquire, ASCENT_NULL, PFW, PFH, (ausize)PFW * 4u);
    PGeomHave = 0; PGeomReqHave = 0;
    return 1;
}

static void publish_picker(void)
{
    if (!PLabel) return;
    if (!a_surface_write(&PSurface, PScratchBuf)) return;
    if (Q_pmconv(PPixmap, PImage, 0)) { Q_setpix(PLabel, PPixmap); Q_update(PLabel); PBlits++; }
}

/* Own guard: a separate function so its __sigsetjmp has its own scope, exactly
 * like guarded_take(). Shares JB/Armed (never nested with the map take). */
static int guarded_take_picker(void)
{
    Armed = 1;
    if (__sigsetjmp(JB, 1) == 0) {
        int ok = (a_frame_consumer_take(&PFC, PScratchBuf, sizeof PScratchBuf)
                  == A_FRAME_TAKE_OK);
        Armed = 0;
        if (ok) PTakes++;
        return ok;
    }
    Armed = 0;
    a_frame_consumer_detach(&PFC);
    logs("SHIM SIGBUS trapped in picker blit; mapping dropped, will re-attach");
    return 0;
}

static void picker_geom_apply(int x, int y)
{
    int r[4], w = (int)PFW, h = (int)PFH, maxx, maxy;
    if (!PLabel || !Q_setgeom) return;
    if (PGeomReqHave && x == PGeomReqX && y == PGeomReqY) return;
    PGeomReqX = x; PGeomReqY = y; PGeomReqHave = 1;
    maxx = (int)Cfg_scrw - w; maxy = (int)Cfg_scrh - h;
    if (maxx < 0) maxx = 0;
    if (maxy < 0) maxy = 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > maxx) x = maxx;
    if (y > maxy) y = maxy;
    if (PGeomHave && x == PGeomX && y == PGeomY) return;
    a_qrect(r, x, y, (int)PFW, (int)PFH);
    Q_setgeom(PLabel, r);
    PGeomX = x; PGeomY = y; PGeomHave = 1;
}

static void picker_geom_poll(au64 now)
{
    char b[64];
    int fd, n, i, sign, val, got, v[2];
    if (now < PNextGeomPoll) return;
    PNextGeomPoll = now + 250;
    fd = open(PICKER_GEOM_PATH, O_RDONLY);
    if (fd < 0) return;
    n = (int)read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return;
    b[n] = 0;
    i = 0; got = 0;
    while (got < 2) {
        while (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r') i++;
        sign = 1;
        if (b[i] == '-') { sign = -1; i++; }
        if (b[i] < '0' || b[i] > '9') break;
        val = 0;
        while (b[i] >= '0' && b[i] <= '9') { val = val * 10 + (b[i] - '0'); i++; }
        v[got++] = sign * val;
    }
    if (got != 2) return;
    picker_geom_apply(v[0], v[1]);
}

/* Run the picker layer for this tick. Parent is the window the map overlay is
 * on, so both layers are siblings on the same plane. No producer -> no layer. */
static void picker_layer_tick(au64 now, int stock_ui, void *parent)
{
    if (!PFCInit) { a_frame_consumer_init(&PFC, PICKER_FRAME_PATH); PFCInit = 1; }
    if (!PFC.map) {
        if (!a_frame_consumer_attach(&PFC)) {
            if (PBuilt && Q_hide && !PHidden) { Q_hide(PLabel); PHidden = 1; }
            return;                               /* picker not running */
        }
    }
    /* Follow a producer resize: a new size is a new producer instance, so this is
     * the moment to adopt it. Drop the old label (wrong size) and rebuild at the
     * new one, exactly as the map layer does. Guarded against overrunning the
     * scratch buffer. */
    if (PFC.w && (PFC.w != PFW || PFC.h != PFH)
            && (au64)PFC.w * (au64)PFC.h <= A_PICKER_MAX_PX) {
        PFW = PFC.w; PFH = PFC.h;
        if (Q_hide && PLabel) Q_hide(PLabel);
        PLabel = ASCENT_NULL; PBuilt = 0; PParent = ASCENT_NULL;
    }
    if (parent && parent != PParent) {
        if (!build_picker(parent)) { PBuildFail++; return; }
        PParent = parent; PBuilt = 1; PHidden = 0;
    }
    if (!PBuilt) return;
    /* A stopped producer must not leave its last VERIFIED cell on screen.
     * Hide on age alone; replacement detection is only for reattachment. */
    if (a_frame_consumer_age_ms(&PFC,now)>A_PICKER_STALE_MS) {
        if(Q_hide && PLabel && !PHidden) {Q_hide(PLabel);PHidden=1;}
        if(a_frame_consumer_replaced(&PFC)) a_frame_consumer_detach(&PFC);
        return;
    }
    if (Q_hide) {
        if (stock_ui && !PHidden) { Q_hide(PLabel); PHidden = 1; }
        else if (!stock_ui && PHidden) { Q_show(PLabel); PHidden = 0; Q_raise(PLabel); }
    }
    if (PHidden) { guarded_take_picker(); return; }   /* keep the transport warm */
    picker_geom_poll(now);
    if (guarded_take_picker()) {
        publish_picker();
    } else if (a_frame_consumer_age_ms(&PFC, now) > A_PICKER_STALE_MS
               && a_frame_consumer_replaced(&PFC)) {
        /* No new frame for a while AND the path now carries a different
         * instance_id: the picker producer was restarted (crash recovery, an
         * update, or a boot where it came up after us). Detach so the next tick
         * re-attaches to the NEW instance instead of compositing the dead one's
         * last frame forever -- the same recovery the map layer already does. */
        a_frame_consumer_detach(&PFC);
        if (Q_hide && PLabel && !PHidden) { Q_hide(PLabel); PHidden = 1; }
        logs("SHIM picker producer replaced; re-attaching");
    }
}

static void tick(void)
{
    Ticks++;
    au64 now = a_now_ms();

    /* 1. Target selection. Everything about WHEN to rebuild is in the policy. */
    void *cand = Q_active();
    int cw = 0, ch = 0;
    cand_size(cand, &cw, &ch);
    a_ovl_action act = a_ovl_tick(&OVL, cand, cw, ch);

    /* ---- z-order: yield to stock UI -----------------------------------
     * The stock menu appeared BEHIND the map on hardware. The direct cause
     * (Phase 2's periodic re-raise) is gone - raise() now happens once, as it
     * did in the hardware-proven shim, so a widget GlassesUI shows later lands
     * above us naturally.
     *
     * This is the belt-and-braces half: when the active window is NOT the one
     * we are parented to, a stock dialog or menu owns the screen, so hide the
     * map entirely and restore it when focus returns. It needs no new
     * hard-required symbol - QWidget::hide() is resolved optionally and the
     * whole block is skipped when it is absent. */
    /* ---- Phase2C: menu detection ---------------------------------------
     * activeWindow() was the wrong instrument and Phase2B proved it: it
     * measured map_hides=0 while the stock menu was visibly on top. A Qt
     * popup never becomes the active window, so a QMenu is invisible to it.
     *
     * QApplication::activePopupWidget() reports an open QMenu;
     * QApplication::activeModalWidget() reports a QDialog shown with exec().
     * GlassesUI uses both - its menus are QMenu subclasses and its dialogs
     * are modal - so between them they cover the stock UI.
     *
     * A detection is a FALSE POSITIVE if what came back is our own overlay or
     * the window we are parented to. Neither is a popup or a modal, so this
     * should stay at zero; it is counted precisely so that "should" is
     * measured rather than assumed. */
    geom_poll(a_now_ms());   /* Phase2D: live X/Y, throttled to 4 Hz */
    status_publish(now);     /* Phase2F: runtime-truthful capability report */

    int stock_ui;
    if (VisArmed && (Real_show || Real_setvis)) {
        /* Phase2C-rev. The decision comes from what GlassesUI actually did,
         * not from what Qt reports as active. The popup/modal detectors are
         * still SAMPLED so the bench record keeps measuring them - Gate 3
         * expects both to stay at zero on this firmware, and a nonzero value
         * would mean the menu is sometimes a real popup after all. */
        if (Q_popup) { void *p = Q_popup(); if (p && !vis_is_ours(p)) PopupDetects++; }
        if (Q_modal) { void *m = Q_modal(); if (m && !vis_is_ours(m)) ModalDetects++; }

        if (VisResync) {
            /* Drop the whole set and relearn from scratch. Showing the map is
             * the safe side of this error: a stale "stock UI present" hides it
             * forever, whereas a stale "clear" costs at most one frame of
             * overlap that the next detection corrects. */
            int i;
            for (i = 0; i < A_VIS_MAX; i++) VisSet[i] = ASCENT_NULL;
            VisTop = 0;
            VisForeign = 0;
            VisBaselineSet = 0;
            VisArmedAt = a_now_ms();
            VisResync = 0;
            VisResyncs++;
            logu("SHIM vis resync - set cleared, relearning. close_edges=", VisCloseEdges);
        }

        if (!VisBaselineSet) {
            /* Never hide while still learning the quiescent level. */
            if (a_now_ms() - VisArmedAt >= A_VIS_BASELINE_MS) {
                VisBaselineSet = 1;
                VisRef = VisForeign;
                logu("SHIM vis reference armed at count=", VisForeign);
            }
            stock_ui = 0;
        } else {
            /* The decision moved to ascent_vis_policy.c, unchanged in substance
             * for the heuristic and extended with the armed gate.
             *
             * WHY IT MOVED. This function runs inside GlassesUI and cannot be
             * unit tested on a host, which is how a rule that is obviously
             * wrong in hindsight -- "a sudden rise in visible widgets means a
             * menu opened", applied while the aircraft is FLYING -- survived:
             * there was nowhere to write the case down. The policy is now pure
             * and carries 67 host checks, including the arm-burst case that
             * produced the report. */
            a_vis_input vin;
            a_vis_output vout;
            flight_poll(now);
            manual_poll(now);
            /* Every field of both structs is assigned below and every field
             * of vout is assigned by a_vis_decide, so there is nothing to zero.
             * Which matters here: this unit is built -ffreestanding with no
             * libc, so a memset would become an unresolvable symbol in a
             * preload library that deliberately carries no DT_NEEDED. */
            vin.foreign = VisForeign;
            vin.ref = VisRef;
            vin.stock_open = StockSince ? 1 : 0;
            vin.open_since_ms = StockSince;
            vin.now_ms = now;
            vin.baseline_set = 1;
            vin.manual = ManualVis;
            vin.flight_known = FlightKnown;
            vin.armed = FlightArmed;
            vin.flight_age_ms = FlightKnown && now > FlightAtMs ? now - FlightAtMs : 0;
            vin.open_delta = A_VIS_OPEN_DELTA;
            vin.close_slack = A_VIS_CLOSE_SLACK;
            vin.max_hide_ms = Cfg_max_hide_ms;

            a_vis_decide(&vin, &vout);
            VisRef = vout.ref;
            stock_ui = vout.visible ? 0 : 1;
            if (vout.armed_gate) ArmedSuppressions++;
            VisBaseline = VisRef;        /* reported for the bench record */

            if (vout.resync) {
                VisResync = 1;
                VisStuckRecoveries++;
                StockSince = 0;
                logu("SHIM vis stuck hidden - forcing relearn. foreign=",
                     VisForeign);
            } else if (stock_ui) {
                if (!StockSince) StockSince = now;
            } else {
                StockSince = 0;
            }
        }
    } else if (Q_popup || Q_modal) {
        void *popup = Q_popup ? Q_popup() : ASCENT_NULL;
        void *modal = Q_modal ? Q_modal() : ASCENT_NULL;

        if (popup && (popup == Label || popup == OVL.current)) {
            MenuFalsePositives++; popup = ASCENT_NULL;
        }
        if (modal && (modal == Label || modal == OVL.current)) {
            MenuFalsePositives++; modal = ASCENT_NULL;
        }
        if (popup) PopupDetects++;
        if (modal) ModalDetects++;
        stock_ui = (popup != ASCENT_NULL) || (modal != ASCENT_NULL);
    } else {
        /* Neither detector resolved. Fall back to Phase2B behaviour exactly,
         * so this build is never worse than the one it replaces. */
        stock_ui = (cand != ASCENT_NULL && cand != OVL.current);
    }

    /* The picker is an independent layer on the same window. Run it every tick,
     * before the map's own early-returns below, so map-hidden / idle / stale on
     * the MAP never stops the picker from updating. It parents to OVL.current --
     * the window the map overlay adopted -- so the two are siblings. */
    picker_layer_tick(now, stock_ui, OVL.current);

    if (Q_hide && OVL.have && Label) {
        if (stock_ui && !MapHidden) {
            Q_hide(Label); MapHidden = 1; Hides++;
            logs("SHIM stock UI active: map hidden");
        } else if (!stock_ui && MapHidden) {
            Q_show(Label); MapHidden = 0; Shows++;
            /* Re-assert position in the sibling order ONCE on return, not on a
             * timer. Returning to the video view is exactly when the map is
             * supposed to be visible again. */
            Q_raise(Label);
            a_paint_init(&PAINT);      /* force a repaint of the live frame */
            logs("SHIM stock UI closed: map restored");
        }
    }
    /* While hidden, keep consuming frames so the transport stays healthy and
     * the staleness logic keeps working - just never present. */
    if (MapHidden) {
        if (FC.map) guarded_take(ASCENT_NULL);
        if (Cfg_stats_ms && now >= NextStats) { NextStats = now + Cfg_stats_ms; emit_stats(now); }
        return;
    }

    /* MENU FADE. A menu key within the window means a menu is being driven; fade
     * the map so it shows through. On a state change, force a repaint so the new
     * opacity lands on the next frame (the engine publishes continuously, so that
     * is within a frame time). Independent of the widget-count HIDE above: the
     * heavy menus still hide, the root menu now fades. */
    {
        int want = (Cfg_menu_fade_ms && LastMenuKeyMs
                    && (now - LastMenuKeyMs) < (au64)Cfg_menu_fade_ms) ? 1 : 0;
        if (want != MenuFade) {
            MenuFade = want;
            if (want) MenuFades++; else MenuFadeEnds++;
            a_paint_init(&PAINT);   /* repaint so the opacity change is applied */
            logs(want ? "SHIM menu active: map faded"
                      : "SHIM menu idle: map restored");
        }
    }

    if (act == A_OVL_IDLE) {
        /* No usable target. Draw nothing, touch nothing - the label may have
         * gone with its parent. This is the fail-invisible path. */
        if (Cfg_stats_ms && now >= NextStats) { NextStats = now + Cfg_stats_ms; emit_stats(now); }
        return;
    }
    if (act == A_OVL_BUILD) {
        if (!build_overlay(OVL.adopt)) {
            BuildFail++;
            a_ovl_build_failed(&OVL);
            logs("SHIM overlay build FAILED; will retry");
            return;
        }
        a_ovl_built(&OVL);
        /* A fresh overlay has nothing on it: force the next paint. */
        a_paint_init(&PAINT);
        NextRaise = now + Cfg_raise_ms;
        logu("SHIM overlay built, total builds=", OVL.builds);
    }

    /* 2. Follow producer replacement. */
    int attached = FC.map != ASCENT_NULL;
    if (!attached) {
        if (a_frame_consumer_attach(&FC)) {
            attached = 1;
            logs("SHIM attached to a map frame");
            /* Phase2E. A producer may publish a geometry different from the
             * one our widgets were built for - that is the whole point of the
             * versioned contract. Follow it here, on attach, because attach is
             * the only moment the geometry can change: the transport makes a
             * resize a NEW PRODUCER INSTANCE, never a resize of a live
             * mapping under a reader. */
            if (FC.w && (FC.w != FW || FC.h != FH)) {
                logu("SHIM geometry change: old w=", (au64)FW);
                logu("SHIM geometry change: old h=", (au64)FH);
                logu("SHIM geometry change: new w=", (au64)FC.w);
                logu("SHIM geometry change: new h=", (au64)FC.h);
                if (!scratch_ensure(FC.buf_bytes)) {
                    a_frame_consumer_detach(&FC);
                    return;                     /* fail closed, retry later */
                }
                FW = FC.w; FH = FC.h; FBufBytes = FC.buf_bytes;
                GeomChanges++;
                /* The existing label is the wrong size and the build path
                 * allocates a fresh one rather than resizing in place, so the
                 * old one must be taken off screen or it would remain visible
                 * at the old size underneath the new one. It is hidden, not
                 * freed: these widgets are owned by Qt's parent and the shim
                 * has never destroyed one. That leaks one label per geometry
                 * change - bounded by the policy's build cap, and a geometry
                 * change only happens when the engine restarts. */
                if (Q_hide && Label) Q_hide(Label);
                Label = ASCENT_NULL;
                a_ovl_init(&OVL, Cfg_stable, Cfg_minw, Cfg_minh, Cfg_cap);
                /* The Phase2C visibility baseline was learned against the OLD
                 * overlay and the OLD widget set. Carrying it across a rebuild
                 * made stock_ui true the moment the new overlay armed, so the
                 * map was hidden immediately and never restored - observed on
                 * hardware as overlay built, map_hides=1, map_shows=0, nothing
                 * on screen. Force the same resync an overflow triggers: clear
                 * the set and relearn. */
                VisResync = 1;
                a_paint_init(&PAINT);
                GeomHave = 0; GeomReqHave = 0;  /* re-apply position at new size */
                return;                          /* rebuild on the next tick */
            }
        }
    }

    int got = 0;
    au64 seq = 0;

    /* 3. Guarded take. */
    if (attached) {
        got = guarded_take(&seq);
        if (!got && !FC.map) return;      /* SIGBUS dropped the mapping */
    }

    /* 4. Staleness is the consumer's job: an indicator drawn inside the frame
     *    cannot report that frames stopped arriving. */
    int stale = 0;
    if (attached && !got) {
        au64 age = a_frame_consumer_age_ms(&FC, now);
        if (age > Cfg_stale_ms) {
            stale = 1;
            if (a_frame_consumer_replaced(&FC)) {
                a_frame_consumer_detach(&FC);
                logs("SHIM producer replaced; re-attaching");
                return;
            }
        }
    }

    /* 5. Repaint ONLY when the pixels actually changed. */
    a_paint_kind k = a_paint_decide(&PAINT, attached, got, seq, stale);
    if (k == A_PAINT_BANNER) {
        if (!attached) banner("MAP ENGINE NOT RUNNING", 0xFFFFA020);
        else           banner("MAP STALE", 0xFFFF5050);
        publish_pixels();
    } else if (k == A_PAINT_FRAME) {
        publish_pixels();
    }

    /* 6. Z-order recovery, at a low bounded rate. raise() on a child widget
     *    reorders it within its parent and invalidates only its own rect, so
     *    at 0.5 Hz the cost is negligible; without it a GlassesUI widget
     *    raised above the overlay would hide the map permanently. */
    if (Cfg_raise_ms && now >= NextRaise) {
        NextRaise = now + Cfg_raise_ms;
        Q_raise(Label);
        Raises++;
    }

    if (Cfg_stats_ms && now >= NextStats) { NextStats = now + Cfg_stats_ms; emit_stats(now); }
}

/* ---- GUI-thread plumbing -------------------------------------------------- */
static void slot(int which, struct SlotBase *s, void *r, void **a, au8 *ret)
{
    (void)r; (void)a; (void)ret;
    struct Task *t = (struct Task*)s;
    if (which == 0) {                              /* destroy */
        /* P2-E (Phase 2 self-review): Qt may destroy a queued invocation
         * WITHOUT calling it - the receiver went away, the event queue was
         * flushed on shutdown. The budget was only released on the call path,
         * so every such event permanently consumed one of the A_MAX_PENDING
         * slots and after two of them the overlay stopped updating for good.
         * Release it here when the call never happened. */
        if (!t->called) __sync_fetch_and_sub(&Pending, 1);
        free(t);
        return;
    }
    if (which != 1) return;
    t->called = 1;
    tick();
    __sync_fetch_and_sub(&Pending, 1);
}

static int enqueue(void)
{
    /* Bounded: never let work accumulate behind a stalled GUI thread. */
    if (__sync_fetch_and_add(&Pending, 0) >= A_MAX_PENDING) { PostsDropped++; return 0; }
    struct Task *t = (struct Task*)malloc(sizeof *t);
    if (!t) return 0;
    a_memset(t, 0, sizeof *t);
    t->base.ref = 1; t->base.impl = slot; t->kind = 1;
    __sync_fetch_and_add(&Pending, 1);
    if (!Q_invoke(App, &t->base, QT_QUEUED_CONNECTION, 0)) {
        __sync_fetch_and_sub(&Pending, 1);
        return 0;
    }
    Posts++;
    return 1;
}

extern int pthread_create(au64*, const void*, void*(*)(void*), void*);

static void *thread_main(void *x)
{
    (void)x;
    logs("SHIM thread start (phase2 build)");
    void **self = 0;
    for (int i = 0; i < 400; i++) {
        self = (void**)dlsym((void*)0, "_ZN16QCoreApplication4selfE");
        if (self && *self) break;
        a_sleep_ms(25);
    }
    if (!self || !*self) { logs("SHIM FAIL QApplication not found"); return 0; }
    App = *self;
    a_sleep_ms(2000);

    load_config();
    a_ovl_init(&OVL, Cfg_stable, Cfg_minw, Cfg_minh, Cfg_cap);
    a_paint_init(&PAINT);
    a_cadence_set(&CAD, Cfg_hz);

    if (!resolve()) return 0;

    /* Zeroed inside scratch_ensure: the first banner-only paint blits the
     * whole buffer, and uninitialised malloc bytes would appear on screen. */
    if (!scratch_ensure(A_FRAME_BUF_BYTES)) return 0;
    a_frame_consumer_init(&FC, A_FRAME_PATH);
    PrevBus = signal(SIGBUS_NO, on_sigbus);

    logu("SHIM overlay cadence Hz=", (au64)CAD.hz);
    logu("SHIM overlay format (4=RGB32 5=ARGB32)=", (au64)Cfg_fmt);
    logu("SHIM overlay rebuild cap=", (au64)Cfg_cap);

    /* The helper thread only schedules; every Qt call is on the GUI thread.
     * It wakes faster than the cadence so the cadence, not the sleep
     * granularity, sets the rate. */
    au64 gran = CAD.period_ms / 3;
    if (gran < 5) gran = 5;
    for (;;) {
        au64 now = a_now_ms();
        if (a_cadence_due(&CAD, now)) enqueue();
        a_sleep_ms((as64)gran);
    }
}

SHIM_EXPORT __attribute__((constructor))
void ascent_frame_overlay_init(void)
{
    logs("SHIM constructor (phase2 build)");
    au64 th = 0;
    if (pthread_create(&th, 0, thread_main, 0) != 0) logs("SHIM FAIL pthread_create");
}
