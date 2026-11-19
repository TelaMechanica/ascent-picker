/* test_cells.c — Picker RC/PWM cell mapping + activation validity. Links the PRODUCTION
 * module. Covers the addendum's required static-test matrix (§12). No hardware. */
#include "atest.h"
#include "../src/ascent_cells.h"

static a_cell_cfg mk(int valid){ a_cell_cfg c; c.bbid[0]=1;c.bbid[1]=2;c.bbid[2]=3;c.bbid[3]=(au8)valid; c.valid=valid; return c; }

int main(void)
{
    SUITE("Picker cell mapping + activation validity");
    int idx[A_CELLS_MAX_CFG];

    /* ---- ACTIVATION matrix (§12) ------------------------------------ */
    { a_cell_cfg c[1]; (void)c; /* 0 configured */
      CHECK_EQI(a_cells_active_valid(0,0,idx,A_CELLS_MAX_CFG), 0, "0 configured => N=0");
      CHECK(!a_cells_picker_active(0), "0 => disabled"); }
    { a_cell_cfg c[1]={mk(1)};
      int N=a_cells_active_valid(c,1,idx,A_CELLS_MAX_CFG);
      CHECK_EQI(N,1,"1 configured/valid => N=1"); CHECK(!a_cells_picker_active(N),"1 => disabled"); }
    { a_cell_cfg c[2]={mk(0),mk(0)};
      int N=a_cells_active_valid(c,2,idx,A_CELLS_MAX_CFG);
      CHECK_EQI(N,0,"2 cfg/0 valid => N=0"); CHECK(!a_cells_picker_active(N),"disabled"); }
    { a_cell_cfg c[2]={mk(1),mk(0)};
      int N=a_cells_active_valid(c,2,idx,A_CELLS_MAX_CFG);
      CHECK_EQI(N,1,"2 cfg/1 valid => N=1"); CHECK(!a_cells_picker_active(N),"disabled"); }
    { a_cell_cfg c[2]={mk(1),mk(1)};
      int N=a_cells_active_valid(c,2,idx,A_CELLS_MAX_CFG);
      CHECK_EQI(N,2,"2 cfg/2 valid => N=2"); CHECK(a_cells_picker_active(N),"enabled"); }
    { a_cell_cfg c[4]={mk(1),mk(0),mk(0),mk(0)};
      int N=a_cells_active_valid(c,4,idx,A_CELLS_MAX_CFG);
      CHECK_EQI(N,1,"4 cfg/1 valid => N=1"); CHECK(!a_cells_picker_active(N),"disabled"); }
    { a_cell_cfg c[4]={mk(1),mk(0),mk(1),mk(0)};
      int N=a_cells_active_valid(c,4,idx,A_CELLS_MAX_CFG);
      CHECK_EQI(N,2,"4 cfg/2 valid => N=2 (enabled)"); CHECK(a_cells_picker_active(N),"enabled");
      CHECK(idx[0]==0 && idx[1]==2, "active-valid keeps configured order (0,2)"); }
    { a_cell_cfg c[4]={mk(1),mk(1),mk(0),mk(1)};
      int N=a_cells_active_valid(c,4,idx,A_CELLS_MAX_CFG);
      CHECK_EQI(N,3,"4 cfg/3 valid => N=3"); CHECK(idx[2]==3,"third valid is index 3"); }
    { a_cell_cfg c[4]={mk(1),mk(1),mk(1),mk(1)};
      CHECK_EQI(a_cells_active_valid(c,4,idx,A_CELLS_MAX_CFG),4,"4 cfg/4 valid => N=4"); }

    /* ---- PWM CENTERS (§12) ------------------------------------------ */
    CHECK_EQI(a_cells_center_us(1,2),1250,"N=2 c1"); CHECK_EQI(a_cells_center_us(2,2),1750,"N=2 c2");
    CHECK_EQI(a_cells_center_us(1,3),1167,"N=3 c1"); CHECK_EQI(a_cells_center_us(2,3),1500,"N=3 c2");
    CHECK_EQI(a_cells_center_us(3,3),1833,"N=3 c3");
    CHECK_EQI(a_cells_center_us(1,4),1125,"N=4 c1"); CHECK_EQI(a_cells_center_us(2,4),1375,"N=4 c2");
    CHECK_EQI(a_cells_center_us(3,4),1625,"N=4 c3"); CHECK_EQI(a_cells_center_us(4,4),1875,"N=4 c4");
    CHECK_EQI(a_cells_center_us(1,5),1100,"N=5 c1"); CHECK_EQI(a_cells_center_us(3,5),1500,"N=5 c3");
    CHECK_EQI(a_cells_center_us(5,5),1900,"N=5 c5");

    /* ---- STARTUP: classify by normal boundary, no prior state ------- */
    CHECK_EQI(a_cells_classify(1250,2,A_CELL_INVALID),0,"N=2 1250 => cell0 (startup)");
    CHECK_EQI(a_cells_classify(1750,2,A_CELL_INVALID),1,"N=2 1750 => cell1 (startup)");
    CHECK_EQI(a_cells_classify(1500,2,A_CELL_INVALID),1,"N=2 exact boundary 1500 => cell1");
    CHECK_EQI(a_cells_classify(1000,3,A_CELL_INVALID),0,"N=3 1000 => cell0");
    CHECK_EQI(a_cells_classify(2000,3,A_CELL_INVALID),2,"N=3 2000 => cell2 (top inclusive)");
    CHECK_EQI(a_cells_classify(1334,3,A_CELL_INVALID),1,"N=3 1334 => cell1 (past 1333.3)");
    CHECK_EQI(a_cells_classify(1333,3,A_CELL_INVALID),0,"N=3 1333 => cell0 (below boundary)");

    /* ---- BOUNDARY HYSTERESIS (§2, §12), N=2 boundary 1500 ----------- */
    /* in cell0: hold until pwm EXCEEDS 1510 */
    CHECK_EQI(a_cells_classify(1500,2,0),0,"cell0 holds at exact boundary 1500");
    CHECK_EQI(a_cells_classify(1510,2,0),0,"cell0 holds at boundary+10 (1510)");
    CHECK_EQI(a_cells_classify(1511,2,0),1,"cell0 -> cell1 just past hyst (1511)");
    CHECK_EQI(a_cells_classify(1490,2,0),0,"cell0 stays in cell0 at 1490");
    /* in cell1: hold until pwm FALLS BELOW 1490 */
    CHECK_EQI(a_cells_classify(1500,2,1),1,"cell1 holds at exact boundary 1500");
    CHECK_EQI(a_cells_classify(1490,2,1),1,"cell1 holds at boundary-10 (1490)");
    CHECK_EQI(a_cells_classify(1489,2,1),0,"cell1 -> cell0 just past hyst (1489)");
    CHECK_EQI(a_cells_classify(1510,2,1),1,"cell1 stays in cell1 at 1510");
    /* sustained jitter around the boundary never flips while within the band */
    { int cur=0, flips=0, prev=0;
      int jit[8]={1495,1505,1498,1502,1507,1493,1500,1509};
      for (int k=0;k<8;k++){ cur=a_cells_classify(jit[k],2,cur); if(k>0&&cur!=prev) flips++; prev=cur; }
      CHECK_EQI(flips,0,"sustained jitter within +/-10 never switches"); }
    /* both directions across a 3-cell inner boundary (~1667) */
    CHECK_EQI(a_cells_classify(1680,3,1),2,"N=3 cell1 -> cell2 past upper+hyst");
    CHECK_EQI(a_cells_classify(1670,3,1),1,"N=3 cell1 holds within hyst of 1667");

    /* ---- OUT-OF-RANGE / invalid sample: hold, no switch ------------- */
    CHECK_EQI(a_cells_classify(800,2,0),A_CELL_INVALID,"800us out of range => invalid");
    CHECK_EQI(a_cells_classify(2200,2,1),A_CELL_INVALID,"2200us out of range => invalid");
    CHECK_EQI(a_cells_classify(950,2,A_CELL_INVALID),0,"950us clamps into cell0 (mild low overshoot)");
    CHECK_EQI(a_cells_classify(2050,2,A_CELL_INVALID),1,"2050us clamps into top cell");

    /* ---- display boundaries sane ----------------------------------- */
    CHECK_EQI(a_cells_lower_us(1,3),1000,"N=3 cell1 lower");
    CHECK_EQI(a_cells_upper_us(3,3),2000,"N=3 cell3 upper");

    return atest_report();
}
