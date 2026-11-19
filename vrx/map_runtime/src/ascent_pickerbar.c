/* ascent_pickerbar.c -- bounded parse + screen-pass draw of the VTX Picker bar.
 * See ascent_pickerbar.h for the format and the host/engine split. */

#include "ascent_pickerbar.h"
#include "picker_icons_data.h"   /* baked glyph styles (generated from picker_icons.zip) */

/* ---- tiny bounded scanners (no libc; match the engine's self-contained style,
 *      identical in shape to ascent_mapoverlay.c) ------------------------------ */

static int pbar_streq(const char *a, const char *b)
{
    au32 i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

/* One whitespace-delimited token from *p, NUL-terminated in place; NULL at end.
 * On return *p points PAST the token, at the start of the remainder -- which is
 * how the `cell` directive recovers the rest-of-line nickname. */
static char *pbar_tok(char **p)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == 0) { *p = s; return 0; }
    char *start = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) { *s = 0; s++; }
    *p = s;
    return start;
}

/* Small unsigned decimal, fully consumed. */
static int pbar_int(const char *s, au32 *out)
{
    au32 v = 0, i = 0, any = 0;
    for (; s[i] >= '0' && s[i] <= '9'; i++) {
        v = v * 10u + (au32)(s[i] - '0');
        any = 1;
        if (v > 1000000u) return 0;
    }
    if (!any || s[i] != 0) return 0;
    *out = v;
    return 1;
}

/* Exactly 8 hex digits -> AARRGGBB. Trailing junk rejects. */
static int pbar_hex8(const char *s, au32 *out)
{
    au32 v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i]; au32 d;
        if (c >= '0' && c <= '9') d = (au32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (au32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (au32)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    if (s[8] != 0) return 0;
    *out = v;
    return 1;
}

/* Rest-of-line, leading whitespace trimmed, copied NUL-terminated and bounded. */
static void pbar_rest(char *dst, ausize cap, const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    a_strlcpy(dst, p, cap);
}

au8 a_pbar_icon_id(const char *name)
{
    if (!name) return A_PICON_NONE;
    if (pbar_streq(name, "eye"))                              return A_PICON_EYE;
    if (pbar_streq(name, "skull"))                            return A_PICON_SKULL;
    if (pbar_streq(name, "bomb"))                             return A_PICON_BOMB;
    if (pbar_streq(name, "target") || pbar_streq(name, "crosshair")) return A_PICON_TARGET;
    if (pbar_streq(name, "bolt")   || pbar_streq(name, "lightning")) return A_PICON_BOLT;
    if (pbar_streq(name, "link")   || pbar_streq(name, "wifi")
                                   || pbar_streq(name, "signal")) return A_PICON_LINK;
    if (pbar_streq(name, "star"))                             return A_PICON_STAR;
    if (pbar_streq(name, "drone"))                            return A_PICON_DRONE;
    return A_PICON_NONE;   /* "-", "none", unknown */
}

au8 a_pbar_icon_style_id(const char *name)
{
    if (!name) return A_PBAR_STYLE_VECTOR;
    if (pbar_streq(name, "vector") || pbar_streq(name, "none") || pbar_streq(name, "-"))
        return A_PBAR_STYLE_VECTOR;
    for (int i = 0; i < A_PICON_NSTYLES; i++)
        if (pbar_streq(name, A_PICON_STYLE_NAME[i]))
            return (au8)(i + 1);          /* 1..N */
    return A_PBAR_STYLE_VECTOR;
}

/* ---- public -------------------------------------------------------------- */

void a_pbar_reset(a_pbar_store *s)
{
    if (!s) return;
    au64 g = s->generation;          /* generation is the caller's; preserve it */
    a_memset(s, 0, sizeof *s);
    s->show = 1;
    s->theme = 0;                    /* dark */
    s->iconstyle = A_PBAR_STYLE_VECTOR;
    s->iconscale = 100;              /* glyph sized to the cell, not the font */
    s->orient = A_PBAR_HORIZONTAL;
    s->accent = 0xFFFF2D9Bu;         /* magenta, the reference accent */
    s->gx = 80; s->gy = 700; s->gw = 840; s->gh = 85;  /* desktop default */
    s->generation = g;
}

int a_pbar_owner_fresh(const a_pbar_store *s, au32 now_ms)
{ return s && s->has_owner && (au32)(now_ms-s->owner_tick)<3000u; }

au32 a_pbar_parse(a_pbar_store *s, const char *text, ausize len)
{
    if (!s) return 0;
    a_pbar_reset(s);
    if (!text || len == 0) return 0;
    if (len > A_PBAR_MAX_FILE) len = A_PBAR_MAX_FILE;

    ausize i = 0;
    while (i < len) {
        ausize j = i;
        while (j < len && text[j] != '\n') j++;

        char lb[A_PBAR_MAX_LINE];
        ausize k = 0;
        for (ausize t = i; t < j && k < A_PBAR_MAX_LINE - 1; t++) {
            char c = text[t];
            if (c == '\r') continue;
            lb[k++] = c;
        }
        lb[k] = 0;
        i = (j < len) ? j + 1 : j;

        char *p = lb;
        char *d = pbar_tok(&p);
        if (!d || d[0] == '#') continue;

        if (pbar_streq(d,"owner_tick")) {
            char *v=pbar_tok(&p);
            if(v && a_strnlen(v,9)==8 && pbar_hex8(v,&s->owner_tick)) s->has_owner=1;
            continue;
        }
        if(pbar_streq(d,"cooldown")) {
            char *v=pbar_tok(&p); au32 n;
            if(v && pbar_int(v,&n) && n<=300)s->cooldown_s=n;
            continue;
        }
        if (pbar_streq(d, "show")) {
            char *v = pbar_tok(&p);
            if (v && (v[0] == '0' || v[0] == '1') && v[1] == 0) s->show = (v[0] == '1');
            continue;
        }
        if (pbar_streq(d, "theme")) {
            char *v = pbar_tok(&p);
            if (v && (v[0] == '0' || v[0] == '1') && v[1] == 0) s->theme = (au8)(v[0] - '0');
            continue;
        }
        if (pbar_streq(d, "iconstyle")) {
            char *v = pbar_tok(&p);
            if (v) s->iconstyle = a_pbar_icon_style_id(v);
            continue;
        }
        if (pbar_streq(d, "iconscale")) {
            char *v = pbar_tok(&p); au32 n;
            if (v && pbar_int(v, &n)) {
                if (n < 25u)  n = 25u;
                if (n > 400u) n = 400u;
                s->iconscale = (au16)n;
            }
            continue;
        }
        if (pbar_streq(d, "orient")) {
            char *v = pbar_tok(&p);
            if (v && (v[0] == '0' || v[0] == '1') && v[1] == 0) s->orient = (au8)(v[0] - '0');
            continue;
        }
        if (pbar_streq(d, "accent")) {
            char *v = pbar_tok(&p); au32 col;
            if (v && pbar_hex8(v, &col)) s->accent = col;
            continue;
        }
        if (pbar_streq(d, "geom")) {
            char *xt = pbar_tok(&p), *yt = pbar_tok(&p);
            char *wt = pbar_tok(&p), *ht = pbar_tok(&p);
            au32 x, y, w, h;
            if (xt && yt && wt && ht &&
                pbar_int(xt, &x) && pbar_int(yt, &y) &&
                pbar_int(wt, &w) && pbar_int(ht, &h)) {
                /* Per-mille, clamped to the frame. A degenerate rect is rejected
                 * (keeps the default) rather than drawn as a sliver. */
                if (x <= 1000 && y <= 1000 && w >= 10 && w <= 1000 &&
                    h >= 10 && h <= 1000 && x + w <= 1000 && y + h <= 1000) {
                    s->gx = x; s->gy = y; s->gw = w; s->gh = h;
                }
            }
            continue;
        }
        if (pbar_streq(d, "cell")) {
            if (s->ncells >= A_PBAR_MAX_CELLS) continue;
            char *at = pbar_tok(&p);
            if (!at || !(at[0] == '0' || at[0] == '1' || at[0] == '2') || at[1] != 0) continue;
            char *ic = pbar_tok(&p);                 /* icon token: a name, or "-" */
            a_pbar_cell *c = &s->cell[s->ncells++];
            c->active = (au8)(at[0] - '0');
            c->icon = a_pbar_icon_id(ic);
            pbar_rest(c->name, sizeof c->name, p);   /* rest of line = nickname */
            continue;
        }
        /* unknown directive -> skip */
    }
    return s->ncells;
}

/* ---- draw ---------------------------------------------------------------- */

static au32 pm_to_px(au32 permille, au32 span)
{
    return (au32)(((au64)permille * (au64)span) / 1000u);
}

/* The glyph set, drawn from the clipped render primitives at a given box
 * (x,y,sz,sz). Deliberately monoline/geometric so it stays legible small and
 * faithful to the HUD aesthetic -- not a bitmap emoji. */
static void draw_icon(const a_canvas *cv, au8 id, int x, int y, int sz, au32 col, au8 style)
{
    if (id == A_PICON_NONE || sz < 6) return;

    /* Baked art style: blit the matte, tinted, nearest-neighbour into the box
     * (no AA, by design). Falls through to the vector set if the slot is empty. */
    if (style >= 1 && style <= A_PICON_NSTYLES && id >= 1 && id <= 8) {
        int zi = 0;
        for (int i = 0; i < A_PICON_NSIZES; i++)
            if (A_PICON_SIZE_PX[i] <= (au16)sz) zi = i;     /* largest baked <= box */
        const a_picon_bitmap *bm = &A_PICON_TABLE[style - 1][id - 1][zi];
        if (bm->px && bm->w && bm->h) {
            /* Crop the matte to its ink before scaling: the baked canvases carry
             * 12-25% transparent margin, so mapping the WHOLE canvas into the box
             * renders the glyph small and off-centre. Find the ink bounding box,
             * then fit it into sz x sz with aspect preserved and centred. */
            int minx = (int)bm->w, miny = (int)bm->h, maxx = -1, maxy = -1;
            for (int yy = 0; yy < (int)bm->h; yy++)
                for (int xx = 0; xx < (int)bm->w; xx++)
                    if (bm->px[(ausize)yy * bm->w + (ausize)xx]) {
                        if (xx < minx) minx = xx;
                        if (xx > maxx) maxx = xx;
                        if (yy < miny) miny = yy;
                        if (yy > maxy) maxy = yy;
                    }
            if (maxx >= minx && maxy >= miny) {
                int gw = maxx - minx + 1, gh2 = maxy - miny + 1;
                int dstw, dsth;
                if (gw >= gh2) { dstw = sz; dsth = gh2 * sz / gw; }
                else           { dsth = sz; dstw = gw * sz / gh2; }
                if (dstw < 1) dstw = 1;
                if (dsth < 1) dsth = 1;
                int ox = x + (sz - dstw) / 2, oy = y + (sz - dsth) / 2;
                for (int dy = 0; dy < dsth; dy++)
                    for (int dx = 0; dx < dstw; dx++) {
                        int sxp = minx + dx * gw / dstw;
                        int syp = miny + dy * gh2 / dsth;
                        if (bm->px[(ausize)syp * bm->w + (ausize)sxp]) a_px(cv, ox + dx, oy + dy, col);
                    }
                return;
            }
        }
    }

    int cx = x + sz / 2, cy = y + sz / 2, r = sz / 2;
    switch (id) {
    case A_PICON_EYE:
        a_circle(cv, cx, cy, r, 2, col);
        a_circle(cv, cx, cy, r / 3 + 1, r, col);          /* filled pupil (t>=r) */
        break;
    case A_PICON_SKULL:
        a_circle(cv, cx, cy - sz / 8, r, 2, col);
        a_circle(cv, cx - r / 2, cy - sz / 8, r / 5 + 1, r, col);
        a_circle(cv, cx + r / 2, cy - sz / 8, r / 5 + 1, r, col);
        a_rect(cv, cx - r / 3, cy + r / 2, (r * 2) / 3, r / 2, col);  /* jaw */
        break;
    case A_PICON_BOMB:
        a_circle(cv, cx, cy + sz / 6, (r * 4) / 5, r, col);          /* body fill */
        a_line(cv, cx + r / 2, cy - r / 3, x + sz - 1, y, 2, col);    /* fuse */
        a_line(cv, x + sz - 1, y, x + sz - 3, y + 3, 1, col);         /* spark */
        a_line(cv, x + sz - 1, y, x + sz + 1, y + 2, 1, col);
        break;
    case A_PICON_TARGET:
        a_circle(cv, cx, cy, r, 2, col);
        a_line(cv, cx, y, cx, y + sz, 1, col);
        a_line(cv, x, cy, x + sz, cy, 1, col);
        break;
    case A_PICON_BOLT:
        a_line(cv, cx + r / 3, y, x + r / 3, cy, 3, col);
        a_line(cv, x + r / 3, cy, cx + r / 4, cy, 2, col);
        a_line(cv, cx + r / 4, cy, cx - r / 3, y + sz, 3, col);
        break;
    case A_PICON_LINK: {                               /* ascending wifi bars */
        int bars = 4, bw2 = sz / (bars + 1), gap = bw2 / 3, bxl = x;
        if (bw2 < 1) bw2 = 1;
        for (int i = 0; i < bars; i++) {
            int bh2 = (sz * (i + 1)) / bars;
            a_rect(cv, bxl, y + sz - bh2, bw2, bh2, col);
            bxl += bw2 + gap;
        }
        break; }
    case A_PICON_STAR:
        a_tri_fill(cv, cx, y, x, y + (sz * 2) / 3, x + sz, y + (sz * 2) / 3, col);
        a_tri_fill(cv, cx, y + sz, x, y + sz / 3, x + sz, y + sz / 3, col);
        break;
    case A_PICON_DRONE:
        a_circle(cv, x + r / 2, y + r / 2, r / 3 + 1, r, col);
        a_circle(cv, x + sz - r / 2, y + r / 2, r / 3 + 1, r, col);
        a_circle(cv, x + r / 2, y + sz - r / 2, r / 3 + 1, r, col);
        a_circle(cv, x + sz - r / 2, y + sz - r / 2, r / 3 + 1, r, col);
        a_rect(cv, cx - r / 3, cy - r / 3, (r * 2) / 3, (r * 2) / 3, col);
        break;
    default: break;
    }
}

/* One cell, laid out in its own rect. Shared by both orientations. The active
 * cell gets the accent treatment from the references: a tinted fill, a solid
 * accent left bar and a 1px accent border, with white content. */
static void draw_cell(const a_canvas *cv, const a_pbar_cell *c,
                      int x, int y, int w, int h, au8 theme, au32 accent, au8 style,
                      au16 iconscale, au64 now_ms)
{
    if (w < 8 || h < 8) return;

    int scale = 1;
    if (h >= A_GLYPH_H * 3 + 10) scale = 3;
    else if (h >= A_GLYPH_H * 2 + 8) scale = 2;
    int adv = a_text_advance(scale);
    int gh  = A_GLYPH_H * scale;

    au32 fg   = theme ? 0xFF101010u : 0xFFF2F2F6u;      /* normal content        */
    au32 tfg  = fg, icol = fg;
    au32 shadow;

    if (c->active == A_PBAR_CELL_ACTIVE) {
        au32 fill   = (accent & 0x00FFFFFFu) | 0x66000000u;   /* tinted fill     */
        a_rect(cv, x, y, w, h, fill);
        a_rect(cv, x, y, 3, h, accent);                       /* accent left bar */
        a_line(cv, x, y, x + w - 1, y, 1, accent);            /* 1px border      */
        a_line(cv, x, y + h - 1, x + w - 1, y + h - 1, 1, accent);
        a_line(cv, x + w - 1, y, x + w - 1, y + h - 1, 1, accent);
        tfg = 0xFFFFFFFFu; icol = 0xFFFFFFFFu;
    } else if (c->active == A_PBAR_CELL_PENDING) {
        /* Arming: blink an accent outline so the cell about to become active reads
         * as "armed, counting down" (the switch cooldown). ~1.7 Hz from now_ms. */
        if (((now_ms / 300u) & 1u) == 0u) {
            a_line(cv, x, y, x + w - 1, y, 2, accent);
            a_line(cv, x, y + h - 1, x + w - 1, y + h - 1, 2, accent);
            a_line(cv, x, y, x, y + h - 1, 2, accent);
            a_line(cv, x + w - 1, y, x + w - 1, y + h - 1, 2, accent);
        }
    }
    shadow = (tfg == 0xFFFFFFFFu) ? 0xFF000000u : 0u;

    int pad = 5;
    /* Icon size is driven by the CELL, not the text font: a taller bar gets a
     * bigger glyph. The default (iconscale 100) is 50% of the cell height; the
     * control file can scale it 25..400%. Clamped so it never exceeds the cell
     * or crowds the label out of a narrow cell. */
    int iconsz = (h * 50 / 100) * (int)iconscale / 100;
    if (iconsz < 10) iconsz = 10;
    if (iconsz > h - 4) iconsz = h - 4;
    if (iconsz > w * 45 / 100) iconsz = w * 45 / 100;
    int gap = (c->icon != A_PICON_NONE) ? 5 : 0;
    int iconw = (c->icon != A_PICON_NONE) ? iconsz : 0;
    int avail = w - 2 * pad;
    int textroom = avail - iconw - gap;
    int maxch = (adv > 0 && textroom > 0) ? textroom / adv : 0;
    int have = (int)a_strnlen(c->name, A_PBAR_MAX_NAME);
    int drawn = have < maxch ? have : maxch;
    int tw = drawn * adv;
    int blockw = iconw + ((iconw && drawn) ? gap : 0) + tw;
    int sx = x + 3 + (w - 3 - blockw) / 2;                /* +3 clears the accent bar */
    if (sx < x + pad) sx = x + pad;

    int xc = sx;
    if (c->icon != A_PICON_NONE) {
        draw_icon(cv, c->icon, xc, y + (h - iconsz) / 2, iconsz, icol, style);
        xc += iconw + (drawn ? gap : 0);
    }
    if (drawn > 0)
        a_text(cv, xc, y + (h - gh) / 2, c->name, maxch, scale, tfg, shadow);
}

void a_pbar_draw(const a_pbar_store *s, const a_canvas *cv, au32 fw, au32 fh, au64 now_ms)
{
    if (!s || !cv) return;
    if (!s->show || s->ncells == 0) return;
    if (fw == 0 || fh == 0) return;

    int bx = (int)pm_to_px(s->gx, fw);
    int by = (int)pm_to_px(s->gy, fh);
    int bw = (int)pm_to_px(s->gw, fw);
    int bh = (int)pm_to_px(s->gh, fh);
    if (bw < 16 || bh < 12) return;              /* too small to render legibly */

    au32 bar_bg = s->theme ? 0xE0E8E8E8u : 0xC8101010u;
    au32 sep    = s->theme ? 0x30000000u : 0x30FFFFFFu;
    a_rect(cv, bx, by, bw, bh, bar_bg);          /* the panel behind the cells  */

    if(s->cooldown_s && bh>=24) {
        char msg[48]; int k=0; const char *v="COOLDOWN ";
        while(*v)msg[k++]=*v++;
        au32 secs=s->cooldown_s;char d[10];int dn=0;
        do {d[dn++]=(char)('0'+secs%10);secs/=10;}while(secs && dn<10);
        while(dn)msg[k++]=d[--dn];
        v="S - ADVISORY";while(*v)msg[k++]=*v++;msg[k]=0;
        int scale=(bh>=48 && bw>=300)?2:1;
        int banner_h=7*scale+4;
        a_text(cv,bx+2,by+bh-banner_h+2,msg,(bw-4)/a_text_advance(scale),scale,s->theme?0xff101010u:0xfff2f2f6u,0);
        bh-=banner_h; /* Banner stays inside this widget's geometry. */
    }
    int n = (int)s->ncells;
    if (s->orient == A_PBAR_VERTICAL) {
        int ch = bh / n; if (ch < 1) ch = 1;
        for (int i = 0; i < n; i++) {
            int cy = by + i * ch;
            int h  = (i == n - 1) ? (by + bh - cy) : ch;
            if (i > 0) a_line(cv, bx, cy, bx + bw - 1, cy, 1, sep);
            draw_cell(cv, &s->cell[i], bx, cy, bw, h, s->theme, s->accent, s->iconstyle, s->iconscale, now_ms);
        }
    } else {
        int cw = bw / n; if (cw < 1) cw = 1;
        for (int i = 0; i < n; i++) {
            int cx = bx + i * cw;
            int w  = (i == n - 1) ? (bx + bw - cx) : cw;
            if (i > 0) a_line(cv, cx, by, cx, by + bh - 1, 1, sep);
            draw_cell(cv, &s->cell[i], cx, by, w, bh, s->theme, s->accent, s->iconstyle, s->iconscale, now_ms);
        }
    }
}
