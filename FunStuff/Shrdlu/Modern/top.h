// The top level of one typed line, over the Lisp run-time.

#ifndef TOP_H
#define TOP_H

#include "rt.h"

// Sets up the variables a sentence starts with. seed is the generator's.
void topInit(unsigned seed);
// Reads, parses and answers a sentence whose words the Lisp reader takes from
// the typed text. Says how it ended, a STOP_ value.
int topSentenceRead(void);
// A sentence whose words are read already, in order, with its punctuation.
int topSentence(V words, V punct);
// A goal, /DO's. okP says whether it succeeded. planFn, if there is one, is given
// the plan, and only if run is set is it carried out.
int topGoal(V goal, int *okP, int run, void (*planFn)(V plan));
// What the last sentence's parse and answer took, in microseconds.
long topParseMicros(void);
long topAnswerMicros(void);
// What the last goal's planning took, and all of it, in microseconds.
long topPlanMicros(void);
long topAllMicros(void);
// The clock those are read from.
long topNowMicros(void);
// The generator's state.
void topSeed(unsigned seed);
unsigned topSeedNow(void);

#endif
