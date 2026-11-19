/* ascent_pickerbar.h -- the VTX Picker's on-goggles bar: a fixed HUD row of
 * named cells, drawn by the same overlay engine that draws the moving map.
 *
 * WHERE THE WORK IS SPLIT -- identical to the KMZ overlay (ascent_mapoverlay):
 * ASCENT Control deploys fleet labels and display settings; the VRX runtime owns
 * the active cell and writes the tiny text control file; this module
 * owns only the bounded parse of that file and the SCREEN-SPACE draw of the
 * bar. Everything here is pure over plain data, so tests/test_pickerbar.c
 * exercises parse + draw on the host with no Qt, no framebuffer and no VRX.
 *
 * WHY SCREEN SPACE, NOT WORLD SPACE. The map overlay projects lat/lon through
 * the tile pager. The picker bar is a HUD element at a fixed place on the
 * frame, like the scale bar and the zoom label -- so it is drawn LAST, on top
 * of everything, and its geometry is a fraction of the frame, not a coordinate.
 *
 * CONTROL FILE FORMAT (text, one directive per line; unknown lines skipped):
 *
 *   # ...            comment, ignored (also blank lines)
 *   show  <0|1>      bar visibility, default 1
 *   theme <0|1>      0 = dark (default), 1 = light
 *   iconscale <n>    icon size as a percent of the default, 25..400 (default 100).
 *                    The default sizes the glyph to the cell, not the text font.
 *   geom  <x> <y> <w> <h>   bar rectangle in PER-MILLE of the frame (0..1000),
 *                           default 80 700 840 85 (matches the desktop default)
 *   cell  <0|1> <nickname...>   one per line, in slot order; the first token is
 *                               the ACTIVE flag (1 = the linked VTX, drawn
 *                               inverse), the rest of the line is the nickname.
 *
 * The VRX runtime resolves deployed labels and verified active-cell state; no BbId travels here.
 * A malformed directive is skipped, never fatal; the file is accepted with
 * whatever valid cells fit inside the caps below.
 */
#ifndef ASCENT_PICKERBAR_H
#define ASCENT_PICKERBAR_H

#include "ascent_types.h"
#include "ascent_render.h"

/* Caps. Bounded so a hostile or merely large file cannot stall the loop or
 * overrun a buffer. The host writer must respect the same limits. The cell cap
 * matches the desktop organizer's 2..8 range and the Hopper's own bound. */
#define A_PBAR_MAX_CELLS 8u
#define A_PBAR_MAX_NAME  23u      /* chars per cell label (+1 for the NUL)     */
#define A_PBAR_MAX_FILE  8192u    /* bytes read from the control file          */
#define A_PBAR_MAX_LINE  512u     /* one directive line                        */

/* The glyph set, grounded in the HUD aesthetic -- hand-built from render
 * primitives, never bitmap emoji. A_PICON_NONE draws nothing (text-only cell).
 * Stylized 0-9 / A-Z are not here: a one-character nickname already renders them
 * through the HUD font, so a letter/digit "icon" is just a short label. */
enum {
    A_PICON_NONE = 0,
    A_PICON_EYE,       A_PICON_SKULL,  A_PICON_BOMB,
    A_PICON_TARGET,    A_PICON_BOLT,   A_PICON_LINK,   /* LINK = wifi/signal bars */
    A_PICON_STAR,      A_PICON_DRONE,
};

/* Map an icon NAME (control-file token) to its id. Unknown -> A_PICON_NONE. */
au8 a_pbar_icon_id(const char *name);

/* Glyph STYLE: 0 = the built-in vector/primitive set; 1..N = a baked art style
 * (sadgirl / industrial / cyberpunk / military ...), selected by the control-file
 * `iconstyle <name>` directive. Unknown / "vector" / "-" -> 0. */
#define A_PBAR_STYLE_VECTOR 0u
au8 a_pbar_icon_style_id(const char *name);

/* Per-cell state: 0 resting, 1 the linked/active VTX (accent fill), 2 the pending/
 * arming target (blinking accent outline -- the switch cooldown). */
enum { A_PBAR_CELL_NORMAL = 0, A_PBAR_CELL_ACTIVE = 1, A_PBAR_CELL_PENDING = 2 };

typedef struct {
    au8  active;                      /* A_PBAR_CELL_* state                     */
    au8  icon;                        /* A_PICON_* glyph, or A_PICON_NONE       */
    char name[A_PBAR_MAX_NAME + 1];   /* nickname, always NUL-terminated        */
} a_pbar_cell;

enum { A_PBAR_HORIZONTAL = 0, A_PBAR_VERTICAL = 1 };

typedef struct {
    int has_owner;
    au32 owner_tick;
    int  show;                 /* bar visibility, default 1                     */
    au8  theme;                /* 0 dark, 1 light                              */
    au8  iconstyle;            /* 0 = vector; 1..N = a baked art style          */
    au8  orient;               /* A_PBAR_HORIZONTAL / A_PBAR_VERTICAL           */
    au16 iconscale;            /* icon size as % of the default (100 = default) */
    au32 accent;               /* 0xAARRGGBB active-cell accent (default magenta)*/
    au32 gx, gy, gw, gh;       /* bar rect, per-mille of the frame (0..1000)    */
    au32 cooldown_s;           /* advisory banner; no operation authority */
    au32 ncells;
    au64 generation;           /* bumped by the caller on an accepted change    */
    a_pbar_cell cell[A_PBAR_MAX_CELLS];
} a_pbar_store;

/* Clear to empty + visible + default geometry. Preserves generation. */
void a_pbar_reset(a_pbar_store *s);

/* Only a live runtime heartbeat permits the independent producer to display state. */
int a_pbar_owner_fresh(const a_pbar_store *s, au32 now_ms);

/* Parse `len` bytes of control text into `s` (previous contents replaced).
 * Returns the number of cells accepted. Never reads past `len`, never exceeds
 * the caps, never crashes on malformed input. Does NOT bump generation -- the
 * caller owns that on a detected change (same contract as a_ovl_parse). */
au32 a_pbar_parse(a_pbar_store *s, const char *text, ausize len);

/* Draw the bar in the SCREEN pass (last, on top). No-op when hidden, empty, or
 * when the resolved rectangle is too small to render legibly. fw/fh are the
 * frame dimensions. Uses only the clipped primitives, so an out-of-range
 * geometry cannot write outside the frame. */
void a_pbar_draw(const a_pbar_store *s, const a_canvas *cv, au32 fw, au32 fh, au64 now_ms);

#endif /* ASCENT_PICKERBAR_H */
