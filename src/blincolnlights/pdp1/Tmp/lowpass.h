/*
 * Structures and definitions for the lowpass filter routines.
*/

#include <stdbool.h>

// The bits in the program flags
#define PF_1    0x20
#define PF_2    0x10
#define PF_3    0x08
#define PF_4    0x04
#define PF_5    0x02
#define PF_6    0x01

#define boundValue(v) (((v) < 0.0)?0.0:(((v) > 1.0)?1.0:(v)))

// Yes, all those parens are important; remember that defines are just text substitutions.
#define getProgFlag(mask, word) (((mask) & (word))?1:0)

// sum 2 samples, multiply by the scale factor
#define mixSamples(s1, s2, scale) (((s1) + (s2)) * (scale))

// How we control the filters
typedef struct {
    float alpha;        // the alpha used to tune filter response, 0.0 <= alpha <= 1.0, lower is lower cutoff freq
    float lastResult;   // the previous result, needed for all filtering
    float gain;         // scale factor to apply to the computed filtered sample
    float initialValue; // value to initialize filter's lastResult to on the first filter call
    float highValue;    // threshold for dead elimination
    int highLimit;      // consecutive cycle count that is the threshold for dead elimnation
    int highCount;      // number of consecutive samples that meet or exceet maxHigh in value
    bool sawOne;        // so we know we had one prior sample
    bool doGain;        // whether or not we apply the gain factor, gain of 9.0 means no
    bool deadElimination;   // do stuck high value elimination
    } FilterSpec, *FilterSpecP;

FilterSpecP initializeFilter(FilterSpecP, float, float, float);   // specP alpha, scale, initial value
void setFilterDeadDetection(FilterSpecP, float, int);
float lowPassFilter(FilterSpecP, float);
