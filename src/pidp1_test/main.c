#include "common.h"
#include "panel_pidp1.h"
#include <pthread.h>

// Row and bit column of the one lamp main()'s sweep currently has lit,
// read by pwmthread(). Not locked: a stale read tallies last position for
// one 5us tick after main() moves on, which is invisible at the sweep's
// 100ms step.
static int curRow;
static int curCol;

Panel*
getpanel(void)
{
	return attachseg("/tmp/pdp1_panel", sizeof(Panel));
}

void*
swthread(void *arg)
{
	volatile Panel *p = (volatile Panel*)arg;
	for(;;) {
		printf("\r%06o %06o %06o %06o",
			p->sw0,
			p->sw1,
			p->sw2,
			p->sw3);
		fflush(stdout);
		nsleep(10000000);
	}
	return nil;
}

// newpanel no longer samples lights0-lights9 for output (14-Jun-26 pwmcount[][]
// rework): it derives lamp brightness from pwmcount[][], scaled against
// cyclecount, both of which only the real emulator's updatelights()/
// updatelights_pwm() advance (panel1.c). This program runs no emulator, so it
// stands in for that tally itself, at a nominal 5us "cycle" matching the real
// one, keeping the lamp at curRow/curCol lit at full brightness.
void*
pwmthread(void *arg)
{
	Panel *p = (Panel*)arg;
	for(;;) {
		p->pwmcount[curRow][curCol]++;
		p->cyclecount++;
		nsleep(5000);
	}
	return nil;
}

int
main()
{
	Panel *p;
	int *lp;
	int i;
	int col;
	pthread_t th;

	p = getpanel();
	if(p == nil)
		return 1;
	p->lights0 = 0;
	p->lights1 = 0;
	p->lights2 = 0;
	p->lights3 = 0;
	p->lights4 = 0;
	p->lights5 = 0;
	p->lights6 = 0;
	p->lights7 = 0;
	p->lights8 = 0;
	p->lights9 = 0;
	pthread_create(&th, nil, swthread, p);
	pthread_create(&th, nil, pwmthread, p);

	lp = &p->lights0;
	*lp = 1;
	i = 0;
	col = 0;
	curRow = i;
	curCol = col;
	for(;;) {
		lp[i] <<= 1;
		col++;
		if(lp[i] & 01000000) {
			lp[i] = 0;
			i = (i+1)%10;
			lp[i] = 1;
			col = 0;
		}
		curRow = i;
		curCol = col;
		nsleep(100000000);
	}
	return 0;
}
