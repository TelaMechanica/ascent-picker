/* ascent_picker_ipc.c — see ascent_picker_ipc.h. Pure parse/format, no I/O. */
#include "ascent_picker_ipc.h"
#include "ascent_picker_cfg.h"   /* a_pcfg_bbid */

static int starts(const char *p, const char *end, const char *lit)
{ while (*lit) { if (p >= end || *p != *lit) return 0; p++; lit++; } return 1; }

int a_pipc_parse(const char *line, ausize len, a_pipc_cmd *out)
{
    a_memset(out,0,sizeof *out); out->cell=-1;
    if(!line) return 0;
    const char *p=line,*end=line+len;
    while(p<end && (*p==' ' || *p=='\t')) p++;
    while(end>p && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) end--;
    if(end-p==6 && starts(p,end,"STATUS")) {out->kind=A_PIPC_STATUS; return 1;}
    if(end-p==7 && starts(p,end,"QUIESCE")) {out->kind=A_PIPC_QUIESCE; return 1;}
    if(end-p==23 && starts(p,end,"SELECT BBID ") && a_pcfg_bbid(p+12,out->bbid)) {out->kind=A_PIPC_SELECT_BBID; return 1;}
    if(starts(p,end,"SELECT CELL ")) {
        p+=12; int v=0,any=0;
        while(p<end && *p>='0' && *p<='9') { if(v>100) return 0; v=v*10+(*p++-'0'); any=1; }
        if(!any || p!=end || v>=A_PCTL_MAX_CELLS) return 0;
        out->kind=A_PIPC_SELECT_CELL; out->cell=v; return 1;
    }
    return 0;
}

/* minimal unsigned->decimal; returns chars written */
static int put_uint(char *o, int cap, int *pos, au32 v)
{
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--) { if (*pos < cap - 1) o[(*pos)++] = tmp[i]; }
    return n;
}
static void put_str(char *o, int cap, int *pos, const char *s)
{ while (*s && *pos < cap - 1) o[(*pos)++] = *s++; }
static void put_int(char *o, int cap, int *pos, int v)
{ if (v < 0) { if (*pos < cap - 1) o[(*pos)++] = '-'; v = -v; } put_uint(o, cap, pos, (au32)v); }
static void put_hex2(char *o, int cap, int *pos, au8 b)
{
    const char *h = "0123456789ABCDEF";
    if (*pos < cap - 1) o[(*pos)++] = h[b >> 4];
    if (*pos < cap - 1) o[(*pos)++] = h[b & 0xF];
}

int a_pipc_format_status(const a_pctl *c, char *out, int cap)
{
    int pos = 0;
    if (!out || cap <= 0) return 0;
    put_str(out, cap, &pos, "STATUS enabled=");
    put_uint(out, cap, &pos, c->enabled ? 1u : 0u);
    put_str(out, cap, &pos, " active=");
    put_uint(out, cap, &pos, (au32)c->active_n);
    put_str(out,cap,&pos," configured="); put_uint(out,cap,&pos,(au32)c->n_cfg);
    put_str(out,cap,&pos," pwm_slots=1");
    put_str(out, cap, &pos, " cur=");
    put_int(out, cap, &pos, c->verified_cell);
    put_str(out,cap,&pos," input="); put_int(out,cap,&pos,c->cur_cell);
    put_str(out, cap, &pos, " target=");
    if (c->has_target) {
        for (int i = 0; i < 4; i++) { put_hex2(out, cap, &pos, c->cur_target[i]); if (i < 3 && pos < cap - 1) out[pos++] = ':'; }
    } else {
        put_str(out, cap, &pos, "-");
    }
    put_str(out, cap, &pos, " busy="); put_uint(out, cap, &pos, c->busy ? 1u : 0u);
    put_str(out,cap,&pos," lifecycle=1 loaded="); put_uint(out,cap,&pos,c->quiesced ? 0u : 1u);
    put_str(out,cap,&pos," rc_enabled="); put_uint(out,cap,&pos,c->rc_enabled ? 1u : 0u);
    put_str(out,cap,&pos," rc_channel="); put_uint(out,cap,&pos,(au32)c->rc_channel);
    put_str(out,cap,&pos," hold_ms="); put_uint(out,cap,&pos,c->hold_stable_ms);
    put_str(out, cap, &pos, " rc_fresh="); put_uint(out,cap,&pos,c->rc_fresh ? 1u : 0u);
    put_str(out,cap,&pos," rc_proto="); put_uint(out,cap,&pos,(au32)c->rc_protocol);
    put_str(out,cap,&pos," rc_us="); put_uint(out,cap,&pos,(au32)c->rc_pwm_us);
    put_str(out,cap,&pos," cooldown_ms="); put_uint(out,cap,&pos,c->cooldown_remaining_ms);
    put_str(out,cap,&pos," switch_ms="); put_uint(out,cap,&pos,c->last_switch_ms);
    put_str(out,cap,&pos," acquire_ms="); put_uint(out,cap,&pos,c->last_acquire_ms);
    put_str(out,cap,&pos," recovery_ok="); put_uint(out,cap,&pos,c->have_last && c->running.recovery_ok ? 1u : 0u);
    put_str(out, cap, &pos, " faulted="); put_uint(out,cap,&pos,c->faulted ? 1u : 0u);
    put_str(out, cap, &pos, " commanded=");
    if (c->has_command) {
        for (int i=0; i<4; i++) { put_hex2(out, cap, &pos, c->commanded_target[i]); if(i<3) put_str(out,cap,&pos,":"); }
    } else put_str(out,cap,&pos,"-");
    put_str(out, cap, &pos, " outcome=");
    put_uint(out, cap, &pos, (au32)(c->have_last ? c->last_outcome : 0));
    put_str(out, cap, &pos, " phase=");
    put_uint(out, cap, &pos, (au32)(c->busy ? c->running.phase : (c->have_last ? c->last_phase : 0)));
    put_str(out, cap, &pos, " reason=");
    put_uint(out, cap, &pos, (au32)c->reason);
    if (pos < cap) out[pos] = 0; else if (cap > 0) out[cap - 1] = 0;
    return pos;
}
