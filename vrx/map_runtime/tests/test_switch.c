/* test_switch.c — the VRX-local Picker switch engine. Links the PRODUCTION module.
 * Mocks the device boundary (a_sw_ops) and a fake clock, so the whole proven sequence
 * and all five locked invariants are exercised with no hardware. */
#include "atest.h"
#include "../src/ascent_switch.h"

/* ---- mock device ---- */
typedef struct {
    au64 clock;
    int  mode;            /* 0 drop-then-connect, 1 never-drop, 2 drop-never-connect */
    int  read_calls;
    int  fail_setcand;    /* inject a set_candidates error on the Nth call (1-based), 0=never */
    int  setcand_calls;
    int  fail_standby;    /* 0=never (default), 1=fail standby-ON, 2=fail standby-OFF */
    int  pair_calls;
    /* ordered event log */
    char ev[80][16]; int nev;
    int  last_setcand_count;
} mockdev;

static int a_strncmp_(const char *x, const char *y); /* fwd */
static void logev(mockdev *m, const char *s)
{ if (m->nev < 80) { int i=0; while (s[i] && i<15){ m->ev[m->nev][i]=s[i]; i++; } m->ev[m->nev][i]=0; m->nev++; } }
static int ev_before(mockdev *m, const char *a, const char *b)
{ int ia=-1, ib=-1; for (int i=0;i<m->nev;i++){ if(ia<0&&!a_strncmp_(m->ev[i],a)) ia=i; if(ib<0&&!a_strncmp_(m->ev[i],b)) ib=i; } return ia>=0 && ib>=0 && ia<ib; }
static int ev_count(mockdev *m, const char *a)
{ int n=0; for(int i=0;i<m->nev;i++) if(!a_strncmp_(m->ev[i],a)) n++; return n; }

/* tiny strcmp (atest links freestanding-ish); returns 0 on equal */
static int a_strncmp_(const char *x, const char *y){ int i=0; for(;x[i]&&y[i];i++) if(x[i]!=y[i]) return 1; return x[i]!=y[i]; }

static int m_standby(void *c, int mode){ mockdev*m=c; logev(m, mode?"SB1":"SB0");
    if ((m->fail_standby==1&&mode==1)||(m->fail_standby==2&&mode==0)) return -1; return 0; }
static int m_setcand(void *c, const au8 macs[][4], int count){ mockdev*m=c; m->setcand_calls++;
    m->last_setcand_count=count; logev(m, count==1?"CAND1":"CANDN");
    if (m->fail_setcand==m->setcand_calls) return -1; return 0; }
static int m_pair(void *c){ mockdev*m=c; m->pair_calls++; logev(m,"PAIR"); return 0; }
static int m_read(void *c, int *st){ mockdev*m=c; m->read_calls++;
    switch(m->mode){
      case 0: *st=(m->read_calls==1)?A_SW_LINK_IDLE:A_SW_LINK_CONNECT; break; /* drop then connect */
      case 1: *st=A_SW_LINK_CONNECT; break;                                    /* never drops */
      case 2: *st=(m->read_calls==1)?A_SW_LINK_IDLE:A_SW_LINK_LOCK; break;     /* drop, never CONNECT */
      case 3: *st=m->clock<18000 ? A_SW_LINK_IDLE:A_SW_LINK_CONNECT; break;
      default:*st=A_SW_LINK_CONNECT; }
    return 0; }
static au64 m_now(void *c){ return ((mockdev*)c)->clock; }
static void m_sleep(void *c, au32 ms){ ((mockdev*)c)->clock += ms; }

static a_sw_ops OPS(mockdev *m){ a_sw_ops o; a_memset(&o,0,sizeof o); o.ctx=m;
    o.send_standby=m_standby; o.set_candidates=m_setcand; o.pair_rearm=m_pair;
    o.read_link=m_read; o.now_ms=m_now; o.sleep_ms=m_sleep; return o; }

static a_sw_cfg CFG(void){ a_sw_cfg c; a_memset(&c,0,sizeof c);
    c.target[0]=0xE6; c.target[1]=0xEC; c.target[2]=0x52; c.target[3]=0x2F;
    c.whitelist[0][0]=0xA5; c.whitelist[0][1]=0xFE; c.whitelist[0][2]=0xDA; c.whitelist[0][3]=0xEA;
    c.whitelist[1][0]=0xE6; c.whitelist[1][1]=0xEC; c.whitelist[1][2]=0x52; c.whitelist[1][3]=0x2F;
    c.whitelist_n=2; c.park_enabled=1; c.rearm_enabled=1;
    c.drop_wait_ms=300; c.connect_stable_ms=100; c.acquire_window_ms=1000; c.poll_ms=50;
    return c; }

int main(void)
{
    SUITE("VRX-local Picker switch engine");
    {
        /* Same delayed device evidence: the old adapter aborted; the SOP
         * ceiling permits completion without adding a minimum wait. */
        mockdev m; a_memset(&m,0,sizeof m); m.mode=3;
        a_sw_ops o=OPS(&m); a_sw_cfg c=CFG(); a_sw_result r;
        c.acquire_window_ms=12000;
        CHECK_EQI(a_switch_execute(&c,&o,&r),A_SW_OUT_NO_REACQUIRE,"old ceiling abandons 18-second acquisition");
        a_memset(&m,0,sizeof m);m.mode=3;o=OPS(&m);
        c.acquire_window_ms=A_SW_ACQUIRE_WINDOW_MS;
        CHECK_EQI(a_switch_execute(&c,&o,&r),A_SW_OUT_SWITCHED,"SOP ceiling accepts delayed stable CONNECT");
        CHECK_EQI(ev_count(&m,"CANDN"),0,"no recovery widening on valid delayed acquisition");
        CHECK_EQI(m.clock,18100,"observation ceiling does not force full wait");
        a_memset(&m,0,sizeof m);o=OPS(&m);
        CHECK_EQI(a_switch_execute(&c,&o,&r),A_SW_OUT_SWITCHED,"fast acquisition remains fast");
        CHECK_EQI(m.clock,100,"no added delay on fast path");
    }
    {
        for(int failed=0;failed<3;failed++) {
            mockdev m;a_memset(&m,0,sizeof m);m.mode=2;
            if(failed==1)m.fail_setcand=2;
            if(failed==2)m.fail_standby=2;
            a_sw_ops o=OPS(&m);a_sw_cfg c=CFG();a_sw_result r;
            CHECK_EQI(a_switch_execute(&c,&o,&r),A_SW_OUT_NO_REACQUIRE,"unavailable target keeps original failure outcome");
            CHECK_EQI(r.recovery_ok,failed==0,"recovery completion reflects actual cleanup errors");
            CHECK_EQI(ev_count(&m,"CANDN"),1,"one cleanup, never a target retry");
        }
    }

    /* ---- frame builders (byte-exact) -------------------------------- */
    {
        au8 f[512];
        int n = a_sw_candidate_frame((const au8(*)[4])CFG().whitelist, 1, f, sizeof f);
        CHECK_EQI(n, A_SW_USBPACK_HDR + A_SW_CANDIDATE_PAYLEN + 1, "candidate frame length");
        CHECK_EQI(f[0], 0xAA, "usbpack start byte");
        CHECK_EQI(f[n-1], 0xBB, "usbpack end byte");
        CHECK_EQI(f[1], (au8)(A_SW_CANDIDATE_PAYLEN & 0xFF), "payload len LE lo");
        CHECK_EQI(f[2], (au8)(A_SW_CANDIDATE_PAYLEN >> 8), "payload len LE hi");
        /* cmd 0x02000004 big-endian at [5..8] */
        CHECK_EQI(f[5],0x02, "cmd BE b0"); CHECK_EQI(f[6],0x00,"cmd BE b1");
        CHECK_EQI(f[7],0x00,"cmd BE b2"); CHECK_EQI(f[8],0x04,"cmd BE b3");
        CHECK_EQI(f[17], a_sw_checksum17(f), "checksum17 self-consistent");
        /* payload: [0]=flags0 [1]=count1 [2..5]=first mac */
        CHECK_EQI(f[A_SW_USBPACK_HDR+0], 0x00, "cand flags");
        CHECK_EQI(f[A_SW_USBPACK_HDR+1], 1,    "cand count");
        CHECK_EQI(f[A_SW_USBPACK_HDR+2], 0xA5, "cand first mac byte");

        int pn = a_sw_pair_frame(1, f, sizeof f);
        CHECK_EQI(pn, A_SW_USBPACK_HDR + 14 + 1, "pair frame length");
        CHECK_EQI(f[5],0x02, "pair cmd BE b0"); CHECK_EQI(f[8],0x02,"pair cmd BE b3=0x02000002");
        CHECK_EQI(f[A_SW_USBPACK_HDR+0], 0x01, "pair ENTER byte0");
        CHECK_EQI(f[A_SW_USBPACK_HDR+1], 0x01, "pair byte1");
        a_sw_pair_frame(0, f, sizeof f);
        CHECK_EQI(f[A_SW_USBPACK_HDR+0], 0x00, "pair EXIT byte0");

        int sn = a_sw_standby_frame(1, 0, 0, f, sizeof f);
        CHECK_EQI(sn, 10, "standby frame length");
        CHECK_EQI(f[0],0x8E,"standby magic0"); CHECK_EQI(f[1],0x25,"standby magic1");
        CHECK_EQI(f[8],56,"standby cmd=56"); CHECK_EQI(f[9],1,"standby mode=1 (ON)");
        au8 ck=0; for(int i=3;i<10;i++) ck^=f[i]; CHECK_EQI(f[2],ck,"standby xor checksum");
    }

    /* ---- HAPPY PATH: drop then CONNECT => SWITCHED ------------------ */
    {
        mockdev m; a_memset(&m,0,sizeof m); m.mode=0;
        a_sw_ops o=OPS(&m); a_sw_cfg c=CFG(); a_sw_result r;
        a_sw_outcome out = a_switch_execute(&c,&o,&r);
        CHECK_EQI(out, A_SW_OUT_SWITCHED, "happy path => SWITCHED");
        CHECK_EQI(r.phase, A_SW_VERIFIED, "reached VERIFIED");
        CHECK_EQI(r.dropped, 1, "measured drop observed");
        CHECK_EQI(r.rearmed, 1, "re-arm invoked (enabled)");
        CHECK_EQI(r.standby_on_sent, 1, "standby ON sent");
        CHECK_EQI(r.standby_off_sent, 1, "standby OFF sent");
        CHECK_EQI(r.recovered, 0, "no recovery on success");
        /* invariant: standby ON before the narrow; narrow before re-arm; standby OFF after acquire */
        CHECK(ev_before(&m,"SB1","CAND1"), "standby-ON precedes the target-only narrow");
        CHECK(ev_before(&m,"CAND1","PAIR"), "narrow precedes the re-arm");
        CHECK(ev_before(&m,"PAIR","SB0"), "re-arm precedes standby-OFF");
        CHECK_EQI(ev_count(&m,"CANDN"), 0, "no recovery widen on success");
    }

    /* ---- NO_DROP: link never leaves CONNECT (invariant #1) --------- */
    {
        mockdev m; a_memset(&m,0,sizeof m); m.mode=1;
        a_sw_ops o=OPS(&m); a_sw_cfg c=CFG(); a_sw_result r;
        a_sw_outcome out = a_switch_execute(&c,&o,&r);
        CHECK_EQI(out, A_SW_OUT_NO_DROP, "never-drop => NO_DROP");
        CHECK_EQI(r.dropped, 0, "no drop recorded");
        CHECK_EQI(m.pair_calls, 0, "re-arm NOT invoked on NO_DROP (invariant #1)");
        CHECK_EQI(r.rearmed, 0, "engine did not re-arm");
        CHECK_EQI(r.recovered, 1, "one-shot recovery widen performed");
        CHECK(ev_before(&m,"CAND1","CANDN"), "narrow then recovery widen");
        CHECK_EQI(ev_count(&m,"CANDN"), 1, "recovery widen is one-shot");
        CHECK_EQI(r.standby_off_sent, 1, "leaving unit restored (standby OFF) on NO_DROP");
        /* and crucially never reached ACQUIRING */
        CHECK(r.phase==A_SW_FAILED, "phase FAILED, never ACQUIRING");
    }

    /* ---- NO_REACQUIRE: drop but no stable CONNECT ------------------- */
    {
        mockdev m; a_memset(&m,0,sizeof m); m.mode=2;
        a_sw_ops o=OPS(&m); a_sw_cfg c=CFG(); a_sw_result r;
        a_sw_outcome out = a_switch_execute(&c,&o,&r);
        CHECK_EQI(out, A_SW_OUT_NO_REACQUIRE, "drop-but-no-connect => NO_REACQUIRE");
        CHECK_EQI(r.dropped, 1, "drop was observed");
        CHECK_EQI(r.recovered, 1, "one-shot recovery widen");
        CHECK_EQI(ev_count(&m,"CANDN"), 1, "recovery widen once, no retry loop (invariant #5)");
    }

    /* ---- re-arm isolation (invariant #2): disabled => never called -- */
    {
        mockdev m; a_memset(&m,0,sizeof m); m.mode=0;
        a_sw_ops o=OPS(&m); a_sw_cfg c=CFG(); c.rearm_enabled=0; a_sw_result r;
        a_sw_outcome out = a_switch_execute(&c,&o,&r);
        CHECK_EQI(out, A_SW_OUT_SWITCHED, "switch still succeeds without re-arm");
        CHECK_EQI(m.pair_calls, 0, "rearm_enabled=0 => pair_rearm never called");
        CHECK_EQI(r.rearmed, 0, "re-arm cleanly removable by one flag");
    }

    /* ---- REFUSED: sentinel target, no RF issued -------------------- */
    {
        mockdev m; a_memset(&m,0,sizeof m); m.mode=0;
        a_sw_ops o=OPS(&m); a_sw_cfg c=CFG(); a_sw_result r;
        c.target[0]=0xFF; c.target[1]=0xFF; c.target[2]=0xFF; c.target[3]=0xFF;
        a_sw_outcome out = a_switch_execute(&c,&o,&r);
        CHECK_EQI(out, A_SW_OUT_REFUSED, "sentinel FF target refused");
        CHECK_EQI(m.setcand_calls, 0, "no SET_CANDIDATES for a refused target");
        CHECK_EQI(m.nev, 0, "no RF of any kind issued");
    }

    /* ---- EXEC_ERR: the narrow fails -> recover --------------------- */
    {
        mockdev m; a_memset(&m,0,sizeof m); m.mode=0; m.fail_setcand=1;  /* first set_candidates fails */
        a_sw_ops o=OPS(&m); a_sw_cfg c=CFG(); a_sw_result r;
        a_sw_outcome out = a_switch_execute(&c,&o,&r);
        CHECK_EQI(out, A_SW_OUT_EXEC_ERR, "narrow transport error => EXEC_ERR");
        CHECK_EQI(r.recovered, 1, "recovery attempted after exec error");
    }

    /* ---- standby-OFF only after CONNECT (invariant #4) ------------- */
    {
        /* On NO_REACQUIRE the only standby-OFF is the recovery RESTORE, never an
         * "unpark joined" -- assert we never reached VERIFIED before any SB0. */
        mockdev m; a_memset(&m,0,sizeof m); m.mode=2;
        a_sw_ops o=OPS(&m); a_sw_cfg c=CFG(); a_sw_result r;
        a_switch_execute(&c,&o,&r);
        CHECK(r.phase!=A_SW_VERIFIED, "never VERIFIED without a stable CONNECT");
    }

    return atest_report();
}
