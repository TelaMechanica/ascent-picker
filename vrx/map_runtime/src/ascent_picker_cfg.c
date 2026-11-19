/* ascent_picker_cfg.c — see ascent_picker_cfg.h. Pure parse, no I/O, no alloc. */
#include "ascent_picker_cfg.h"
#include "ascent_switch.h"   /* a_sw_target_valid */

static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int a_pcfg_bbid(const char *s, au8 out[4])
{
    /* <four-byte-hex-id> */
    for (int i = 0; i < 4; i++) {
        int hi = hexnib(s[0]), lo = hexnib(s[1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (au8)((hi << 4) | lo);
        s += 2;
        if (i < 3) { if (*s != ':') return 0; s++; }
    }
    return a_sw_target_valid(out);   /* rejects <four-byte-hex-id> and <four-byte-hex-id> */
}

/* small helpers (freestanding: no libc string funcs assumed here) */
static int starts_with(const char *p, const char *end, const char *lit)
{
    while (*lit) { if (p >= end || *p != *lit) return 0; p++; lit++; }
    return 1;
}
static au32 parse_u32(const char *p, const char *end)
{
    if(p==end) return 0xFFFFFFFFu;
    au32 v=0;
    while(p<end && *p>='0' && *p<='9') {
        if(v>100000u) return 0xFFFFFFFFu;
        v=v*10+(au32)(*p++-'0');
    }
    return p==end?v:0xFFFFFFFFu;
}

void a_pcfg_parse(const char *text, ausize len, a_pcfg *out)
{
    a_memset(out, 0, sizeof(*out));
    out->hold_ms = 120;
    out->iconscale = 100;
    out->rc_channel = 5;
    if (!text || len == 0) { out->ok = 0; return; }
    const char *p = text, *end = text + len;

    /* line iterator */
    int seen_header = 0;
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        const char *le = p;                 /* [ls,le) is one line (no \n) */
        if (p < end) p++;                    /* skip \n */
        /* trim leading spaces */
        while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '\r')) ls++;
        /* trim trailing \r/space */
        while (le > ls && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r')) le--;
        if (ls == le) continue;              /* blank */

        if (!seen_header) {
            if (le-ls != 19 || !starts_with(ls, le, "ascent_picker_rt v1")) { out->ok = 0; return; }
            seen_header = 1;
            continue;
        }
        if(starts_with(ls,le,"display_hidden ")) {
            au32 v=parse_u32(ls+15,le); if(v>1) {out->ok=0;return;}
            out->display_hidden=(int)v; continue;
        }
        if(starts_with(ls,le,"cooldown_ms ")) {
            out->cooldown_ms=parse_u32(ls+12,le);
            if(out->cooldown_ms>300000) {out->ok=0;return;}
            continue;
        }
        if (starts_with(ls, le, "hold_ms ")) {
            out->hold_ms = parse_u32(ls + 8, le);
            if(out->hold_ms>5000) {out->ok=0;return;}
            continue;
        }
        if (starts_with(ls, le, "rc_enabled ")) { au32 on=parse_u32(ls+11,le); if(on>1) {out->ok=0;return;} out->rc_enabled = on == 1; continue; }
        if (starts_with(ls, le, "rc_channel ")) {
            au32 ch = parse_u32(ls + 11, le);
            if (ch < 1 || ch > 16) {out->ok=0;return;}
            out->rc_channel = (int)ch;
            continue;
        }
        if (starts_with(ls, le, "iconscale ")) {
            au32 v=parse_u32(ls+10,le); if(v<25 || v>400) {out->ok=0;return;} out->iconscale=v; continue;
        }
        if (starts_with(ls, le, "theme ")) {
            au32 v=parse_u32(ls+6,le); if(v>1) {out->ok=0;return;} out->theme=(int)v; continue;
        }
        if (starts_with(ls, le, "iconstyle ")) {
            const char *q=ls+10; int n=(int)(le-q);
            if(n<1 || n>=A_PCFG_LABEL) {out->ok=0;return;}
            for(int i=0;i<n;i++) {
                char ch=q[i];
                if(!((ch>='a' && ch<='z') || (ch>='0' && ch<='9') || ch=='_' || ch=='-')) {out->ok=0;return;}
                out->iconstyle[i]=ch;
            }
            continue;
        }
        if (starts_with(ls, le, "glyph ")) {
            const char *q=ls+6; au8 bb[4];
            if(le-q<13 || !a_pcfg_bbid(q,bb) || q[11]!=' ') {out->ok=0;return;}
            int n=(int)(le-q-12), found=0;
            if(n<1 || n>=A_PCFG_LABEL) {out->ok=0;return;}
            for(int i=0;i<n;i++) if(!((q[12+i]>='a' && q[12+i]<='z') || q[12+i]=='-')) {out->ok=0;return;}
            for(int i=0;i<out->n;i++) if(a_memcmp(out->cells[i].bbid,bb,4)==0) {
                a_memcpy(out->cells[i].glyph,q+12,(ausize)n); found=1;
            }
            if(!found) {out->ok=0;return;}
            continue;
        }
        if (le-ls==5 && starts_with(ls,le,"empty")) {
            if(out->n>=A_PCFG_MAX_CELLS) {out->ok=0;return;}
            out->n++; continue;
        }
        if (starts_with(ls, le, "cell ")) {
            const char *q = ls + 5;
            if (le - q < 11) { out->ok = 0; return; }   /* need <four-byte-hex-id> */
            if (out->n >= A_PCFG_MAX_CELLS) { out->ok=0; out->n=0; return; }    /* bar cap; ignore extras */
            au8 bb[4];
            if (!a_pcfg_bbid(q, bb)) { out->ok = 0; return; }
            if (le-q>11 && q[11]!=' ' && q[11]!='\t') { out->ok=0; return; }
            for(int i=0;i<out->n;i++) if(a_memcmp(out->cells[i].bbid,bb,4)==0) {out->ok=0; return;}
            a_memcpy(out->cells[out->n].bbid, bb, 4);
            /* label = remainder after "<four-byte-hex-id> " */
            const char *lp = q + 11;
            while (lp < le && (*lp == ' ' || *lp == '\t')) lp++;
            int k = 0;
            while (lp < le && k < A_PCFG_LABEL - 1) out->cells[out->n].label[k++] = *lp++;
            out->cells[out->n].label[k] = 0;
            out->n++;
            continue;
        }
        /* unknown line: ignore (forward-compat) */
    }
    out->ok = (seen_header && out->n > 0) ? 1 : 0;   /* fail closed if nothing parsed */
    if (!out->ok) out->n = 0;
}

static int ws(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; }
/* Validate the complete JSON before extracting membership. A partial or malformed
 * document must not leave an apparently valid subset of T2 available to RF. */
static void skip_ws(const char **p,const char *end) { while(*p<end && ws(**p)) (*p)++; }
static int json_string(const char **p,const char *end) {
    if(*p>=end || *(*p)++!='"') return 0;
    while(*p<end) {
        unsigned char ch=(unsigned char)*(*p)++;
        if(ch=='"') return 1;
        if(ch<32) return 0;
        if(ch=='\\') {
            if(*p>=end) return 0;
            ch=(unsigned char)*(*p)++;
            if(ch=='u') { for(int i=0;i<4;i++) {if(*p>=end || hexnib(*(*p)++)<0) return 0;} }
            else if(ch!='"' && ch!='\\' && ch!='/' && ch!='b' && ch!='f' && ch!='n' && ch!='r' && ch!='t') return 0;
        }
    }
    return 0;
}
static int json_value(const char **p,const char *end,int depth) {
    skip_ws(p,end); if(*p>=end || depth>32) return 0;
    char ch=**p;
    if(ch=='"') return json_string(p,end);
    if(ch=='{' || ch=='[') {
        char close=ch=='{'?'}':']'; (*p)++; skip_ws(p,end);
        if(*p<end && **p==close) {(*p)++;return 1;}
        for(;;) {
            if(ch=='{') {
                if(!json_string(p,end)) return 0;
                skip_ws(p,end); if(*p>=end || *(*p)++!=':') return 0;
            }
            if(!json_value(p,end,depth+1)) return 0;
            skip_ws(p,end); if(*p>=end) return 0;
            char sep=*(*p)++; if(sep==close) return 1;
            if(sep!=',') return 0;
            skip_ws(p,end);
        }
    }
    const char *lit=ch=='t'?"true":ch=='f'?"false":ch=='n'?"null":0;
    if(lit) {while(*lit) if(*p>=end || *(*p)++!=*lit++) return 0;return 1;}
    if(ch=='-') (*p)++;
    if(*p>=end) return 0;
    if(**p=='0') (*p)++;
    else {if(**p<'1' || **p>'9') return 0;while(*p<end && **p>='0' && **p<='9') (*p)++;}
    if(*p<end && **p=='.') {(*p)++;const char *start=*p;while(*p<end && **p>='0' && **p<='9') (*p)++;if(*p==start) return 0;}
    if(*p<end && (**p=='e' || **p=='E')) {
        (*p)++;if(*p<end && (**p=='+' || **p=='-')) (*p)++;
        const char *start=*p;while(*p<end && **p>='0' && **p<='9') (*p)++;if(*p==start) return 0;
    }
    return 1;
}
int a_pcfg_parse_t2(const char *json, ausize len, au8 (*macs)[4], int cap)
{
    if (!json || !macs || cap <= 0) return 0;
    int n=0; const char *p=json, *end=json+len;
    while(p<end && ws(*p)) p++;
    while(end>p && ws(end[-1])) end--;
    if(end-p<2 || *p!='{' || end[-1]!='}') return 0;
    const char *validated=p;
    if(!json_value(&validated,end,0)) return 0;
    skip_ws(&validated,end); if(validated!=end) return 0;
    int depth=0;
    while (p<end) {
        if(*p=='{' || *p=='[') {depth++; p++; continue;}
        if(*p=='}' || *p==']') {if(--depth<0) return 0; p++; continue;}
        if (*p++ != '"') continue;
        const char *key=p;
        int escaped=0;
        while(p<end && *p!='"') {
            if(*p=='\\') {escaped=1; p++; if(p==end) return 0;}
            p++;
        }
        if(p==end) return 0;
        const char *ke=p++;
        if(depth!=1 || escaped || !starts_with(key,ke,"bb_mac_addr")) continue;
        p=key;
        p+=11;
        if (p<end && *p=='_') { p++; const char *d=p; while(p<end && *p>='0' && *p<='9') p++; if(p==d) {p=ke+1; continue;} }
        if (p!=ke) {p=ke+1; continue;}
        p++;
        while(p<end && ws(*p)) p++;
        if(p>=end || *p++!=':') continue; /* a string value, not a key */
        while(p<end && ws(*p)) p++;
        if(p>=end || *p++!='[') return 0;
        au8 mac[4]; int ok=1;
        for(int i=0;i<4;i++) {
            while(p<end && ws(*p)) p++;
            if(end-p<6 || p[0]!='"' || p[1]!='0' || (p[2]!='x' && p[2]!='X') ||
                hexnib(p[3])<0 || hexnib(p[4])<0 || p[5]!='"') { ok=0; break; }
            mac[i]=(au8)((hexnib(p[3])<<4)|hexnib(p[4])); p+=6;
            while(p<end && ws(*p)) p++;
            if(p>=end || *p++!=(i==3?']':',')) { ok=0; break; }
        }
        if(!ok) return 0; /* malformed recognized T2 entry invalidates membership */
        if(a_sw_target_valid(mac)) {
            int duplicate=0; for(int i=0;i<n;i++) if(a_memcmp(macs[i],mac,4)==0) duplicate=1;
            if(!duplicate) { if(n>=cap) return 0; a_memcpy(macs[n++],mac,4); }
        }
    }
    return depth==0?n:0;
}
