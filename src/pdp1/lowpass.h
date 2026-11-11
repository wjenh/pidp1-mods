/*
 * Structures and definitions for the lowpass filter routines.
*/

#include <stdbool.h>
#include <stdint.h>

// The bits in the program flags
#define PF_1    0x20
#define PF_2    0x10
#define PF_3    0x08
#define PF_4    0x04
#define PF_5    0x02
#define PF_6    0x01

// Yes, all those parens are important; remember that defines are just text substitutions.
#define getProgFlag(mask, word) (((mask) & (word))?1:0)

// sum 2 samples, multiply by the scale factor
#define mixSamples(s1, s2, scale) (((s1) + (s2)) * (scale))

// One RC low-pass, run in continuous time: the capacitor is brought exactly to each moment asked
// for, so an input change lands at its own time, not at the next sample.
typedef struct {
    double invTauNs;        // 1 / (R*C), per nanosecond
    double y;               // the output, in units of the input's swing
    double x;               // the level the input drives now
    uint64_t t;             // the simtime y was last brought to
    uint64_t cacheDt[2];    // the last two steps and their decay factors: the steps between
    double cacheK[2];       // samples take only two lengths, so exp() is mostly skipped
    int cacheNext;
    } RCFilter, *RCFilterP;

void rcInit(RCFilterP, double level, uint64_t t);
void rcSetCutoff(RCFilterP, double hz);
void rcAdvance(RCFilterP, uint64_t t);
void rcSetInput(RCFilterP, uint64_t t, double level);
