#ifndef XLDR_H
#define XLDR_H
#include <stdint.h>

#define LDR_START_ADDR 07751

uint32_t xloader[] = {
0724074, //     eem
0730002, // loop, rpb
0327773, //     dio addr
0642000, //     spi
0607766, //     jmp done
0730002, //     rpb
0327774, //     dio end
0730002, // load, rpb
0337773, //     dio i addr
0447773, //     idx addr
0527774, //     sas end
0607760, //     jmp load
0607752, //     jmp loop
0662001, // done, ril 1s
0652000, //     spi i
0617773, //     jmp i addr	/ start prog
0760400, //     hlt		/ nostart, just halt
0607752, //     jmp loop	/ and go again
0000000, // addr, 0
0000000, // end, 0
};
#endif
