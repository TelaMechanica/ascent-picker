/* test_picker_glue.c — pure device-glue parsers: config deser (fail-closed), T2 extract,
 * IPC command/status framing, OSD control text. Links PRODUCTION modules. No sockets, no I/O. */
#include "atest.h"
#include "../src/ascent_picker_cfg.h"
#include "../src/ascent_picker_ipc.h"
#include "../src/ascent_picker_osd.h"

static const au8 A[4]={0x02,0x11,0x22,0x31};
static const au8 B[4]={0x06,0x11,0x22,0x32};

int main(void)
{
    SUITE("Picker device glue (pure parsers)");

    /* ---- config parse (valid) --------------------------------------- */
    {
        const char *t =
            "ascent_picker_rt v1\n"
            "hold_ms 150\n"
            "cooldown_ms 12000\n"
            "display_hidden 1\n"
            "rc_channel 7\n"
            "cell 02:" "11:" "22:" "31 Drone A\n"
            "cell 06:" "11:" "22:" "32 Fixture B\n";
        a_pcfg c; a_pcfg_parse(t, a_strnlen(t, 4096), &c);
        CHECK_EQI(c.ok, 1, "valid config parses");
        CHECK_EQI(c.n, 2, "two cells");
        CHECK_EQI(c.display_hidden,1,"hidden config parsed");
        CHECK_EQI(c.hold_ms, 150, "hold_ms parsed");
        CHECK_EQI(c.cooldown_ms,12000,"advisory duration parsed");
        CHECK_EQI(c.rc_channel, 7, "rc_channel parsed");
        CHECK(a_memcmp(c.cells[0].bbid,A,4)==0, "cell0 bbid A");
        CHECK(a_memcmp(c.cells[1].bbid,B,4)==0, "cell1 bbid B");
        CHECK(a_strnlen(c.cells[0].label,32)==7, "label 'Drone A'");
    }
    /* ---- config FAIL CLOSED ----------------------------------------- */
    {
        a_pcfg c;
        a_pcfg_parse("garbage first line\ncell 02:" "11:" "22:" "31 x\n", 40, &c);
        CHECK_EQI(c.ok, 0, "bad header => fail closed");
        CHECK_EQI(c.n, 0, "no cells on failure");
        const char *bad = "ascent_picker_rt v1\ncell ZZ:ZZ:ZZ:ZZ x\n";
        a_pcfg_parse(bad, a_strnlen(bad,256), &c);
        CHECK_EQI(c.ok, 0, "bad bbid => fail closed");
        a_pcfg_parse("", 0, &c);
        CHECK_EQI(c.ok, 0, "empty => fail closed");
        const char *hdr_only = "ascent_picker_rt v1\n";
        a_pcfg_parse(hdr_only, a_strnlen(hdr_only,64), &c);
        CHECK_EQI(c.ok, 0, "header but zero cells => fail closed");
    }
    /* ---- T2 extract from user_cfg.json ------------------------------ */
    {
        const char *j =
            "{ \"bb_mac_addr_0\":[\"0x02\",\"0x11\",\"0x22\",\"0x31\"],"
            "  \"bb_mac_addr_1\":[\"0x06\",\"0x11\",\"0x22\",\"0x32\"],"
            "  \"bb_mac_addr_2\":[\"0xFF\",\"0xFF\",\"0xFF\",\"0xFF\"] }";
        au8 macs[8][4]; int n = a_pcfg_parse_t2(j, a_strnlen(j,512), macs, 8);
        CHECK_EQI(n, 2, "two real T2 macs (FF sentinel dropped)");
        CHECK(a_memcmp(macs[0],A,4)==0, "T2 mac0 = A");
        CHECK(a_memcmp(macs[1],B,4)==0, "T2 mac1 = B");
    }

    {
        au8 macs[8][4];
        const char *j="{\"bb_mac_addr_0\":[\"0x02\",\"0x11\",\"0x22\",\"0x31\"],\"bb_mac_addr_1\":null}";
        CHECK_EQI(a_pcfg_parse_t2(j,a_strnlen(j,512),macs,8),0,"malformed membership invalidates T2");
        j="{\"note\":\"bb_mac_addr_1\",\"bb_mac_addr_0\":[\"0x02\",\"0x11\",\"0x22\",\"0x31\"]}";
        CHECK_EQI(a_pcfg_parse_t2(j,a_strnlen(j,512),macs,8),1,"string values cannot inject T2 membership");
        CHECK_EQI(a_pcfg_parse_t2(j,a_strnlen(j,512)-1,macs,8),0,"truncated T2 object fails closed");
    }

    /* ---- IPC command parse ----------------------------------------- */
    {
        a_pipc_cmd cmd;
        CHECK(a_pipc_parse("STATUS\n", 7, &cmd) && cmd.kind==A_PIPC_STATUS, "STATUS parsed");
        CHECK(a_pipc_parse("QUIESCE\n",8,&cmd) && cmd.kind==A_PIPC_QUIESCE,"QUIESCE parsed");
        CHECK(!a_pipc_parse("QUIESCE extra\n",14,&cmd),"quiesce with arguments rejected");
        CHECK(a_pipc_parse("SELECT BBID 02:" "11:" "22:" "31\n", 24, &cmd)
              && cmd.kind==A_PIPC_SELECT_BBID && a_memcmp(cmd.bbid,A,4)==0, "SELECT BBID parsed");
        CHECK(a_pipc_parse("SELECT CELL 2\n", 14, &cmd)
              && cmd.kind==A_PIPC_SELECT_CELL && cmd.cell==2, "SELECT CELL parsed");
        CHECK(!a_pipc_parse("NONSENSE\n", 9, &cmd), "garbage command rejected");
        CHECK(!a_pipc_parse("SELECT BBID ZZ:..\n", 18, &cmd), "SELECT BBID bad bbid rejected");
    }
    /* ---- IPC status format (reports controller state) --------------- */
    {
        a_pctl c; a_pctl_init(&c, 0); c.enabled=1; c.active_n=2; c.cur_cell=1;
        a_memcpy(c.cur_target,B,4); c.has_target=1; c.have_last=1;
        c.last_outcome=A_SW_OUT_SWITCHED; c.last_phase=A_SW_VERIFIED; c.reason=A_PCTL_OK;
        char out[256]; int n=a_pipc_format_status(&c,out,sizeof out);
        CHECK(n>0, "status formatted");
        /* must contain the live fields */
        CHECK(a_strnlen(out,256)>0 && out[0]=='S', "status line starts STATUS");
        /* spot-check target hex present */
        int found=0; for(int i=0;i+10<=n;i++) if(out[i]=='0'&&out[i+1]=='6'&&out[i+2]==':') found=1;
        CHECK(found, "status carries synthetic target B");
        CHECK(strstr(out,"lifecycle=1 loaded=1")!=0,"status identifies loaded lifecycle");
        c.quiesced=1; a_pipc_format_status(&c,out,sizeof out);
        CHECK(strstr(out,"lifecycle=1 loaded=0")!=0,"status identifies established unload barrier");
    }

    /* ---- OSD control text ------------------------------------------ */
    {
        /* build an enabled controller with 2 active cells */
        a_pctl c; a_pctl_init(&c,0);
        au8 cfgb[2][4]; a_memcpy(cfgb[0],A,4); a_memcpy(cfgb[1],B,4);
        au8 t2[2][4]; a_memcpy(t2[0],A,4); a_memcpy(t2[1],B,4);
        a_pctl_set_config(&c, cfgb, 2, t2, 2);
        c.cur_cell=1; c.verified_cell=1; c.has_target=1;
        a_pcfg cfg; a_memset(&cfg,0,sizeof cfg); cfg.n=2; cfg.ok=1;
        a_memcpy(cfg.cells[0].bbid,A,4); cfg.cells[0].label[0]='A'; cfg.cells[0].label[1]=0;
        a_memcpy(cfg.cells[1].bbid,B,4); cfg.cells[1].label[0]='B'; cfg.cells[1].label[1]=0;
        char osd[512]; int n=a_posd_format(&c,&cfg,osd,sizeof osd);
        CHECK(n>0, "osd text produced");
        CHECK(osd[0]=='s'&&osd[5]=='1', "enabled => show 1");
        /* cell1 (B) should be the active one */
        int act=0; for(int i=0;i+9<=n;i++) if(osd[i]=='c'&&osd[i+5]=='1'&&osd[i+7]=='-'&&osd[i+9]=='B') act=1;
        CHECK(act, "current cell B marked active");

        /* Presentation changes preserve the live RF controller and RC input. */
        c.rc_enabled=1; c.rc_channel=12; c.cur_cell=1;
        a_pctl before=c;
        cfg.display_hidden=1;
        n=a_posd_format(&c,&cfg,osd,sizeof osd);
        CHECK(n==7 && osd[5]=='0', "hidden => only show 0");
        CHECK(a_memcmp(&before,&c,sizeof c)==0, "hiding preserves entire controller");
        CHECK_EQI(c.active_n,2,"hidden retains all RC choices");
        cfg.display_hidden=0;
        a_posd_format(&c,&cfg,osd,sizeof osd);
        CHECK(osd[5]=='1', "show again without changing RF state");
        CHECK(a_memcmp(&before,&c,sizeof c)==0, "showing preserves entire controller");
        c.quiesced=1;
        n=a_posd_format(&c,&cfg,osd,sizeof osd);
        CHECK(n==7 && osd[5]=='0',"unload barrier conceals output without changing saved visibility");
        CHECK(!cfg.display_hidden && c.enabled && c.active_n==2,"unload retains visibility preference and configured bank");

        /* disabled => hidden */
        a_pctl d; a_pctl_init(&d,0);
        au8 t1[1][4]; a_memcpy(t1[0],A,4);
        a_pctl_set_config(&d, cfgb, 2, t1, 1);   /* only 1 valid => disabled */
        n=a_posd_format(&d,&cfg,osd,sizeof osd);
        CHECK(osd[5]=='0', "disabled => show 0 (hidden)");
    }


    {
        a_pctl c;a_pctl_init(&c,0);c.enabled=1;c.cooldown_remaining_ms=12500;
        char out[1024];a_posd_format(&c,0,out,sizeof out);
        CHECK(strstr(out,"cooldown 13\n")!=0,"receiver OSD carries rounded-up advisory seconds");
        c.busy=1;a_posd_format(&c,0,out,sizeof out);
        CHECK(strstr(out,"cooldown 0\n")!=0,"ongoing switch does not display stale advisory");
        c.rc_protocol=2;c.rc_pwm_us=1833;a_pipc_format_status(&c,out,sizeof out);
        CHECK(strstr(out,"rc_proto=2 rc_us=1833 cooldown_ms=12500")!=0,"IPC exposes source, selected channel PWM and advisory");
    }
    return atest_report();
}
