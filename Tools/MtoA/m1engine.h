// m1engine.h - macro1's two assembly passes, for mactoam1.
//
// m1engine.c is the assembly core of src/macro/macro1_1.c (Gary A. Messenbrink, Bob Supnik,
// Phil Budne), reading the source from memory and writing nothing itself. Instead it reports
// what it does through the h...() hooks below, which mactoam1.c provides: every term, operator
// and expression it evaluates, every tag, definition and pseudo-instruction, each with the
// source position it came from, macro expansions included.
//
// A position is a source line (0-based) and column, packed by M1POS. The column is a character
// index into the line as read, tabs counted as one.
//
// Where it departs from macro1 on purpose:
// - a tag that names a dummy defines the argument's symbol at the current location, as DEC's
//   MACRO does (macro1 sets the dummy instead, and the symbol stays undefined);
// - dimension, DEC MACRO's, is accepted when no symbol of that name is defined;
// - its literal and constants-block limits are larger;
// - lines are read whole, not cut at 94 characters;
// - form feeds are removed when the source is loaded, not as each line is read.

#ifndef M1ENGINE_H
#define M1ENGINE_H

#define M1POS(line, col)   (((line) << 10) | (col))
#define M1LINE(pos)        ((pos) >> 10)
#define M1COL(pos)         ((pos) & 01777)
#define M1MAXCOL           01776

#define M1SYMLEN           7    // 6 significant characters and the NUL
#define M1MAXARGS          20

// The source, each line without its line end. mactoam1.c loads it.
extern char **m1Lines;
extern int m1LineCount;

// Pseudo-instruction codes, as macro1 numbers them, and dimension.
enum
{
    M1P_DECIMAL, M1P_DEFINE, M1P_FLEX, M1P_CONSTANTS, M1P_OCTAL, M1P_REPEAT, M1P_START,
    M1P_CHAR, M1P_VARIABLES, M1P_TEXT, M1P_NOINPUT, M1P_EXPUNGE, M1P_DIMENSION
};

// What a term is, for hTerm().
enum
{
    M1T_NUMBER,
    M1T_DOT,        // . the location
    M1T_LABEL,      // a tag
    M1T_EQU,        // a name given its value by =
    M1T_VAR,        // a variable, overbarred or dimensioned
    M1T_PERM,       // a permanent symbol: lac, i, 1s
    M1T_UNDEF,      // a name with no value yet
    M1T_DUMMY,      // a dummy of the macro being expanded; info is its number, 1 up
    M1T_R,          // R, the location where the expansion began
    M1T_EPSEUDO,    // decimal or octal as a term; info is the pseudo code
    M1T_PSEUDO,     // another pseudo-instruction where a value is wanted (an error)
    M1T_MACRO,      // a macro name where a value is wanted (an error)
    M1T_FLEX,       // flexo xyz
    M1T_CHAR,       // char lx
    M1T_LITERAL,    // (e) a constant; val is its address, info the value inside
    M1T_GROUP,      // [e]
    M1T_BAD         // an illegal character
};

// Kinds of expression for hExpr().
enum
{
    M1E_WORD,       // a statement that assembles one word
    M1E_LOCCHECK,   // e, which checks the location
    M1E_ORIGIN,     // e/
    M1E_NOWORD,     // a statement whose value is a pseudo-instruction's, decimal for one
    M1E_ASSIGN,     // the right side of name=
    M1E_ARG,        // one argument of a macro use
    M1E_START,      // start e
    M1E_REPEAT,     // the count of a repeat
    M1E_DIMENSION   // the size in dimension a(n)
};

void m1Init(void);
// Pass 1, pass 2, or 3: pass 2 again from the state pass 2 started in.
void m1Pass(int pass);
// Expand a macro that the source never uses, with no arguments, reporting it as pass 4 and
// leaving the assembly state as it was. Called after a pass, so that its body is seen.
void m1Phantom(int macIdx);
int m1Clc(void);
int m1Radix(void);

// Hooks, defined by mactoam1.c. pass is 1, 2 or 3 as above. pos and end are positions; end is
// the position just after the thing reported.
void hTitle(int pass, int line);
void hTerm(int pass, int kind, int pos, int end, int val, int info);
void hUnary(int pass, int pos, int op);
void hOp(int pass, int pos, int op);
void hOpen(int pass, int pos, int bracket);
void hExpr(int pass, int kind, int pos, int end, int val);
void hWord(int pass, int addr, int val);
void hLabel(int pass, int pos, int end, int val, int inMacro, int dummyTag);
void hNumTag(int pass, int pos, int end);
void hAssign(int pass, int pos, int end, int val, int dummy);
void hDefine(int pass, int pos, int nameLine, int macIdx, const char *nameP,
             char args[][M1SYMLEN], int nargs, int termLine);
void hInvokeStart(int pass, int pos, int end, int macIdx);
void hInvokeArgs(int pass, int nargs, int end);
void hInvokeEnd(int pass);
void hInvokeEmpty(int pass, int pos, int end, int macIdx);
void hPseudo(int pass, int pos, int end, int code);
void hConstants(int pass, int pos, int start, int reserved, int nvals, const int *valsP);
void hVariables(int pass, int pos, int start, int end);
void hVarAlloc(int pass, const char *nameP, int addr, int size);
void hText(int pass, int pos, int end, int nwords, const int *wordsP);
void hRepeat(int pass, int pos, int count, int bodyPos, int bodyEnd);
void hRepeatEnd(int pass);
void hStart(int pass, int pos, int end, int haveAddr, int addr);
void hComment(int pass, int pos);
void hError(int pass, int pos, const char *msgP);
void hOrphanLiterals(int pass, int count);
// The assembly cannot go on: write what refuses it and exit. pos may be -1.
void hFatal(int pass, int pos, const char *msgP);

#endif
