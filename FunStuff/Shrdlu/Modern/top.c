// The top level of one typed line: what SHRDLU's loop does around the parse and
// the answer (syscom.180:19-74), and the goal a /DO types. Each runs under a top
// frame, where an abandoned sentence lands.

#include <time.h>

#include "top.h"

#define LFSR_MASK 0377777u
#define LFSR_TAP 0220000u
#define LFSR_STEPS 17

static unsigned lfsrState;
static long parseMicros;
static long answerMicros;
static long planMicros;
static long allMicros;

static V keepC;

// Microseconds on a clock that does not stop.
static long
nowMicros(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((long)ts.tv_sec * 1000000L) + (ts.tv_nsec / 1000L);
}

static Symbol *
sym(const char *nameP)
{
    return rtIntern(nameP);
}

static V
symv(const char *nameP)
{
    return SYMV(rtIntern(nameP));
}

static V
list1(V a)
{
    return rtCons(a, NIL);
}

static V
list2(V a, V b)
{
    return rtCons(a, list1(b));
}

static V
list3(V a, V b, V c)
{
    return rtCons(a, list2(b, c));
}

unsigned
topSeedNow(void)
{
    return lfsrState;
}

void
topSeed(unsigned seed)
{
    lfsrState = seed & LFSR_MASK;
    if(!lfsrState)
    {
        lfsrState = 1;
    }
}

// The program's generator, a 17-bit Galois shift register stepped once per bit.
V
hostRandom(void)
{
    int i;
    unsigned lsb;

    for(i = 0; i < LFSR_STEPS; i++)
    {
        lsb = lfsrState & 1u;
        lfsrState >>= 1;
        if(lsb)
        {
            lfsrState ^= LFSR_TAP;
        }
    }
    return FIX((intptr_t)lfsrState);
}

void
topInit(unsigned seed)
{
    topSeed(seed);
    rtAddRoot(&keepC);
}

// What a sentence's start clears (syscom.180:41-60).
static void
sentenceStart(void)
{
    static const char *const zeroP[] =
    {
        "PLNR-TIME", "ANS-PLNR-TIME", "SMN-TIME", "ERT-TIME", "SH-GCTIME", NULL
    };
    static const char *const pronounP[] = {"IT", "THEY", "ONE", NULL};
    V args[2];
    int i;

    sym("SENTNO")->value = FIX(FIXV(sym("SENTNO")->value) + 1);
    sym("PARSINGS")->value = FIX(0);
    sym("LEVEL")->value = FIX(0);
    sym("LASTSENTNO")->value = FIX(FIXV(sym("LASTSENTNO")->value) + 1);
    sym("LASTSENT")->value = sym("C")->value;
    sym("GLOBAL-MESSAGE")->value = NIL;
    sym("MES")->value = symv("NOPE");
    sym("BACKREF")->value = NIL;
    for(i = 0; zeroP[i]; i++)
    {
        sym(zeroP[i])->value = FIX(0);
    }
    sym("RUNTIME")->value = FIX(nowMicros());
    for(i = 0; pronounP[i]; i++)
    {
        args[0] = symv(pronounP[i]);
        args[1] = symv("BIND");
        rtApply(symv("REMPROP"), 2, args);
    }
    rtSentenceStart();
}

// GLOBAL-MESSAGE, or I DON'T UNDERSTAND.
static void
sayFailure(void)
{
    V msg;

    msg = sym("GLOBAL-MESSAGE")->value;
    if(msg == NIL)
    {
        msg = list3(symv("I"), symv("DON'T"), symv("UNDERSTAND."));
    }
    rtApplyList(symv("SAY"), msg);
}

// The parse and the answer of a sentence read.
static void
parseAndAnswer(V sent)
{
    V c;
    V arg;
    long t0;
    long idle0;

    t0 = nowMicros();
    arg = sym("PARSEARGS")->value;
    c = rtApply(symv("PARSEVAL"), 1, &arg);
    keepC = c;
    sym("C")->value = c;
    sym("PT")->value = c;
    if(c == NIL)
    {
        parseMicros = nowMicros() - t0;
        sayFailure();
        return;
    }
    sym("FE")->value = rtApply(symv("FE"), 1, &c);
    sym("NB")->value = sent;
    sym("H")->value = rtApply(symv("H"), 1, &c);
    sym("INTERPRETATION")->value = rtApply(symv("SM"), 1, &c);
    parseMicros = nowMicros() - t0;
    t0 = nowMicros();
    idle0 = hostIdleMicros();
    arg = list2(symv("ANSWER"), symv("C"));
    if(rtApply(symv("TIME-ANSWER"), 1, &arg) == NIL)
    {
        sayFailure();
    }
    answerMicros = (nowMicros() - t0) - (hostIdleMicros() - idle0);
}

static void
sentenceBody(void)
{
    V sent;

    sentenceStart();
    sent = rtEval(list1(symv("ETAOIN")));
    sym("SENT")->value = sent;
    sym("N")->value = sent;
    parseAndAnswer(sent);
}

// A sentence's start, its words and its parse, for words already read.
static void
sentenceGiven(V words, V punct)
{
    sentenceStart();
    sym("SENT")->value = words;
    sym("N")->value = words;
    sym("PUNCT")->value = punct;
    parseAndAnswer(words);
}

int
topSentence(V words, V punct)
{
    Frame frame;
    int r;

    parseMicros = 0;
    answerMicros = 0;
    rtFrameEnter(&frame, FRAME_TOP);
    r = setjmp(frame.jump);
    if(r == 0)
    {
        sentenceGiven(words, punct);
    }
    rtFramePop(&frame);
    return r;
}

int
topSentenceRead(void)
{
    Frame frame;
    int r;

    parseMicros = 0;
    answerMicros = 0;
    rtFrameEnter(&frame, FRAME_TOP);
    r = setjmp(frame.jump);
    if(r == 0)
    {
        sentenceBody();
    }
    rtFramePop(&frame);
    return r;
}

static int
goalBody(V goal, int run, void (*planFn)(V plan))
{
    V thval;
    V ok;
    long t0;

    t0 = nowMicros();
    rtSentenceStart();
    sym("PLAN")->value = NIL;
    thval = list3(symv("THVAL"),
                  list2(symv("QUOTE"),
                        list3(symv("THGOAL"), goal,
                              list2(symv("THTBF"), symv("THTRUE")))),
                  list2(symv("QUOTE"), list1(list2(symv("EV"), symv("COMMAND")))));
    ok = rtEval(thval);
    planMicros = nowMicros() - t0;
    allMicros = planMicros;
    if(ok == NIL)
    {
        return 0;
    }
    thval = rtEval(list2(symv("REVERSE"), symv("PLAN")));
    if(planFn)
    {
        planFn(thval);
    }
    if(run)
    {
        t0 = hostIdleMicros();
        rtApply(symv("EVLIS"), 1, &thval);
        allMicros = planMicros + (hostIdleMicros() - t0);
    }
    return 1;
}

int
topGoal(V goal, int *okP, int run, void (*planFn)(V plan))
{
    Frame frame;
    int r;

    *okP = 0;
    rtFrameEnter(&frame, FRAME_TOP);
    r = setjmp(frame.jump);
    if(r == 0)
    {
        *okP = goalBody(goal, run, planFn);
    }
    rtFramePop(&frame);
    return r;
}

long
topNowMicros(void)
{
    return nowMicros();
}

long
topPlanMicros(void)
{
    return planMicros;
}

long
topAllMicros(void)
{
    return allMicros;
}

long
topParseMicros(void)
{
    return parseMicros;
}

long
topAnswerMicros(void)
{
    return answerMicros;
}
