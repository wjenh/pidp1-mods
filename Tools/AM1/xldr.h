#ifndef XLDR_H
#define XLDR_H
#include <stdint.h>

#define LDR_START_ADDR  07751

uint32_t xloader[] = {
0724074,  //       eem
0730002,  // loop, rpb
0760040,  //       lai
0027772,  //       and (177777
0247770,  //       dac addr
0642000,  //       spi
0617770,  //       jmp i addr	/done
0730002,  //       rpb
0327771,  //       dio end
0730002,  //       load, rpb
0337770,  //       dio i addr
0447770,  //       idx addr
0527771,  //       sas end
0607762,  //       jmp load
0607752,  //       jmp loop
0000000,  // addr, 0
0000000,  // end,  0
0177777   // constants
};

#endif
