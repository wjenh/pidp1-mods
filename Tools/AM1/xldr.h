#ifndef XLDR_H
#define XLDR_H
#include <stdint.h>

#define LDR_START_ADDR  07751

uint32_t xloader[] = {
0724074,  //       eem
0730002,  // loop, rpb
0760040,  //       lai
0027776,  //       and (177777
0247774,  //       dac addr
0642000,  //       spi
0607770,  //       jmp  done
0730002,  //       rpb
0327775,  //       dio end
0730002,  //       load, rpb
0337774,  //       dio i addr
0447774,  //       idx addr
0527775,  //       sas end
0607762,  //       jmp load
0607752,  //       jmp loop
0662001,  // done, ril 1s
0642000,  //       spi
0760400,  //       hlt
0617774,  //       jmp i addr
0000000,  // addr, 0
0000000,  // end,  0
0177777   // constants
};

#endif
