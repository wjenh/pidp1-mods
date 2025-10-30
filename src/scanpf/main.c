#include "common.h"
#include "panel_pidp1.h"
#include <pthread.h>

Panel*
getpanel(void)
{
	return attachseg("/tmp/pdp1_panel", sizeof(Panel));
}

int
main()
{
	Panel *p;

	p = getpanel();
	if(p == nil)
		return 0;
	return (p->sw2>>10)&077;
}
