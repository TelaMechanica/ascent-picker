/* ascent_picker_osd.c — see ascent_picker_osd.h. Pure text render, no I/O. */
#include "ascent_picker_osd.h"

static void puts_(char *o, int cap, int *pos, const char *s)
{ while (*s && *pos < cap - 1) o[(*pos)++] = *s++; }

int a_posd_format(const a_pctl *c, const a_pcfg *cfg, char *out, int cap)
{
    int pos = 0;
    if (cap <= 0) return 0;
    if (!c->enabled || c->quiesced || (cfg && cfg->display_hidden)) {            /* hidden: never a <2-aircraft bar */
        puts_(out, cap, &pos, "show 0\n");
        out[pos < cap ? pos : cap - 1] = 0;
        return pos;
    }
    puts_(out, cap, &pos, "show 1\n");
    puts_(out,cap,&pos,(cfg && cfg->theme) ? "theme 1\n" : "theme 0\n");
    puts_(out,cap,&pos,"iconscale ");
    au32 scale=(cfg && cfg->iconscale) ? cfg->iconscale : 100;
    char digits[4]; int dn=0;
    do {digits[dn++]=(char)('0'+scale%10);scale/=10;} while(scale && dn<4);
    while(dn && pos<cap-1) out[pos++]=digits[--dn];
    puts_(out,cap,&pos,"\n");
    puts_(out,cap,&pos,"iconstyle ");
    puts_(out,cap,&pos,(cfg && cfg->iconstyle[0]) ? cfg->iconstyle : "vector");
    puts_(out,cap,&pos,"\n");
    puts_(out,cap,&pos,"cooldown ");
    au32 secs=c->busy ? 0 : (c->cooldown_remaining_ms+999)/1000;
    char cd[10]; int cn=0;
    do {cd[cn++]=(char)('0'+secs%10);secs/=10;} while(secs && cn<10);
    while(cn && pos<cap-1)out[pos++]=cd[--cn];
    puts_(out,cap,&pos,"\n");
    for (int k = 0; k < c->n_cfg; k++) {
        int ci = k;              /* index into configured cells */
        puts_(out, cap, &pos, (c->has_target && k == c->verified_cell) ? "cell 1 " : "cell 0 ");
        const char *glyph = (cfg && ci < cfg->n && cfg->cells[ci].glyph[0]) ? cfg->cells[ci].glyph : "-";
        puts_(out,cap,&pos,glyph); puts_(out,cap,&pos," ");
        /* label from the deployed config; fall back to the BbId if absent */
        const char *lbl = (cfg && ci < cfg->n && cfg->cells[ci].label[0]) ? cfg->cells[ci].label : 0;
        if (!c->cells[ci].valid) {
            puts_(out, cap, &pos, "Unavailable");
        } else if (lbl) {
            puts_(out, cap, &pos, lbl);
        } else {
            const char *hex = "0123456789ABCDEF";
            const au8 *bb = c->cells[ci].bbid;
            for (int i = 0; i < 4; i++) {
                if (pos < cap - 1) out[pos++] = hex[bb[i] >> 4];
                if (pos < cap - 1) out[pos++] = hex[bb[i] & 0xF];
                if (i < 3 && pos < cap - 1) out[pos++] = ':';
            }
        }
        if (pos < cap - 1) out[pos++] = '\n';
    }
    out[pos < cap ? pos : cap - 1] = 0;
    return pos;
}
