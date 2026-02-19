#include "common.h"
#include "panel_pidp1.h"
#include "pdp1.h"

void
updateswitches(PDP1P pdp1P, Panel *panel)
{
    int sw0 = panel->sw0;
    int sw1 = panel->sw1;
    int sw2 = panel->sw2;
    int sw3 = panel->sw3;

    pdp1P->extend_sw = !!(sw0 & SW_EXTEND);
    pdp1P->eta = sw0 & EXTMASK;
    pdp1P->ta = sw0 & ADDRMASK;
    pdp1P->tw = sw1;
    pdp1P->ss = (sw2 >> 10) & 077;

    pdp1P->power_sw = !!(sw0 & SW_POWER);
    pdp1P->single_cyc_sw = !!(sw2 & SW_SSTEP);
    pdp1P->single_inst_sw = !!(sw2 & SW_SINST);

    pdp1P->start_sw = !!(sw2 & (KEY_START | KEY_START_UP));
    pdp1P->sbm_start_sw = !!(sw2 & KEY_START_UP);
    pdp1P->stop_sw = !!(sw2 & KEY_STOP);
    pdp1P->continue_sw = !!(sw2 & KEY_CONT);
    pdp1P->examine_sw = !!(sw2 & KEY_EXAM);
    pdp1P->deposit_sw = !!(sw2 & KEY_DEP);
    pdp1P->readin_sw = !!(sw2 & KEY_READIN);

    pdp1P->tape_feed = !!(sw2 & KEY_FEED);

    pdp1P->spcwar1 = (sw3 >> 5) & 017;
    pdp1P->spcwar2 = (sw3 >> 9) & 017;
}

void
updatelights(PDP1P pdp1P, Panel *panel)
{
    int l5, l8, l9;
    l5 = 0;

    if(pdp1P->run)
    {
        l5 |= L5_RUN;
    }

    if(pdp1P->cyc)
    {
        l5 |= L5_CYC;
    }

    if(pdp1P->df1)
    {
        l5 |= L5_DF1;
    }

    if(pdp1P->hsc)
    {
        l5 |= L5_HSC;    // wje - enable the HS Cycle light
    }

    if(pdp1P->bc & 1)
    {
        l5 |= L5_BC1;
    }

    if(pdp1P->bc & 2)
    {
        l5 |= L5_BC2;
    }

    if(pdp1P->ov1)
    {
        l5 |= L5_OV1;
    }

    if(pdp1P->rim)
    {
        l5 |= L5_RIM;
    }

    if(pdp1P->sbm)
    {
        l5 |= L5_SBM;
    }

    if(pdp1P->exd)
    {
        l5 |= L5_EXD;
    }

    if(pdp1P->ioh)
    {
        l5 |= L5_IOH;
    }

    if(pdp1P->ioc)
    {
        l5 |= L5_IOC;
    }

    if(pdp1P->ios)
    {
        l5 |= L5_IOS;
    }

    l5 |= L5_PWR;

    if(pdp1P->single_cyc_sw)
    {
        l5 |= L5_SSTEP;
    }

    if(pdp1P->single_inst_sw)
    {
        l5 |= L5_SINST;
    }

    l8 = 0;

    if(pdp1P->rby)
    {
        l8 |= 0400000;
    }

    if(pdp1P->rcp)
    {
        l8 |= 0200000;
    }

    if(pdp1P->rc & 1)
    {
        l8 |= 0100000;
    }

    if(pdp1P->rc & 2)
    {
        l8 |= 0040000;
    }

    if(pdp1P->rcl)
    {
        l8 |= 0020000;
    }

    if(pdp1P->r)
    {
        l8 |= 0010000;
    }

    if(pdp1P->rs)
    {
        l8 |= 0004000;
    }

    if(pdp1P->w)
    {
        l8 |= 0002000;
    }

    if(pdp1P->i)
    {
        l8 |= 0001000;
    }

    l8 |= pdp1P->pb << 1;

    l9 = 0;

    if(pdp1P->tbs)
    {
        l9 |= 0400000;
    }

    // schematics says TBB set means red
    // but manual says lamp on means black, weird
    if(!pdp1P->tbb)
    {
        l9 |= 0200000;
    }

    if(pdp1P->tyo)
    {
        l9 |= 0100000;
    }

    if(pdp1P->tcp)
    {
        l9 |= 0040000;
    }

    l9 |= pdp1P->tb << 8;

    if(pdp1P->punon)
    {
        l9 |= 0000200;
    }

    if(pdp1P->pcp)
    {
        l9 |= 0000100;
    }

    if(pdp1P->df2)
    {
        l9 |= 0000040;
    }

    if(pdp1P->ov2)
    {
        l9 |= 0000020;
    }

    panel->lights0 = pdp1P->epc | PC;
    panel->lights1 = pdp1P->ema | MA;
    panel->lights2 = MB;
    panel->lights3 = AC;
    panel->lights4 = IO;
    panel->lights5 = l5;
    panel->lights6 = pdp1P->ir << 13 | pdp1P->ss << 6 | pdp1P->pf;
    panel->lights7 = pdp1P->rb;
    panel->lights8 = l8;
    panel->lights9 = l9;
}

void
lightsoff(Panel *panel)
{
    panel->lights0 = 0;
    panel->lights1 = 0;
    panel->lights2 = 0;
    panel->lights3 = 0;
    panel->lights4 = 0;
    panel->lights5 = 0;
    panel->lights6 = 0;
    panel->lights7 = 0;
    panel->lights8 = 0;
    panel->lights9 = 0;
}

void
lightson(Panel *panel)
{
    panel->lights0 = 0777777;
    panel->lights1 = 0777777;
    panel->lights2 = 0777777;
    panel->lights3 = 0777777;
    panel->lights4 = 0777777;
    panel->lights5 = 0777777;
    panel->lights6 = 0777777;
    panel->lights7 = 0777777;
    panel->lights8 = 0777777;
    panel->lights9 = 0777777;
}

Panel*
getpanel(void)
{
    return attachseg("/tmp/pdp1_panel", sizeof(Panel));
}
