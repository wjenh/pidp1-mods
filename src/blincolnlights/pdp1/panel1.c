/*
 * Translates emulator state (PDP1) to/from the shared front-panel memory
 * segment (Panel): updateswitches() reads switch positions set by the panel
 * driver, updatelights() writes the indicator-light bits read by the panel
 * driver(s).
 *
 * wje 14-Jun-26 - in updatelights(), also tally each lamp bit into
 *                 panel->pwmcount[][] (see panel_pidp1.h) for the new
 *                 lower-overhead lamp PWM scheme; lights0-lights9 unchanged
 */
#include "common.h"
#include "panel_pidp1.h"
#include "pdp1.h"

// Add 1 to cnt[i] for each bit i (0-17) of bits that is set, saturating at
// 255 instead of wrapping. Used by updatelights() to tally per-cycle "on"
// counts into panel->pwmcount[][]. Saturation guards against the panel
// driver's read+reset interval occasionally running longer than expected
// (255 cycles = ~1.275ms) without wrapping pwmcount back to a small value.
static void
incrcount(u8 *cnt, u32 bits)
{
	for(int i = 0; i < 18; i++)
	{
		if((bits >> i) & 1)
		{
			if(cnt[i] < 255)
			{
				cnt[i]++;
			}
		}
	}
}

void
updateswitches(PDP1 *pdp, Panel *panel)
{
	int sw0 = panel->sw0;
	int sw1 = panel->sw1;
	int sw2 = panel->sw2;
	int sw3 = panel->sw3;

	pdp->extend_sw = !!(sw0 & SW_EXTEND);
	pdp->eta = sw0 & EXTMASK;
	pdp->ta = sw0 & ADDRMASK;
	pdp->tw = sw1;
	pdp->ss = (sw2>>10) & 077;

	pdp->power_sw = !!(sw0 & SW_POWER);
	pdp->single_cyc_sw = !!(sw2 & SW_SSTEP);
	pdp->single_inst_sw = !!(sw2 & SW_SINST);

	pdp->start_sw = !!(sw2 & (KEY_START|KEY_START_UP));
	pdp->sbm_start_sw = !!(sw2 & KEY_START_UP);
	pdp->stop_sw = !!(sw2 & KEY_STOP);
	pdp->continue_sw = !!(sw2 & KEY_CONT);
	pdp->examine_sw = !!(sw2 & KEY_EXAM);
	pdp->deposit_sw = !!(sw2 & KEY_DEP);
	pdp->readin_sw = !!(sw2 & KEY_READIN);

	pdp->tape_feed = !!(sw2 & KEY_FEED);

	pdp->spcwar1 = (sw3>>5) & 017;
	pdp->spcwar2 = (sw3>>9) & 017;
}

void
updatelights(PDP1 *pdp, Panel *panel)
{
	int l5, l8, l9;
	l5 = 0;
	if(pdp->run) l5 |= L5_RUN;
	if(pdp->cyc) l5 |= L5_CYC;
	if(pdp->df1) l5 |= L5_DF1;
	if(pdp->bc&1) l5 |= L5_BC1;
	if(pdp->bc&2) l5 |= L5_BC2;
	if(pdp->ov1) l5 |= L5_OV1;
	if(pdp->rim) l5 |= L5_RIM;
	if(pdp->sbm) l5 |= L5_SBM;
	if(pdp->exd) l5 |= L5_EXD;
	if(pdp->ioh) l5 |= L5_IOH;
	if(pdp->ioc) l5 |= L5_IOC;
	if(pdp->ios) l5 |= L5_IOS;
	if(pdp->hsc) l5 |= L5_HSC;
	l5 |= L5_PWR;
	if(pdp->single_cyc_sw) l5 |= L5_SSTEP;
	if(pdp->single_inst_sw) l5 |= L5_SINST;

	l8 = 0;
	if(pdp->rby) l8 |= 0400000;
	if(pdp->rcp) l8 |= 0200000;
	if(pdp->rc&1) l8 |= 0100000;
	if(pdp->rc&2) l8 |= 0040000;
	if(pdp->rcl) l8 |= 0020000;
	if(pdp->r) l8 |= 0010000;
	if(pdp->rs) l8 |= 0004000;
	if(pdp->w) l8 |= 0002000;
	if(pdp->i) l8 |= 0001000;
	l8 |= pdp->pb<<1;

	l9 = 0;
	if(pdp->tbs) l9 |= 0400000;
	// schematics says TBB set means red
	// but manual says lamp on means black, weird
	if(!pdp->tbb) l9 |= 0200000;
	if(pdp->tyo) l9 |= 0100000;
	if(pdp->tcp) l9 |= 0040000;
	l9 |= pdp->tb << 8;
	if(pdp->punon) l9 |= 0000200;
	if(pdp->pcp) l9 |= 0000100;
	if(pdp->df2) l9 |= 0000040;
	if(pdp->ov2) l9 |= 0000020;

	panel->lights0 = pdp->epc | PC;
	panel->lights1 = pdp->ema | MA;
	panel->lights2 = MB;
	panel->lights3 = AC;
	panel->lights4 = IO;
	panel->lights5 = l5;
	panel->lights6 = pdp->ir<<13 | pdp->ss<<6 | pdp->pf;
	panel->lights7 = pdp->rb;
	panel->lights8 = l8;
	panel->lights9 = l9;

	// Tally this cycle's lamp snapshot into pwmcount[][] for the panel
	// driver's PWM scheme (see panel_pidp1.h). Uses the same
	// lights0-lights9 values just computed above.
	incrcount(panel->pwmcount[0], panel->lights0);
	incrcount(panel->pwmcount[1], panel->lights1);
	incrcount(panel->pwmcount[2], panel->lights2);
	incrcount(panel->pwmcount[3], panel->lights3);
	incrcount(panel->pwmcount[4], panel->lights4);
	incrcount(panel->pwmcount[5], panel->lights5);
	incrcount(panel->pwmcount[6], panel->lights6);
	incrcount(panel->pwmcount[7], panel->lights7);
	incrcount(panel->pwmcount[8], panel->lights8);
	incrcount(panel->pwmcount[9], panel->lights9);
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
