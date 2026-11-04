// The terminal: one telnet client at a time on the program's port, and what it
// types. Nothing echoes but this program, so the reader shows each character as
// it takes it. A line is English unless it starts with /. English is read a
// character at a time, into words as they end: a word the program cannot read
// stops the sentence where it is, and after a Return the user goes on from there.
// Each reply ends with a line holding only *.
//
// The commands after / read the Lisp world's own state and print it in the
// machine's forms, numbers in octal.

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rt.h"
#include "scene.h"
#include "term.h"
#include "top.h"

#define OUT_MAX 4096
#define IN_MAX 4096
#define WORD_MAX 120
#define LINE_MAX_CHARS 1024
#define TOKEN_MAX 40
#define LIST_MAX 8
#define FIND_ARGS 4
#define MICROS_PER_MILLI 1000L
// The output's wrapping width: the Lisp's LINEL, and the WORDS: echo's. A word ends at
// the width less one characters at the most, so with a closing quote and period after
// it a line is OUT_WIDTH+1 characters, 79, at the most (TASK-LINE-WRAP).
#ifndef OUT_WIDTH
#define OUT_WIDTH 78
#endif
// More numbers than any sentence has questions: a question that will not take its
// answer stops the sentence, or there would be no way out of it.
#define ANSWERS_MAX 4

#define TELNET_IAC 255
#define TELNET_SB 250
#define TELNET_SE 240
#define TELNET_WILL 251
#define TELNET_DONT 254

#define MODE_SENTENCE 0
#define MODE_COMMAND 1
#define MODE_WAIT 2

#define CHAR_CTRL_U 025
#define CHAR_BS 010
#define CHAR_DEL 0177

#define WORD_APOSTROPHE '"'

// What a command line's carrying out says beside 0, bad, and 1, done.
#define LINE_WORD_BAD 2

// What a telnet client is told on connecting: this end echoes, and characters
// come one at a time.
static const unsigned char greeting[] =
{
    0xFF, 0xFB, 0x01, 0xFF, 0xFB, 0x03, 0xFF, 0xFD, 0x03, 0xFF, 0xFC, 0x22, 0xFF, 0xFE, 0x22
};

static const char banner[] =
    "SHRDLU: TYPE A SENTENCE, OR A COMMAND AFTER /: /WORD X, /WORD X FEATURES,\n"
    "/TREE ON, /TREE OFF, /SM, /PLAN ON, /PLAN OFF, /ROOM; /DO GOAL, /TRY GOAL,\n"
    "/TRACE ON, /TRACE OFF, /EVENTS, /HISTORY X, /FIND PRED ARGS, /SEED N, /TIMES;\n"
    "/MOVETO X Y Z, /GRASP :BN, /UNGRASP, /REFRESH, /SCENE, /STATUS, /RESET\n";

typedef struct GoalForm
{
    const char *nameP;
    int nArgs;              // -1: the rest of the line, as one list
} GoalForm;

static const GoalForm goalForms[] =
{
    {"PICKUP", 1}, {"GRASP", 1}, {"UNGRASP", 0}, {"RAISEHAND", 0}, {"PUTON", 2},
    {"PUTIN", 2}, {"STACKUP", -1}, {"CLEARTOP", 1}, {"GET-RID-OF", 1}, {"NOTICE", 1},
    {NULL, 0}
};

static int clientFd = -1;
static unsigned char outBuf[OUT_MAX];
static size_t outLen;
static unsigned char inBuf[IN_MAX];
static size_t inLen;
static size_t inAt;
static int telnetState;
static int atLineStart = 1;
static int termCol;         // the characters sent on this line
static long readMicros;
static int answers;
static int lispMuted;       // what a goal's planning prints is its own, not the user's

// The reader.
static int mode;
static char word[WORD_MAX + 1];
static int wordLen;
static int quoteOpen;
static int newWord;
static int punct;
static V typed;             // the sentence so far, newest word first
static int typedLen;
static char line[LINE_MAX_CHARS + 1];
static int lineLen;

// The switches.
static int treeOn;
static int planOn;
static int planPrinted;

// What the last sentence built, newest first, and left of it.
static V built;
static V keep;
static V lastTree;

// The line being read as a command.
static const char *linePosP;
static char token[TOKEN_MAX + 1];
static V knownFeatures;

static void
flushOut(void)
{
    size_t off;
    ssize_t r;

    off = 0;
    while((off < outLen) && (clientFd >= 0))
    {
        r = send(clientFd, outBuf + off, outLen - off, MSG_NOSIGNAL);
        if(r <= 0)
        {
            close(clientFd);
            clientFd = -1;
        }
        else
        {
            off += (size_t)r;
        }
    }
    outLen = 0;
}

// One character to the client, a line's end as telnet has it; output with no one
// connected is discarded.
static void
termChar(int c)
{
    if(c == '\n')
    {
        termChar('\r');
    }
    if(outLen >= OUT_MAX)
    {
        flushOut();
    }
    outBuf[outLen] = (unsigned char)c;
    outLen++;
    atLineStart = (c == '\n');
    if(atLineStart)
    {
        termCol = 0;
        rtOutLineStart();
    }
    else if(c != '\r')
    {
        termCol++;
    }
}

void
termText(const char *textP)
{
    while(*textP)
    {
        termChar((unsigned char)*textP);
        textP++;
    }
}

void
hostOut(const char *textP, size_t n)
{
    size_t k;

    if(lispMuted)
    {
        return;
    }
    for(k = 0; k < n; k++)
    {
        termChar((unsigned char)textP[k]);
    }
}

// Ends a line unless the cursor is at a line's start already.
static void
lineEnd(void)
{
    if(!atLineStart)
    {
        termChar('\n');
    }
}

static void
starLine(void)
{
    termText("*\n");
}

// The next byte the client typed, telnet's commands taken out; -1 once it has gone.
static int
termGet(void)
{
    ssize_t r;
    int c;

    for(;;)
    {
        if(inAt >= inLen)
        {
            flushOut();
            if(clientFd < 0)
            {
                return -1;
            }
            r = recv(clientFd, inBuf, sizeof(inBuf), 0);
            if(r <= 0)
            {
                close(clientFd);
                clientFd = -1;
                return -1;
            }
            inLen = (size_t)r;
            inAt = 0;
        }
        c = inBuf[inAt];
        inAt++;
        switch(telnetState)
        {
        case 0:
            if(c == TELNET_IAC)
            {
                telnetState = 1;
                break;
            }
            return c;
        case 1:
            if(c == TELNET_IAC)
            {
                telnetState = 0;
            }
            else if(c == TELNET_SB)
            {
                telnetState = 3;
            }
            else if((c >= TELNET_WILL) && (c <= TELNET_DONT))
            {
                telnetState = 2;
            }
            else
            {
                telnetState = 0;
            }
            break;
        case 2:
            telnetState = 0;
            break;
        case 3:
            telnetState = (c == TELNET_IAC) ? 4 : 3;
            break;
        default:
            telnetState = (c == TELNET_SE) ? 0 : 3;
            break;
        }
    }
}

// READ, the number typed after a clarification. Blanks before it are passed over,
// among them the Return the sentence ended with; it ends at a Return or a space.
// A line with anything but digits is NIL. A client gone stops the sentence.
V
hostRead(void)
{
    int c;
    int any;
    int bad;
    long n;
    long t0;

    any = 0;
    bad = 0;
    n = 0;
    t0 = topNowMicros();
    for(;;)
    {
        c = termGet();
        if(c < 0)
        {
            rtAbort("THE CLIENT HAS GONE");
        }
        c &= 0177;
        if((c == '\r') || (c == '\n') || (c == ' '))
        {
            if(any)
            {
                break;
            }
            continue;
        }
        if(c == 0)
        {
            continue;
        }
        any = 1;
        if((c >= ' ') && (c != CHAR_DEL))
        {
            termChar(c);
        }
        if(isdigit(c))
        {
            n = (n * 10) + (c - '0');
        }
        else
        {
            bad = 1;
        }
    }
    termChar('\n');
    readMicros += topNowMicros() - t0;
    if(bad)
    {
        return NIL;
    }
    answers++;
    if(answers > ANSWERS_MAX)
    {
        rtAbort("THE QUESTION WILL NOT TAKE ITS ANSWER");
    }
    return FIX(n);
}

int
hostTyi(int peek)
{
    (void)peek;
    rtStopInput();
}

// What a sentence's clock leaves out: the hand's pace and the user's typing.
long
hostIdleMicros(void)
{
    return sceneHandMicros() + readMicros;
}

void
hostBuilt(V structure)
{
    built = rtCons(structure, built);
}

void
hostPlanDone(void)
{
    sceneRefresh();
}

// A name as the replies print it: a " is an apostrophe, unless it stands alone.
static void
nameOut(const char *sP)
{
    char c;

    if(!strcmp(sP, "\"") || !strcmp(sP, "\"."))
    {
        termText(sP);
        return;
    }
    for(; *sP; sP++)
    {
        c = (*sP == '"') ? '\'' : *sP;
        termChar((unsigned char)c);
    }
}

static void
valueOut(V x)
{
    char *textP;

    if(IS_SYM(x))
    {
        nameOut(SYMP(x)->nameP);
        return;
    }
    textP = rtToString(x, 0);
    termText(textP);
    free(textP);
}

// The characters valueOut prints for a value.
static size_t
valueLen(V x)
{
    char *textP;
    size_t n;

    if(IS_SYM(x))
    {
        return strlen(SYMP(x)->nameP);
    }
    textP = rtToString(x, 0);
    n = strlen(textP);
    free(textP);
    return n;
}

// The separator before a word of n characters on the WORDS: line, as the Lisp's PRINT3
// has it: a space where the word and the space fit under OUT_WIDTH, else a new line,
// and nothing at a line's start.
static void
wordSeparator(size_t n)
{
    if(termCol == 0)
    {
        return;
    }
    termChar((((size_t)termCol + n + 1) < OUT_WIDTH) ? ' ' : '\n');
}

static void
numberOut(V x)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%lo", (unsigned long)(long)rtNumber(x));
    termText(buf);
}

// One call of a plan on its own line, as /DO prints it.
static void
callOut(int which, int n, V *argsP)
{
    static const char *const namesP[] = {"MOVETO", "GRASP", "UNGRASP", "BLINK"};
    int i;

    termText(namesP[which]);
    for(i = 0; i < n; i++)
    {
        termChar(' ');
        if(IS_SYM(argsP[i]))
        {
            valueOut(argsP[i]);
        }
        else
        {
            numberOut(argsP[i]);
        }
    }
    termChar('\n');
}

void
hostDisplay(int which, int n, V *argsP)
{
    if(planOn)
    {
        callOut(which, n, argsP);
        planPrinted = 1;
    }
    // The hand takes seconds, and what is printed so far must not wait for it.
    flushOut();
    switch(which)
    {
    case 0:
        sceneMoveto((int)rtNumber(argsP[0]), (int)rtNumber(argsP[1]), (int)rtNumber(argsP[2]));
        break;
    case 1:
        sceneGrasp(sceneObject(SYMP(argsP[0])->nameP));
        break;
    case 2:
        sceneUngrasp();
        break;
    default:
        break;
    }
}

// The plan's calls, printed without being done.
static void
planOut(V plan)
{
    static const char *const namesP[] = {"MOVETO", "GRASP", "UNGRASP", "BLINK"};
    V p;
    V form;
    V x;
    V args[4];
    int which;
    int n;

    for(p = plan; IS_CONS(p); p = rtCdr(p))
    {
        form = rtCar(p);
        for(which = 0; which < 4; which++)
        {
            if(rtCar(form) == SYMV(rtIntern(namesP[which])))
            {
                n = 0;
                for(x = rtCdr(form); IS_CONS(x) && (n < 4); x = rtCdr(x))
                {
                    args[n] = rtEval(rtCar(x));
                    n++;
                }
                callOut(which, n, args);
            }
        }
    }
}

// A node's register, given its symbol or the list that starts with it.
static V
nodeReg(V node, const char *nameP)
{
    return rtGet(node, SYMV(rtIntern(nameP)));
}

static void
featuresOut(V node)
{
    V f;

    termChar('(');
    for(f = nodeReg(node, "FEATURES"); IS_CONS(f); f = rtCdr(f))
    {
        valueOut(rtCar(f));
        if(IS_CONS(rtCdr(f)))
        {
            termChar(' ');
        }
    }
    termChar(')');
}

static void
wordsOut(V node)
{
    V nb;
    V n;

    nb = nodeReg(node, "FIRSTWORD");
    n = nodeReg(node, "WORDAFTER");
    while(IS_CONS(nb) && (nb != n))
    {
        valueOut(rtCar(nb));
        nb = rtCdr(nb);
        if(IS_CONS(nb) && (nb != n))
        {
            termChar(' ');
        }
    }
}

// A tree, a node a line: a unit's features then its words, a word's the other way
// round, each level two spaces in, the first parsed first.
static void
treeOut(V node, int depth)
{
    V d;
    V list;
    int i;

    for(i = 0; i < depth; i++)
    {
        termText("  ");
    }
    d = nodeReg(node, "DAUGHTERS");
    if(!IS_CONS(d))
    {
        wordsOut(node);
        termText("  ");
        featuresOut(node);
        termChar('\n');
        return;
    }
    featuresOut(node);
    termText("  ");
    wordsOut(node);
    termChar('\n');
    list = NIL;
    for(; IS_CONS(d); d = rtCdr(d))
    {
        list = rtCons(rtCar(d), list);
    }
    keep = rtCons(list, keep);
    for(; IS_CONS(list); list = rtCdr(list))
    {
        treeOut(rtCar(list), depth + 1);
    }
}

// The Lisp's value of a name.
static V
value(const char *nameP)
{
    return rtIntern(nameP)->value;
}

// After a sentence or a stopped one: the end of its line, then what is due.
static void
sentenceReply(int stop)
{
    V c;

    lineEnd();
    if(stop == STOP_ABORT)
    {
        termText("I DON'T UNDERSTAND.\n");
    }
    else if((stop == STOP_NONE) && treeOn)
    {
        c = value("C");
        if(IS_CONS(c))
        {
            treeOut(rtCar(c), 0);
        }
    }
    keep = NIL;
}

static void
readerClear(void)
{
    typed = NIL;
    typedLen = 0;
    wordLen = 0;
    punct = 0;
    newWord = 0;
    quoteOpen = 0;
}

// The sentence so far in the order typed.
static V
typedForward(void)
{
    V res;
    V x;

    res = NIL;
    for(x = typed; IS_CONS(x); x = rtCdr(x))
    {
        res = rtCons(rtCar(x), res);
    }
    return res;
}

// The sentence so far, for the user to go on from.
static void
typedShow(void)
{
    V x;

    for(x = typedForward(); IS_CONS(x); x = rtCdr(x))
    {
        valueOut(rtCar(x));
        termChar(' ');
    }
}

// A sentence read whole: its words and punctuation, what it means, then *. A
// sentence with no words gets only the *.
static void
sentenceRun(int finalChar)
{
    V x;
    V words;
    char name[2];

    lineEnd();
    if(typedLen)
    {
        words = typedForward();
        termCol = 0;
        termText("WORDS:");
        for(x = words; IS_CONS(x); x = rtCdr(x))
        {
            wordSeparator(valueLen(rtCar(x)));
            valueOut(rtCar(x));
        }
        wordSeparator(1);
        termChar(finalChar);
        termChar('\n');
        name[0] = (char)finalChar;
        name[1] = 0;
        built = NIL;
        planPrinted = 0;
        answers = 0;
        // A pasted line comes whole, so termGet would not flush its echo until the
        // sentence was done, and the hand can take a minute over it.
        flushOut();
        sentenceReply(topSentence(words, SYMV(rtIntern(name))));
    }
    starLine();
}

// The punctuation waiting: . ? or ! ends the sentence; : and ; go into it as words.
static void
punctDo(void)
{
    int c;
    char name[2];

    c = punct;
    punct = 0;
    if((c == ':') || (c == ';'))
    {
        name[0] = (char)c;
        name[1] = 0;
        typed = rtCons(SYMV(rtIntern(name)), typed);
        typedLen++;
        return;
    }
    sentenceRun(c);
    readerClear();
}

// A word through NWORD, with the sentence given as its SENT. Says what it said.
static V
wordAnalyze(const char *textP, int len, int fresh, V *sentP)
{
    V chars;
    V args[2];
    V res;
    Symbol *sentSymP;
    V saved;
    char one[2];
    int k;

    chars = NIL;
    one[1] = 0;
    for(k = 0; k < len; k++)
    {
        one[0] = textP[k];
        chars = rtCons(SYMV(rtIntern(one)), chars);
    }
    sentSymP = rtIntern("SENT");
    saved = sentSymP->value;
    sentSymP->value = *sentP;
    args[0] = chars;
    args[1] = fresh ? SYMV(rtIntern("T")) : NIL;
    res = rtApply(SYMV(rtIntern("NWORD")), 2, args);
    *sentP = sentSymP->value;
    sentSymP->value = saved;
    return res;
}

// The word typed so far, run under a frame so that an error in it is only the
// word's. Says what NWORD said, or NIL.
static V
wordRun(void)
{
    Frame frame;
    V res;
    int r;

    res = NIL;
    rtSentenceStart();
    rtFrameEnter(&frame, FRAME_TOP);
    r = setjmp(frame.jump);
    if(r == 0)
    {
        res = wordAnalyze(word, wordLen, newWord, &typed);
    }
    rtFramePop(&frame);
    return res;
}

// A word's end: the word looked up and put in the sentence, or the sentence
// stopped at it if it is unknown; then any punctuation that ended it.
static void
wordEnd(void)
{
    V res;
    int k;
    V x;

    if(wordLen)
    {
        res = wordRun();
        newWord = 0;
        if(res == NIL)
        {
            lineEnd();
            termText("SORRY, I DON'T KNOW THE WORD \"");
            for(k = 0; k < wordLen; k++)
            {
                termChar((word[k] == WORD_APOSTROPHE) ? '\'' : word[k]);
            }
            termText("\".\nPLEASE TYPE RETURN AND CONTINUE THE SENTENCE.\n");
            wordLen = 0;
            punct = 0;
            quoteOpen = 0;
            mode = MODE_WAIT;
            return;
        }
        wordLen = 0;
        typedLen = 0;
        for(x = typed; IS_CONS(x); x = rtCdr(x))
        {
            typedLen++;
        }
    }
    if(punct)
    {
        punctDo();
    }
}

static void
wordAdd(int c)
{
    if(wordLen < WORD_MAX)
    {
        word[wordLen] = (char)c;
        wordLen++;
    }
}

static void commandLine(void);

// One character of a command line.
static void
commandChar(int c)
{
    if((c == '\r') || (c == '\n'))
    {
        if(c == '\r')
        {
            lineEnd();
        }
        mode = MODE_SENTENCE;
        if(lineLen)
        {
            line[lineLen] = 0;
            atLineStart = 1;
            commandLine();
        }
        return;
    }
    if((c == CHAR_BS) || (c == CHAR_DEL))
    {
        if(lineLen)
        {
            lineLen--;
            termText("\b \b");
        }
        return;
    }
    if((c >= ' ') && (lineLen < LINE_MAX_CHARS))
    {
        line[lineLen] = (char)c;
        lineLen++;
    }
}

// One character of a sentence.
static void
sentenceChar(int c)
{
    switch(c)
    {
    case '\r':
        lineEnd();
        wordEnd();
        return;
    case '\n':
    case ' ':
        wordEnd();
        return;
    case CHAR_BS:
    case CHAR_DEL:
        if(wordLen)
        {
            wordLen--;
            termText("\b \b");
        }
        else if(typedLen)
        {
            typed = rtCdr(typed);
            typedLen--;
            termChar('\n');
            typedShow();
        }
        return;
    case CHAR_CTRL_U:
        readerClear();
        lineEnd();
        return;
    case '/':
        if(!typedLen && !wordLen && !quoteOpen)
        {
            mode = MODE_COMMAND;
            lineLen = 0;
        }
        return;
    case '\'':
        wordAdd(WORD_APOSTROPHE);
        return;
    case '"':
        if(quoteOpen)
        {
            quoteOpen = 0;
            wordEnd();
        }
        else
        {
            quoteOpen = 1;
            newWord = 1;
        }
        return;
    case '.':
    case '?':
    case '!':
    case ':':
    case ';':
        punct = c;
        wordEnd();
        return;
    case '=':
        if(!wordLen)
        {
            wordAdd('=');
        }
        return;
    default:
        if(isalnum(c))
        {
            wordAdd(c);
        }
        return;
    }
}

// One character as it is read, lower case folded to upper; a Return read away
// from a line's start is followed by our line feed.
static void
readerChar(int c)
{
    c &= 0177;
    if((c >= ' ') && (c != CHAR_DEL))
    {
        termChar(c);
    }
    if(isalpha(c))
    {
        c = toupper(c);
    }
    if(c == '\n')
    {
        lineEnd();
    }
    switch(mode)
    {
    case MODE_COMMAND:
        commandChar(c);
        break;
    case MODE_WAIT:
        if((c == '\r') || (c == '\n'))
        {
            if(c == '\r')
            {
                lineEnd();
            }
            mode = MODE_SENTENCE;
            typedShow();
        }
        break;
    default:
        sentenceChar(c);
        break;
    }
}

// The next token of the command line: a word, or ( or ). Empty at its end.
static int
tokenRead(void)
{
    int n;

    n = 0;
    while(*linePosP == ' ')
    {
        linePosP++;
    }
    if(!*linePosP)
    {
        token[0] = 0;
        return 0;
    }
    if((*linePosP == '(') || (*linePosP == ')'))
    {
        token[0] = *linePosP;
        token[1] = 0;
        linePosP++;
        return 1;
    }
    while(*linePosP && (*linePosP != ' ') && (*linePosP != '(') && (*linePosP != ')'))
    {
        if(n < TOKEN_MAX)
        {
            token[n] = *linePosP;
            n++;
        }
        linePosP++;
    }
    token[n] = 0;
    return n;
}

// An octal number from the token: says whether it was one.
static int
tokenNumber(const char *sP, long *nP)
{
    long v;

    if(!*sP)
    {
        return 0;
    }
    v = 0;
    for(; *sP; sP++)
    {
        if((*sP < '0') || (*sP > '7'))
        {
            return 0;
        }
        v = (v * 8) + (*sP - '0');
    }
    *nP = v;
    return 1;
}

// A name the program knows, as it stands or with a # before it; 0 if not.
static V
constantOf(const char *sP)
{
    char buf[TOKEN_MAX + 2];
    Symbol *symP;

    if(sP[0] != '#')
    {
        snprintf(buf, sizeof(buf), "#%s", sP);
        symP = rtIntern(buf);
        if(symP->plist != NIL)
        {
            return SYMV(symP);
        }
    }
    symP = rtIntern(sP);
    return (symP->plist != NIL) ? SYMV(symP) : 0;
}

// An atom from the token: an object (:B1), an event (EE, or En in octal), a number
// in octal, a constant, or NIL. The Lisp's events are numbered in decimal.
// Says whether it was one.
static int
atomRead(V *atomP)
{
    long n;
    char buf[32];
    V c;

    if(token[0] == ':')
    {
        c = constantOf(token);
        if(!c)
        {
            return 0;
        }
        *atomP = c;
        return 1;
    }
    if(!strcmp(token, "NIL"))
    {
        *atomP = NIL;
        return 1;
    }
    if(!strcmp(token, "EE"))
    {
        *atomP = SYMV(rtIntern("EE"));
        return 1;
    }
    if((token[0] == 'E') && tokenNumber(token + 1, &n))
    {
        snprintf(buf, sizeof(buf), "E%ld", n);
        *atomP = SYMV(rtIntern(buf));
        return 1;
    }
    if(tokenNumber(token, &n))
    {
        *atomP = FIX(n);
        return 1;
    }
    c = constantOf(token);
    if(!c)
    {
        return 0;
    }
    *atomP = c;
    return 1;
}

// An atom, or a parenthesized list of atoms beginning at the token.
static int
valueRead(V *valueP)
{
    V items[LIST_MAX];
    V res;
    int n;

    if(strcmp(token, "("))
    {
        return atomRead(valueP);
    }
    n = 0;
    for(;;)
    {
        if(!tokenRead())
        {
            return 0;
        }
        if(!strcmp(token, ")"))
        {
            break;
        }
        if((n == LIST_MAX) || !atomRead(&items[n]))
        {
            return 0;
        }
        n++;
    }
    res = NIL;
    while(n > 0)
    {
        n--;
        res = rtCons(items[n], res);
    }
    *valueP = res;
    return 1;
}

// A goal and its arguments from the line: (#NAME args). Says whether they are right.
static int
goalRead(V *goalP)
{
    const GoalForm *gP;
    char name[TOKEN_MAX + 2];
    V args;
    V a;
    V list;
    V rest[TOKEN_MAX];
    int i;
    int n;

    if(!tokenRead())
    {
        return 0;
    }
    for(gP = goalForms; gP->nameP; gP++)
    {
        if(!strcmp(token, gP->nameP))
        {
            break;
        }
    }
    if(!gP->nameP)
    {
        return 0;
    }
    snprintf(name, sizeof(name), "#%s", gP->nameP);
    args = NIL;
    if(gP->nArgs < 0)
    {
        n = 0;
        while(tokenRead())
        {
            if((n == TOKEN_MAX) || !valueRead(&rest[n]))
            {
                return 0;
            }
            n++;
        }
        list = NIL;
        while(n > 0)
        {
            n--;
            list = rtCons(rest[n], list);
        }
        args = rtCons(list, NIL);
    }
    else
    {
        for(i = 0; i < gP->nArgs; i++)
        {
            if(!tokenRead())
            {
                *goalP = NIL;
                return 1;
            }
            if(!valueRead(&rest[i]))
            {
                return 0;
            }
        }
        for(i = gP->nArgs - 1; i >= 0; i--)
        {
            args = rtCons(rest[i], args);
        }
        if(tokenRead())
        {
            return 0;
        }
    }
    a = rtCons(SYMV(rtIntern(name)), args);
    *goalP = a;
    return 1;
}

static void
planOrFail(int ok, int stop)
{
    if(stop)
    {
        termText("I CAN'T\n");
    }
    else
    {
        termText(ok ? "OK\n" : "I CAN'T\n");
    }
}

// Plans a goal in a child and forgets it. Says whether the planning was stopped, in
// which case the Lisp's world, part way through a plan, must not be left like that.
static int
goalStops(V goal)
{
    pid_t pid;
    int ok;
    int status;

    flushOut();
    pid = fork();
    if(pid == 0)
    {
        sceneDetach();
        lispMuted = 1;
        _exit(topGoal(goal, &ok, 0, NULL) ? 1 : 0);
    }
    if(pid < 0)
    {
        return 0;
    }
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && (WEXITSTATUS(status) == 1);
}

static void
commandDo(V goal)
{
    int ok;
    int stop;

    lineEnd();
    if((goal == NIL) || goalStops(goal))
    {
        planOrFail(0, 0);
        return;
    }
    lispMuted = 1;
    stop = topGoal(goal, &ok, 1, planOut);
    lispMuted = 0;
    lineEnd();
    planOrFail(ok, stop);
}

// TRY plans in a child that goes away, so that the world, the events and the
// histories stay as they were; only the random numbers it took stay taken, which it
// passes back. Its times come back to nobody.
static void
commandTry(V goal)
{
    pid_t pid;
    int ok;
    int stop;
    int status;
    int fds[2];
    unsigned seedNow;

    if(goal == NIL)
    {
        planOrFail(0, 0);
        return;
    }
    flushOut();
    if(pipe(fds) < 0)
    {
        return;
    }
    pid = fork();
    if(pid == 0)
    {
        close(fds[0]);
        sceneDetach();
        lispMuted = 1;
        stop = topGoal(goal, &ok, 0, planOut);
        lispMuted = 0;
        lineEnd();
        planOrFail(ok, stop);
        flushOut();
        seedNow = topSeedNow();
        if(write(fds[1], &seedNow, sizeof(seedNow)) < 0)
        {
            _exit(1);
        }
        _exit(0);
    }
    close(fds[1]);
    if((pid > 0) && (read(fds[0], &seedNow, sizeof(seedNow)) == (ssize_t)sizeof(seedNow)))
    {
        topSeed(seedNow);
    }
    close(fds[0]);
    if(pid > 0)
    {
        waitpid(pid, &status, 0);
    }
}

// A form to find every assertion of a predicate that matches, ? matching
// anything: (THVAL '(THFIND ALL ($?A ...) (A ...) (THGOAL (pred ...))) env).
static V
findMatches(V pred, int n, const V *argsP, int *anyP)
{
    static const char *const varP[] = {"V0", "V1", "V2", "V3"};
    V skeleton;
    V vars;
    V pattern;
    V thv;
    V quote;
    V form;
    V v;
    Symbol *anySymP;
    int i;

    thv = SYMV(rtIntern("THV"));
    quote = SYMV(rtIntern("QUOTE"));
    anySymP = rtIntern("?");
    skeleton = NIL;
    vars = NIL;
    pattern = NIL;
    for(i = n - 1; i >= 0; i--)
    {
        v = SYMV(rtIntern(varP[i]));
        if(argsP[i] == SYMV(anySymP))
        {
            skeleton = rtCons(rtCons(thv, rtCons(v, NIL)), skeleton);
            vars = rtCons(v, vars);
            pattern = rtCons(rtCons(thv, rtCons(v, NIL)), pattern);
        }
        else
        {
            skeleton = rtCons(argsP[i], skeleton);
            pattern = rtCons(argsP[i], pattern);
        }
    }
    *anyP = (vars != NIL);
    pattern = rtCons(pred, pattern);
    form = rtCons(SYMV(rtIntern("THGOAL")), rtCons(pattern, NIL));
    form = rtCons(SYMV(rtIntern("THFIND")),
                  rtCons(SYMV(rtIntern("ALL")),
                         rtCons(skeleton, rtCons(vars, rtCons(form, NIL)))));
    form = rtCons(SYMV(rtIntern("THVAL")),
                  rtCons(rtCons(quote, rtCons(form, NIL)),
                         rtCons(rtCons(quote, rtCons(rtCons(rtCons(SYMV(rtIntern("EV")),
                                                                   rtCons(SYMV(rtIntern("COMMAND")), NIL)),
                                                            NIL), NIL)), NIL)));
    return rtEval(form);
}

// An atom as a line of the database shows it: an event's number in octal.
static void
atomOut(V x)
{
    const char *sP;
    char buf[32];

    if(IS_SYM(x))
    {
        sP = SYMP(x)->nameP;
        if((sP[0] == 'E') && isdigit((unsigned char)sP[1]))
        {
            snprintf(buf, sizeof(buf), "E%lo", (unsigned long)strtol(sP + 1, NULL, 10));
            termText(buf);
            return;
        }
        valueOut(x);
        return;
    }
    if(IS_FIX(x) || IS_FLT(x))
    {
        numberOut(x);
        return;
    }
    if(x == NIL)
    {
        termText("NIL");
        return;
    }
    termChar('(');
    for(; IS_CONS(x); x = rtCdr(x))
    {
        atomOut(rtCar(x));
        if(IS_CONS(rtCdr(x)))
        {
            termChar(' ');
        }
    }
    termChar(')');
}

static void
factsOut(V pred, int n, const V *argsP)
{
    V found;
    V f;
    V x;
    int i;
    int any;

    found = findMatches(pred, n, argsP, &any);
    for(f = found; IS_CONS(f); f = rtCdr(f))
    {
        valueOut(pred);
        x = rtCar(f);
        for(i = 0; (i < n) && IS_CONS(x); i++)
        {
            termChar(' ');
            atomOut(rtCar(x));
            x = rtCdr(x);
        }
        termChar('\n');
    }
}

// #AT, from the location table, for each object it lists, or for the one named.
static void
locationsOut(int n, const V *argsP)
{
    V t;
    V e;

    for(t = value("ATABLE"); IS_CONS(t); t = rtCdr(t))
    {
        e = rtCar(t);
        if(strncmp(SYMP(rtCar(e))->nameP, ":BW", 3)
           && ((n == 0) || (argsP[0] == SYMV(rtIntern("?"))) || (argsP[0] == rtCar(e))))
        {
            termText("#AT ");
            valueOut(rtCar(e));
            termChar(' ');
            atomOut(rtCar(rtCdr(e)));
            termChar('\n');
        }
    }
}

static V findPred;
static int findN;

// Each predicate's facts, by every number of arguments.
static void
findAllOne(Symbol *symP)
{
    V args[FIND_ARGS];
    int i;

    if((symP->nameP[0] != '#') || (rtGet(SYMV(symP), SYMV(rtIntern("THASSERTION"))) == NIL)
       || !strcmp(symP->nameP, "#AT"))
    {
        return;
    }
    findPred = SYMV(symP);
    for(i = 0; i < FIND_ARGS; i++)
    {
        args[i] = SYMV(rtIntern("?"));
    }
    for(findN = 1; findN <= FIND_ARGS; findN++)
    {
        factsOut(findPred, findN, args);
    }
}

// FIND, under a frame: an error in it is only a shorter list.
static void
commandFind(V pred, int n, const V *argsP, int all)
{
    Frame frame;
    int r;

    rtFrameEnter(&frame, FRAME_TOP);
    r = setjmp(frame.jump);
    if(r == 0)
    {
        if(all)
        {
            rtEachSymbol(findAllOne);
            locationsOut(0, NULL);
        }
        else if(pred == SYMV(rtIntern("#AT")))
        {
            locationsOut(n, argsP);
        }
        else
        {
            factsOut(pred, n, argsP);
        }
    }
    rtFramePop(&frame);
}

// What the planner has done, oldest first.
static void
eventsOut(void)
{
    V list;
    V e;
    V form;
    V x;
    V end;
    V why;

    list = rtEval(rtCons(SYMV(rtIntern("EVENTS")), NIL));
    list = rtApply(SYMV(rtIntern("REVERSE")), 1, &list);
    for(; IS_CONS(list); list = rtCdr(list))
    {
        e = rtCar(list);
        atomOut(e);
        termChar(' ');
        form = rtApply(SYMV(rtIntern("EVFORM")), 1, &e);
        if(IS_CONS(form))
        {
            atomOut(rtCar(form));
            for(x = rtCdr(rtCdr(form)); IS_CONS(x); x = rtCdr(x))
            {
                termChar(' ');
                atomOut(rtCar(x));
            }
        }
        else
        {
            termChar('?');
        }
        termText(" START ");
        atomOut(rtGet(e, SYMV(rtIntern("START"))));
        termText(" END ");
        end = rtGet(e, SYMV(rtIntern("END")));
        if(end == NIL)
        {
            termChar('?');
        }
        else
        {
            atomOut(end);
        }
        termText(" WHY ");
        why = rtGet(e, SYMV(rtIntern("WHY")));
        atomOut(why);
        termChar('\n');
    }
}

// Where an object has been, newest first: the time, the place, what held it.
static void
historyOut(V obj)
{
    V list;
    V e;

    for(list = rtGet(obj, SYMV(rtIntern("HISTORY"))); IS_CONS(list); list = rtCdr(list))
    {
        e = rtCar(list);
        atomOut(rtCar(e));
        termChar(' ');
        atomOut(rtCar(rtCdr(e)));
        termChar(' ');
        atomOut(rtCar(rtCdr(rtCdr(e))));
        termChar('\n');
    }
}

// /SM: each structure the last sentence built, in the order made, its properties
// newest first.
static void
structuresOut(void)
{
    V list;
    V x;
    V p;
    V ind;
    V val;
    int first;
    char *textP;

    list = NIL;
    for(x = built; IS_CONS(x); x = rtCdr(x))
    {
        list = rtCons(rtCar(x), list);
    }
    for(; IS_CONS(list); list = rtCdr(list))
    {
        x = rtCar(list);
        if(!IS_SYM(x))
        {
            valueOut(x);
            termChar('\n');
            continue;
        }
        valueOut(x);
        termChar(':');
        first = 1;
        for(p = SYMP(x)->plist; IS_CONS(p); p = rtCdr(rtCdr(p)))
        {
            ind = rtCar(p);
            val = rtCar(rtCdr(p));
            if((ind == SYMV(rtIntern("PARSENODE="))) && IS_CONS(val))
            {
                val = rtCar(val);
            }
            termText(first ? " " : "; ");
            first = 0;
            valueOut(ind);
            termChar(' ');
            textP = rtToString(val, 2);
            termText(textP);
            free(textP);
        }
        termChar('\n');
    }
}

// ms from microseconds, rounded down, in octal.
static void
millisOut(const char *labelP, long micros)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "%s%lo", labelP, (unsigned long)(micros / MICROS_PER_MILLI));
    termText(buf);
}

static void
roomOut(void)
{
    char buf[80];
    V list;
    long n;

    snprintf(buf, sizeof(buf), "CELLS %lo\nCOLLECTIONS %lo\n", (unsigned long)rtCellsLive(),
             (unsigned long)rtCollections());
    termText(buf);
    n = 0;
    for(list = value("EVENTLIST"); IS_CONS(list); list = rtCdr(list))
    {
        n++;
    }
    snprintf(buf, sizeof(buf), "EVENTS %lo\n", (unsigned long)n);
    termText(buf);
}

static void
wordEntryOut(V w)
{
    V f;
    V root;

    valueOut(w);
    termChar(':');
    for(f = rtGet(w, SYMV(rtIntern("FEATURES"))); IS_CONS(f); f = rtCdr(f))
    {
        termChar(' ');
        valueOut(rtCar(f));
    }
    root = rtGet(w, SYMV(rtIntern("ROOT")));
    if(root != NIL)
    {
        termText(" ROOT ");
        valueOut(root);
    }
    termChar('\n');
}

static void
featureCollectFrom(V list)
{
    for(; IS_CONS(list); list = rtCdr(list))
    {
        if(IS_SYM(rtCar(list)) && (rtMemq(rtCar(list), knownFeatures) == NIL))
        {
            knownFeatures = rtCons(rtCar(list), knownFeatures);
        }
    }
}

static void
featureCollect(Symbol *symP)
{
    featureCollectFrom(rtGet(SYMV(symP), SYMV(rtIntern("FEATURES"))));
    featureCollectFrom(rtGet(SYMV(symP), SYMV(rtIntern("ELIM"))));
}

// A token of a /WORD line in the word's own spelling: says whether it can be one.
static int
wordToken(char *sP)
{
    for(; *sP; sP++)
    {
        if(*sP == '\'')
        {
            *sP = WORD_APOSTROPHE;
        }
        else if(!isalnum((unsigned char)*sP) && !strchr("\"-=,*:;", *sP))
        {
            return 0;
        }
    }
    return 1;
}

// /WORD x, its features and root, derived if need be; /WORD x f ..., the word
// given these features and no root for the rest of the run.
static int
commandWord(void)
{
    char name[WORD_MAX + 1];
    V sent;
    V res;
    V feats;
    V f;
    V x;
    V args[4];
    Frame frame;
    int r;

    if(!tokenRead() || !wordToken(token) || (strlen(token) > WORD_MAX))
    {
        return LINE_WORD_BAD;
    }
    strcpy(name, token);
    if(!tokenRead())
    {
        rtFrameEnter(&frame, FRAME_TOP);
        r = setjmp(frame.jump);
        if(r == 0)
        {
            sent = NIL;
            res = wordAnalyze(name, (int)strlen(name), 0, &sent);
            if(res == NIL)
            {
                valueOut(SYMV(rtIntern(name)));
                termText(": UNKNOWN\n");
            }
            else
            {
                sent = rtApply(SYMV(rtIntern("REVERSE")), 1, &sent);
                for(x = sent; IS_CONS(x); x = rtCdr(x))
                {
                    wordEntryOut(rtCar(x));
                }
            }
        }
        rtFramePop(&frame);
        return 1;
    }
    if(knownFeatures == NIL)
    {
        rtEachSymbol(featureCollect);
    }
    feats = NIL;
    do
    {
        f = SYMV(rtIntern(token));
        if(rtMemq(f, knownFeatures) == NIL)
        {
            return LINE_WORD_BAD;
        }
        feats = rtCons(f, feats);
    }
    while(tokenRead());
    feats = rtApply(SYMV(rtIntern("REVERSE")), 1, &feats);
    args[0] = SYMV(rtIntern(name));
    args[1] = feats;
    args[2] = NIL;
    args[3] = NIL;
    rtApply(SYMV(rtIntern("BUILDWORD")), 4, args);
    args[1] = SYMV(rtIntern("ROOT"));
    rtApply(SYMV(rtIntern("REMPROP")), 2, args);
    wordEntryOut(args[0]);
    return 1;
}

static int
switchRead(int *flagP)
{
    tokenRead();
    if(!strcmp(token, "ON"))
    {
        *flagP = 1;
    }
    else if(!strcmp(token, "OFF"))
    {
        *flagP = 0;
    }
    else
    {
        return 0;
    }
    return !tokenRead();
}

static int
pointRead(long *pP)
{
    int i;

    for(i = 0; i < 3; i++)
    {
        if(!tokenRead() || (strlen(token) > 7) || !tokenNumber(token, &pP[i]))
        {
            return 0;
        }
    }
    return !tokenRead();
}

// A reply wraps at a space under OUT_WIDTH: the Lisp's PRINT3 does it, by LINEL.
static void
lineWidthSet(void)
{
    rtIntern("LINEL")->value = FIX(OUT_WIDTH);
    rtOutLineStart();
}

// The world, the events and the words as they started, and the picture.
static void
commandReset(void)
{
    rtReload();
    built = NIL;
    knownFeatures = NIL;
    topInit(1);
    lineWidthSet();
    sceneReset();
}

// Carries out a command line. Says whether it was right; each reply is whole but
// for its *.
static int
commandCarry(void)
{
    V goal;
    V a;
    V pred;
    V args[FIND_ARGS];
    long p[3];
    int n;
    int obj;
    int sx;
    int sy;
    int dummy;

    linePosP = line;
    if(!tokenRead())
    {
        return 1;
    }
    if(!strcmp(token, "WORD"))
    {
        return commandWord();
    }
    if(!strcmp(token, "DO") || !strcmp(token, "TRY"))
    {
        n = !strcmp(token, "TRY");
        if(!goalRead(&goal))
        {
            return 0;
        }
        if(n)
        {
            commandTry(goal);
        }
        else
        {
            commandDo(goal);
        }
        return 1;
    }
    if(!strcmp(token, "TRACE"))
    {
        return switchRead(&dummy);
    }
    if(!strcmp(token, "TREE"))
    {
        return switchRead(&treeOn);
    }
    if(!strcmp(token, "PLAN"))
    {
        return switchRead(&planOn);
    }
    if(!strcmp(token, "SM"))
    {
        if(tokenRead())
        {
            return 0;
        }
        structuresOut();
        return 1;
    }
    if(!strcmp(token, "EVENTS"))
    {
        if(tokenRead())
        {
            return 0;
        }
        eventsOut();
        return 1;
    }
    if(!strcmp(token, "HISTORY"))
    {
        if(!tokenRead() || !atomRead(&a) || !IS_SYM(a) || (sceneObject(SYMP(a)->nameP) < 0)
           || tokenRead())
        {
            return 0;
        }
        historyOut(a);
        return 1;
    }
    if(!strcmp(token, "FIND"))
    {
        if(!tokenRead())
        {
            return 0;
        }
        n = 0;
        if(!strcmp(token, "?"))
        {
            if(tokenRead())
            {
                return 0;
            }
            commandFind(NIL, 0, args, 1);
            return 1;
        }
        if(!valueRead(&pred) || !IS_SYM(pred))
        {
            return 0;
        }
        while(tokenRead())
        {
            if(n == FIND_ARGS)
            {
                return 0;
            }
            if(!strcmp(token, "?"))
            {
                args[n] = SYMV(rtIntern("?"));
            }
            else if(!valueRead(&args[n]))
            {
                return 0;
            }
            n++;
        }
        commandFind(pred, n, args, 0);
        return 1;
    }
    if(!strcmp(token, "SEED"))
    {
        if(!tokenRead() || !tokenNumber(token, &p[0]) || tokenRead())
        {
            return 0;
        }
        topSeed((unsigned)p[0]);
        return 1;
    }
    if(!strcmp(token, "TIMES"))
    {
        if(tokenRead())
        {
            return 0;
        }
        millisOut("PLAN ", topPlanMicros());
        millisOut(" ALL ", topAllMicros());
        millisOut(" PARSE ", topParseMicros());
        millisOut(" ANSWER ", topAnswerMicros());
        snprintf(line, sizeof(line), "\nMICROSECONDS PARSE %ld ANSWER %ld\n", topParseMicros(),
                 topAnswerMicros());
        termText(line);
        return 1;
    }
    if(!strcmp(token, "ROOM"))
    {
        tokenRead();
        if(token[0] && strcmp(token, "CLEAR"))
        {
            return 0;
        }
        if(tokenRead())
        {
            return 0;
        }
        if(strstr(line, "CLEAR") == NULL)
        {
            roomOut();
        }
        return 1;
    }
    if(!strcmp(token, "RESET"))
    {
        if(tokenRead())
        {
            return 0;
        }
        commandReset();
        return 1;
    }
    if(!strcmp(token, "MOVETO"))
    {
        if(!pointRead(p))
        {
            return 0;
        }
        sceneMoveto((int)p[0], (int)p[1], (int)p[2]);
        return 1;
    }
    if(!strcmp(token, "PROJ"))
    {
        if(!pointRead(p))
        {
            return 0;
        }
        sceneProject((int)p[0], (int)p[1], (int)p[2], &sx, &sy);
        snprintf(line, sizeof(line), "%o %o\n", (unsigned)sx, (unsigned)sy);
        termText(line);
        return 1;
    }
    if(!strcmp(token, "GRASP"))
    {
        tokenRead();
        obj = sceneObject(token);
        if((obj < 0) || tokenRead())
        {
            return 0;
        }
        sceneGrasp(obj);
        return 1;
    }
    if(!strcmp(token, "UNGRASP") || !strcmp(token, "REFRESH") || !strcmp(token, "SCENE")
       || !strcmp(token, "STATUS"))
    {
        n = token[0];
        if(tokenRead())
        {
            return 0;
        }
        switch(n)
        {
        case 'U':
            sceneUngrasp();
            break;
        case 'R':
            sceneRefresh();
            break;
        default:
            if(line[1] == 'C')
            {
                sceneOut();
            }
            else
            {
                sceneStatusOut();
            }
            break;
        }
        return 1;
    }
    return 0;
}

static void
commandLine(void)
{
    int r;

    // A /MOVETO walks the hand with no plan call on the way, as a sentence's does.
    flushOut();
    rtSentenceStart();
    r = commandCarry();
    if(r == LINE_WORD_BAD)
    {
        termText("ERROR\n");
    }
    else if(r == 0)
    {
        termText("ERROR ");
        termText(line);
        termChar('\n');
    }
    starLine();
}

// One connection: the greeting, the banner, then what it types until it goes.
static void
clientServe(int fd)
{
    int c;
    int one;

    clientFd = fd;
    one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if(send(fd, greeting, sizeof(greeting), MSG_NOSIGNAL) < 0)
    {
        close(fd);
        clientFd = -1;
        return;
    }
    inLen = 0;
    inAt = 0;
    telnetState = 0;
    mode = MODE_SENTENCE;
    readerClear();
    atLineStart = 1;
    termText(banner);
    starLine();
    for(;;)
    {
        c = termGet();
        if(c < 0)
        {
            return;
        }
        readerChar(c);
    }
}

void
termServe(int port)
{
    int listenFd;
    int fd;
    int one;
    struct sockaddr_in addr;

    rtAddRoot(&typed);
    rtAddRoot(&built);
    rtAddRoot(&keep);
    rtAddRoot(&lastTree);
    rtAddRoot(&knownFeatures);
    lineWidthSet();
    listenFd = socket(AF_INET, SOCK_STREAM, 0);
    one = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if((bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) || (listen(listenFd, 4) < 0))
    {
        perror("telnet port");
        exit(1);
    }
    for(;;)
    {
        fd = accept(listenFd, NULL, NULL);
        if(fd >= 0)
        {
            clientServe(fd);
        }
    }
}
