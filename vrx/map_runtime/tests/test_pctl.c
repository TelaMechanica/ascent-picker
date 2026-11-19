/* test_pctl.c — Picker controller: RC + manual SELECT both route through the ONE engine,
 * activation/validity/PWM come only from the core, fail-closed. Links PRODUCTION modules.
 * The engine commit is mocked (records the target); no hardware, no socket. */
#include "atest.h"
#include "../src/ascent_picker_ctl.h"

typedef struct {
    au64 clock;
    int  commits;
    au8  last_target[4];
    int  fail;              /* commit returns <0 (transport error) */
} mockeng;

static int m_commit(void *ctx, const au8 target[4], a_sw_result *out)
{
    mockeng *m = ctx; a_memset(out, 0, sizeof(*out));
    if (m->fail==1) return -1;
    m->commits++; a_memcpy(m->last_target, target, 4);
    out->outcome = A_SW_OUT_SWITCHED; out->phase = A_SW_VERIFIED;
    if(m->fail>=2) {out->outcome=A_SW_OUT_NO_REACQUIRE;out->phase=A_SW_FAILED;
        out->recovered=1;out->recovery_ok=m->fail==2;}
    return 0;
}
static au64 m_now(void *ctx){ return ((mockeng*)ctx)->clock; }

static a_pctl_ops OPS(mockeng *m){ a_pctl_ops o; o.ctx=m; o.commit=m_commit; o.now_ms=m_now; return o; }

/* three distinct bbids */
static const au8 A[4]={0xF2,0x00,0x00,0x1C};
static const au8 B[4]={0xF2,0x00,0x00,0x33};
static const au8 C[4]={0xF2,0x00,0x00,0x0A};

int main(void)
{
    SUITE("Picker controller (RC + manual SELECT through one engine)");
    {
        a_pctl c; a_pctl_init(&c,0);
        au8 cells[2][4]; a_memcpy(cells[0],A,4); a_memcpy(cells[1],B,4);
        a_pctl_set_config(&c,cells,2,cells,2);
        mockeng m; a_memset(&m,0,sizeof m); a_pctl_ops o=OPS(&m); a_sw_result r;
        c.busy=1;
        CHECK(!a_pctl_quiesce(&c),"busy owner refuses unload barrier");
        CHECK(!c.quiesced,"refused barrier cannot stop a transaction");
        c.busy=0; c.faulted=1;
        CHECK(!a_pctl_quiesce(&c),"unresolved RF fault refuses unload barrier");
        c.faulted=0;
        CHECK(a_pctl_quiesce(&c),"idle owner establishes unload barrier");
        CHECK(!a_pctl_select_bbid(&c,&o,A,&r),"quiesced owner refuses manual RF");
        a_pctl_on_source(&c,&o,1250,1,1,&r);
        a_pctl_on_source(&c,&o,1750,1,1,&r);
        CHECK_EQI(m.commits,0,"quiesced owner refuses RC RF");
        a_pctl_set_config(&c,cells,2,cells,2);
        CHECK(c.quiesced,"identical config cannot clear unload barrier");
        a_memcpy(cells[1],C,4); a_pctl_set_config(&c,cells,2,cells,2);
        CHECK(c.quiesced,"changed config cannot clear unload barrier");
        CHECK(!a_pctl_select_bbid(&c,&o,C,&r),"config reload cannot resume switching");
        a_pctl_init(&c,0);
        CHECK(!c.quiesced,"new runtime starts with a new lifecycle");
    }
    {
        a_pctl c;a_pctl_init(&c,0);au8 cells[2][4];a_memcpy(cells[0],A,4);a_memcpy(cells[1],B,4);
        a_pctl_set_config(&c,cells,2,cells,2);
        mockeng m;memset(&m,0,sizeof m);a_pctl_ops o=OPS(&m);a_sw_result r;
        a_pctl_on_rc(&c,&o,1250,1,&r);m.fail=2;
        CHECK(a_pctl_on_rc(&c,&o,1750,1,&r),"unavailable target attempted once");
        CHECK(!c.faulted && !c.has_target,"completed recovery keeps RC available without inventing selection");
        CHECK(!a_pctl_on_rc(&c,&o,1750,1,&r),"held failed position is never automatically retried");
        m.fail=0;CHECK(a_pctl_on_rc(&c,&o,1250,1,&r),"next deliberate RC pick works without restart/config write");
        CHECK(c.has_target && !c.faulted,"next successful pick verifies normally");
        m.fail=3;CHECK(a_pctl_on_rc(&c,&o,1750,1,&r),"cleanup failure retained");
        CHECK(c.faulted && !c.has_target,"unconfirmed cleanup does not authorize another RF operation");
    }

    /* Six-cell bench shape: successful RC handoffs and identical background
     * reconciles must not create a one-pick lock or undo a manual selection. */
    {
        a_pctl c; a_pctl_init(&c,400);
        au8 cells[6][4]={{0xF2,0x00,0x00,0x33},{0xF2,0x00,0x00,0x32},
            {0xF2,0x00,0x00,0x2E},{0xF2,0x00,0x00,0x0A},
            {0xF2,0x00,0x00,0x0C},{0xF2,0x00,0x00,0x0F}};
        a_pctl_set_config(&c,cells,6,cells,6);
        mockeng m; a_memset(&m,0,sizeof m);a_pctl_ops o=OPS(&m);a_sw_result r;
        a_pctl_on_source(&c,&o,988,1,1,&r);m.clock=400;
        a_pctl_on_source(&c,&o,988,1,1,&r);
        CHECK_EQI(m.commits,0,"988us establishes first of six cells without RF");
        for(int k=1;k<6;k++) {
            int us=a_cells_center_us(k+1,6);
            m.clock+=20;a_pctl_on_source(&c,&o,us,1,1,&r);
            a_pctl_set_config(&c,cells,6,cells,6);
            m.clock+=400;a_pctl_on_source(&c,&o,us,1,1,&r);
            CHECK_EQI(m.commits,k,"next RC cell remains usable after successful pick/reconcile");
            CHECK(!c.faulted && c.has_target,"success does not latch failure");
            m.clock+=20;a_pctl_on_source(&c,&o,us,1,1,&r);
            CHECK_EQI(m.commits,k,"held RC position does not issue another switch");
        }
        a_pctl_select_cell(&c,&o,0,&r);
        m.clock+=500;a_pctl_on_source(&c,&o,1917,1,1,&r);
        CHECK_EQI(m.commits,6,"held RC cannot fight manual SELECT");
        m.clock+=20;a_pctl_on_source(&c,&o,1250,1,1,&r);
        m.clock+=400;a_pctl_on_source(&c,&o,1250,1,1,&r);
        CHECK_EQI(m.commits,7,"deliberate RC move after manual SELECT still works");
    }

    /* ---- config / activation (fail closed) -------------------------- */
    {
        a_pctl c; a_pctl_init(&c, 0);
        au8 cfg[1][4]; a_memcpy(cfg[0],A,4);
        au8 t2[1][4];  a_memcpy(t2[0],A,4);
        CHECK_EQI(a_pctl_set_config(&c, cfg, 1, t2, 1), 0, "1 valid => disabled");
        CHECK_EQI(c.reason, A_PCTL_DISABLED_FEW_VALID, "reason: few valid");
    }
    {
        a_pctl c; a_pctl_init(&c, 0);
        CHECK_EQI(a_pctl_set_config(&c, 0, 0, 0, 0), 0, "no config => disabled");
        CHECK_EQI(c.reason, A_PCTL_DISABLED_MALFORMED, "reason: malformed");
    }
    {   /* 4 configured, only 2 in T2 => enabled as N=2, invalid ones excluded */
        a_pctl c; a_pctl_init(&c, 0);
        au8 cfg[4][4]; a_memcpy(cfg[0],A,4); a_memcpy(cfg[1],B,4);
        cfg[2][0]=0x11;cfg[2][1]=0x22;cfg[2][2]=0x33;cfg[2][3]=0x44; a_memcpy(cfg[3],C,4);
        au8 t2[2][4]; a_memcpy(t2[0],A,4); a_memcpy(t2[1],B,4);   /* only A,B in T2 */
        CHECK_EQI(a_pctl_set_config(&c, cfg, 4, t2, 2), 1, "4 cfg / 2 in T2 => enabled");
        CHECK_EQI(c.active_n, 2, "active_n = 2 (not 4)");
        CHECK(c.active_idx[0]==0 && c.active_idx[1]==1, "active-valid = the two in T2, in order");
    }

    /* ---- RC: startup establishes, change switches, same cell no retrigger ---- */
    {
        a_pctl c; a_pctl_init(&c, 0);          /* hold_stable_ms = 0 => no debounce wait */
        au8 cfg[2][4]; a_memcpy(cfg[0],A,4); a_memcpy(cfg[1],B,4);
        au8 t2[2][4];  a_memcpy(t2[0],A,4);  a_memcpy(t2[1],B,4);
        a_pctl_set_config(&c, cfg, 2, t2, 2);
        mockeng m; a_memset(&m,0,sizeof m); a_pctl_ops o=OPS(&m); a_sw_result r;

        /* startup sample in cell0 establishes, NO switch */
        CHECK_EQI(a_pctl_on_rc(&c,&o,1250,1,&r), 0, "startup sample does not switch");
        CHECK_EQI(m.commits, 0, "no commit on startup establish");
        CHECK_EQI(c.cur_cell, 0, "current cell established = 0");
        /* staying in cell0 never retriggers */
        CHECK_EQI(a_pctl_on_rc(&c,&o,1200,1,&r), 0, "same cell: no switch");
        CHECK_EQI(m.commits, 0, "still no commit");
        /* move to cell1 => one switch to B */
        CHECK_EQI(a_pctl_on_rc(&c,&o,1750,1,&r), 1, "move to cell1 => switch");
        CHECK_EQI(m.commits, 1, "exactly one commit");
        CHECK(a_memcmp(m.last_target,B,4)==0, "committed target is B (cell1 bbid)");
        /* staying in cell1 never retriggers */
        CHECK_EQI(a_pctl_on_rc(&c,&o,1800,1,&r), 0, "stay in cell1: no retrigger");
        CHECK_EQI(m.commits, 1, "still one commit");
        /* back to cell0 => switch to A */
        CHECK_EQI(a_pctl_on_rc(&c,&o,1100,1,&r), 1, "back to cell0 => switch");
        CHECK(a_memcmp(m.last_target,A,4)==0, "committed target is A");
    }

    /* ---- disabled => no RC switch at all ---------------------------- */
    {
        a_pctl c; a_pctl_init(&c, 0);
        au8 cfg[2][4]; a_memcpy(cfg[0],A,4); a_memcpy(cfg[1],B,4);
        au8 t2[1][4]; a_memcpy(t2[0],A,4);   /* only A valid => N=1 => disabled */
        a_pctl_set_config(&c, cfg, 2, t2, 1);
        mockeng m; a_memset(&m,0,sizeof m); a_pctl_ops o=OPS(&m); a_sw_result r;
        CHECK_EQI(a_pctl_on_rc(&c,&o,1750,1,&r), 0, "disabled: RC never switches");
        CHECK_EQI(a_pctl_select_cell(&c,&o,0,&r), 0, "disabled: manual SELECT refused");
        CHECK_EQI(m.commits, 0, "disabled: zero commits from any path");
    }

    /* ---- invalid/stale RC: hold --------------------------------------- */
    {
        a_pctl c; a_pctl_init(&c, 0);
        au8 cfg[2][4]; a_memcpy(cfg[0],A,4); a_memcpy(cfg[1],B,4);
        au8 t2[2][4]; a_memcpy(t2[0],A,4); a_memcpy(t2[1],B,4);
        a_pctl_set_config(&c, cfg, 2, t2, 2);
        mockeng m; a_memset(&m,0,sizeof m); a_pctl_ops o=OPS(&m); a_sw_result r;
        a_pctl_on_rc(&c,&o,1250,1,&r);                 /* establish cell0 */
        CHECK_EQI(a_pctl_on_rc(&c,&o,1750,0,&r), 0, "valid=0 (failsafe) => no switch");
        CHECK_EQI(a_pctl_on_rc(&c,&o,800,1,&r), 0, "out-of-range 800us => no switch");
        CHECK_EQI(m.commits, 0, "stale/out-of-range never commits");
    }

    /* ---- debounce: a changed cell must hold hold_stable_ms ----------- */
    {
        a_pctl c; a_pctl_init(&c, 100);                /* 100 ms debounce */
        au8 cfg[2][4]; a_memcpy(cfg[0],A,4); a_memcpy(cfg[1],B,4);
        au8 t2[2][4]; a_memcpy(t2[0],A,4); a_memcpy(t2[1],B,4);
        a_pctl_set_config(&c, cfg, 2, t2, 2);
        mockeng m; a_memset(&m,0,sizeof m); a_pctl_ops o=OPS(&m); a_sw_result r;
        /* establish cell0 (needs to hold 100ms) */
        a_pctl_on_rc(&c,&o,1250,1,&r); m.clock=100; a_pctl_on_rc(&c,&o,1250,1,&r);
        CHECK_EQI(c.cur_cell, 0, "cell0 established after debounce");
        m.clock=200;
        CHECK_EQI(a_pctl_on_rc(&c,&o,1750,1,&r), 0, "cell1 seen but not yet held => no switch");
        m.clock=250;
        CHECK_EQI(a_pctl_on_rc(&c,&o,1750,1,&r), 0, "still within debounce => no switch");
        m.clock=300;
        CHECK_EQI(a_pctl_on_rc(&c,&o,1750,1,&r), 1, "held 100ms => switch");
        CHECK_EQI(m.commits, 1, "one commit after debounce");
    }

    /* ---- manual SELECT uses the SAME engine path -------------------- */
    {
        a_pctl c; a_pctl_init(&c, 0);
        au8 cfg[3][4]; a_memcpy(cfg[0],A,4); a_memcpy(cfg[1],B,4); a_memcpy(cfg[2],C,4);
        au8 t2[3][4]; a_memcpy(t2[0],A,4); a_memcpy(t2[1],B,4); a_memcpy(t2[2],C,4);
        a_pctl_set_config(&c, cfg, 3, t2, 3);
        mockeng m; a_memset(&m,0,sizeof m); a_pctl_ops o=OPS(&m); a_sw_result r;
        CHECK_EQI(a_pctl_select_cell(&c,&o,2,&r), 1, "manual SELECT cell2 commits");
        CHECK(a_memcmp(m.last_target,C,4)==0, "manual SELECT target is C (same engine commit)");
        CHECK_EQI(a_pctl_select_cell(&c,&o,2,&r), 0, "re-select same target => no retrigger");
        CHECK_EQI(a_pctl_select_bbid(&c,&o,A,&r), 1, "SELECT by exact bbid A commits");
        CHECK(a_memcmp(m.last_target,A,4)==0, "bbid SELECT target is A");
        /* a bbid not active-valid is refused */
        au8 bad[4]={0xF2,0x00,0x00,0x16};
        CHECK_EQI(a_pctl_select_bbid(&c,&o,bad,&r), 0, "SELECT of a non-active-valid bbid refused");
    }

    /* ---- T2 change drops below 2 => disables (§11) ------------------ */
    {
        a_pctl c; a_pctl_init(&c, 0);
        au8 cfg[2][4]; a_memcpy(cfg[0],A,4); a_memcpy(cfg[1],B,4);
        au8 t2a[2][4]; a_memcpy(t2a[0],A,4); a_memcpy(t2a[1],B,4);
        CHECK_EQI(a_pctl_set_config(&c, cfg, 2, t2a, 2), 1, "both in T2 => enabled");
        au8 t2b[1][4]; a_memcpy(t2b[0],A,4);           /* B removed from T2 */
        CHECK_EQI(a_pctl_set_config(&c, cfg, 2, t2b, 1), 0, "T2 shrinks => disabled");
        CHECK_EQI(c.reason, A_PCTL_DISABLED_FEW_VALID, "reason after shrink");
    }

    {
        a_pctl c; a_pctl_init(&c, 87);
        au8 cfg[2][4]; a_memcpy(cfg[0], A, 4); a_memcpy(cfg[1], B, 4);
        a_pctl_set_config(&c,cfg,2,cfg,2);
        mockeng m; a_memset(&m,0,sizeof m); a_pctl_ops o=OPS(&m); a_sw_result res;
        a_pctl_on_rc(&c,&o,1250,1,&res); m.clock=87; a_pctl_on_rc(&c,&o,1250,1,&res);
        CHECK(!c.has_target && c.verified_cell == -1,"startup input is not verified identity");
        a_pctl_select_bbid(&c,&o,A,&res);
        CHECK(c.has_target && c.verified_cell==0,"stable engine outcome promotes verified identity");
        c.pending_cell=1; c.pending_since=123;
        a_pctl_set_config(&c,cfg,2,cfg,2);
        CHECK(c.has_target && c.pending_since==123 && c.pending_cell==1 && c.have_last,"unchanged config preserves state");
        c.busy=1;
        CHECK(!a_pctl_select_bbid(&c,&o,B,&res),"busy manual refused");
        CHECK(!a_pctl_on_rc(&c,&o,1750,1,&res),"busy RC refused");
        c.busy=0; m.fail=1;
        a_pctl_select_bbid(&c,&o,B,&res);
        CHECK(!c.has_target && c.faulted,"failed switch never promotes target and stops further attempts");
        CHECK(c.commanded_target[0]==B[0],"commanded target distinct from verified");
        a_pctl_set_config(&c,cfg,2,cfg,2);
        CHECK(c.faulted && c.have_last,"unchanged reload preserves failure latch");
        CHECK(!a_pctl_select_bbid(&c,&o,A,&res),"failure cannot be blindly retried");
        a_pctl_set_config(&c,cfg,2,cfg,1);
        CHECK(!c.enabled && !c.has_target,"T2 shrink fails closed and clears identity");
    }

    {
        a_pctl c; a_pctl_init(&c, 100);
        au8 cells[2][4]; a_memcpy(cells[0],A,4); a_memcpy(cells[1],B,4);
        a_pctl_set_config(&c,cells,2,cells,2);
        mockeng m; a_memset(&m,0,sizeof m); a_pctl_ops o=OPS(&m); a_sw_result r;
        a_pctl_on_rc(&c,&o,1250,1,&r); m.clock=100;
        a_pctl_on_rc(&c,&o,1250,1,&r);
        m.clock=110; a_pctl_on_rc(&c,&o,1750,1,&r);
        m.clock=300; a_pctl_on_rc(&c,&o,1750,0,&r);
        m.clock=310; a_pctl_on_rc(&c,&o,1750,1,&r);
        CHECK_EQI(m.commits,0,"stale gap restarts debounce rather than committing");
        m.clock=410; a_pctl_on_rc(&c,&o,1750,1,&r);
        CHECK_EQI(m.commits,1,"continuous fresh hold commits once");
    }

    {
        a_pctl c; a_pctl_init(&c,0);
        au8 cells[2][4]; a_memcpy(cells[0],A,4); a_memcpy(cells[1],B,4);
        a_pctl_set_config(&c,cells,2,cells,2);
        mockeng m; a_memset(&m,0,sizeof m); a_pctl_ops o=OPS(&m); a_sw_result r;
        a_pctl_on_rc(&c,&o,1250,1,&r);
        a_pctl_select_bbid(&c,&o,B,&r);
        CHECK_EQI(c.cur_cell,0,"manual SELECT preserves RC position latch");
        a_pctl_on_rc(&c,&o,1250,1,&r);
        CHECK_EQI(m.commits,1,"unchanged stick cannot undo manual SELECT");
        a_pctl_on_rc(&c,&o,1750,1,&r);
        CHECK_EQI(m.commits,1,"moving RC onto verified target needs no RF retry");
        a_pctl_on_rc(&c,&o,1250,1,&r);
        CHECK_EQI(m.commits,2,"next actual RC position change uses same engine");
    }


    {
        a_pctl c; a_pctl_init(&c,0);au8 cells[2][4];a_memcpy(cells[0],A,4);a_memcpy(cells[1],B,4);
        a_pctl_set_config(&c,cells,2,cells,2);c.cooldown_ms=20000;
        mockeng m;memset(&m,0,sizeof m);a_pctl_ops o=OPS(&m);a_sw_result r;
        a_pctl_select_bbid(&c,&o,A,&r);
        CHECK_EQI(c.cooldown_remaining_ms,20000,"successful switch publishes advisory timer");
        CHECK(a_pctl_select_bbid(&c,&o,B,&r),"advisory never blocks manual SELECT");
        a_pctl_on_rc(&c,&o,1750,1,&r); /* establishes input without RF */
        CHECK(a_pctl_on_rc(&c,&o,1250,1,&r),"advisory never blocks RC selection");
        CHECK_EQI(m.commits,3,"both paths execute during advisory");
        a_pctl_update_time(&c,20001);CHECK_EQI(c.cooldown_remaining_ms,0,"advisory expires");
        c.cooldown_ms=0;a_pctl_select_bbid(&c,&o,B,&r);
        CHECK_EQI(c.cooldown_remaining_ms,0,"zero hides advisory");
        m.fail=1;a_pctl_select_bbid(&c,&o,A,&r);
        CHECK(c.faulted,"failure diagnostic stop remains");
        CHECK_EQI(c.cooldown_remaining_ms,0,"failed attempt does not masquerade as timed cooldown");
    }

    {
        a_pctl c;a_pctl_init(&c,0);au8 cells[2][4];a_memcpy(cells[0],A,4);a_memcpy(cells[1],B,4);
        a_pctl_set_config(&c,cells,2,cells,2);
        mockeng m;memset(&m,0,sizeof m);a_pctl_ops o=OPS(&m);a_sw_result r;
        CHECK(!a_pctl_on_source(&c,&o,1250,1,1,&r),"initial source establishes without RF");
        CHECK(!a_pctl_on_source(&c,&o,1750,1,2,&r),"changed source cannot immediately select a different aircraft");
        CHECK_EQI(m.commits,0,"source change did not execute RF");
        CHECK(a_pctl_on_source(&c,&o,1250,1,2,&r),"subsequent real position change uses existing controller");
        CHECK(!c.rc_fresh && c.rc_pwm_us==0,"RF transaction invalidates cached freshness");
    }
    {
        a_pctl c; a_pctl_init(&c,0); au8 cells[6][4]={{0}};
        a_memcpy(cells[0],A,4); a_memcpy(cells[2],B,4); a_memcpy(cells[5],C,4);
        au8 t2[3][4];a_memcpy(t2[0],A,4);a_memcpy(t2[1],B,4);a_memcpy(t2[2],C,4);
        a_pctl_set_config(&c,cells,6,t2,3);
        mockeng m;memset(&m,0,sizeof m);a_pctl_ops o=OPS(&m);a_sw_result r;
        CHECK(!a_pctl_on_rc(&c,&o,1083,1,&r),"six positions establish without RF");
        CHECK(!a_pctl_on_rc(&c,&o,1250,1,&r),"empty second position never selects third");
        CHECK(a_pctl_on_rc(&c,&o,1417,1,&r),"third position selects third configured cell");
        CHECK(!memcmp(m.last_target,B,4),"position three resolves exact B");
        CHECK(!a_pctl_on_rc(&c,&o,1750,1,&r),"empty fifth position holds current selection");
        CHECK(a_pctl_on_rc(&c,&o,1917,1,&r),"sixth position selects sixth configured cell");
        CHECK(!memcmp(m.last_target,C,4),"position six resolves exact C");
        CHECK_EQI(m.commits,2,"empty positions cannot emit RF");
    }
    return atest_report();
}
