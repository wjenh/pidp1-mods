#ifndef PANEL1_H
#define PANEL1_H
// The emulator's side of the front-panel segment (panel1.c): mapping it, reading the switches,
// and the lamp tests. updatelights() and updatelights_pwm(), which the CPU calls, are in pdp1.h.

#include "pdp1.h"

void updateswitches(PDP1 *pdp, Panel *panel);
void lightsoff(Panel *panel);
void lightson(Panel *panel);
Panel *getpanel(void);
#endif
