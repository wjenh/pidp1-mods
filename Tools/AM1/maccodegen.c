/*
 * Process a parse tree to generate macro source output.
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>

#include "am1.h"
#include "y.tab.h"
#include "type340chars.h"

#define LOADER_HALT 07772       // the halt instruction in our loader, keep in sync with xldr

// macro1 reads at most 94 characters of a line and assembles the rest as a line of its own,
// so a comment is wrapped well inside that: no comment line is wider than this, the whole
// line counted, the statement before a trailing comment included.
#define MACLINEWIDTH 72
#define MACTABSTOP 8            // a tab moves to the next multiple of this column

static int macColumn;           // the column the next character written to the .mac lands in

extern bool sawBank;
extern bool ignoreAsserts;
extern BankContextP banksP;

extern bool doWarn(int warnNo);
extern int evalExpr(PNodeP);
extern int onesComplAdj(int);
extern char *assertText(PNodeP assertP);
extern Word *ovTableWordsP;

static void emitStatements(FILE *, PNodeP);
static void emitOperand(FILE *, PNodeP);
static void emitAscii(FILE *outfP, char *strP);
static void emitText(FILE *outfP, FlexText flexText);
static void emitVars(FILE *outfP, PNodeListP listP);
static int emitConstants(FILE *outfP, SymNodeP nodeP);
static void emitOverlayDirective(FILE *outfP, PNodeP nodeP);
static void emitCommentText(FILE *outfP, const char *strP, bool ownLine);
static void emitComment(FILE *outfP, const char *textP, int len, bool trailing);
static void emitBlockComment(FILE *outfP, const char *textP);
static const char *commentLineEnd(const char *textP, const char *endP, int col);
static int nextColumn(int col, char ch);
static void macPrintf(FILE *outfP, const char *fmtP, ...) __attribute__((format(printf, 2, 3)));

extern void verror(char *msgP, ...);
extern void verrorlf(int lineNo, const char *fileNameP, const char *msgP, ...);

// Walk a tree and emit equivalent macro1 code.  Returns 1; an error exits through verror().
int
macCodegen(FILE *outfP, PNodeP rootP)
{
    // The root is a HEADER.
    // The root lhs is the program body, the rhs the START or STOP at the end of the program.
    macColumn = 0;
    macPrintf(outfP,"%s\n", rootP->value.strP);
    emitStatements(outfP, rootP->leftP);

    // Finish any trailing constants and vars
    for(BankContextP bankP = banksP; bankP; bankP = bankP->nextP)
    {
        if( bankP->constSymP )
        {
            macPrintf(outfP, "/ Constants for bank %d\n", bankP->bank);
            macPrintf(outfP, "%o/\n", bankP->constPC);
            emitConstants(outfP, bankP->constSymP);
        }

        if( bankP->varNodesP )
        {
            macPrintf(outfP, "/ Vars for bank %d\n", bankP->bank);
            macPrintf(outfP, "%o/\n", bankP->varPC);
            emitVars(outfP, bankP->varNodesP);
        }
    }

    if( rootP->rightP->type == STOP )
    {
        if( doWarn(WARN_STOP) )
        {
            fprintf(stderr,"am1: WARNING: stop is not supported by macro1, replacing with a jump to hlt\n");
        }

        macPrintf(outfP,"start %o\n", LOADER_HALT);
    }
    else
    {
        macPrintf(outfP,"start %o\n", rootP->rightP->value.ival);
    }

    return(1);
}

static void
emitStatements(FILE *outfP, PNodeP nodeP)
{
int i, j;
PNodeP node2P;
PNodeP prevP = NILP;    // the node before this one, for EMPTYLINE
bool noNl = false;
char *cP;

    while( nodeP )
    {
        switch( nodeP->type )
        {
        case COMMENT:
            emitCommentText(outfP, nodeP->value.strP, ((nodeP->flags & PN_SOL) != 0));
            break;

        case CSCOMMENT:
            emitBlockComment(outfP, nodeP->value.strP);
            noNl = true;        // the lexer's newline after the "*/" is the comment's own
            break;

        case ORIGIN:
            macPrintf(outfP, "%o/", nodeP->value.ival);
            if( nodeP->rightP )
            {
                macPrintf(outfP," ");
                emitOperand(outfP, nodeP->rightP);
            }
            else
            {
                macPrintf(outfP, "\n");
            }
            break;

        case EXPR:
            macPrintf(outfP, "    ");
            emitOperand(outfP, nodeP->rightP);
            break;

        case LOCATION:
            macPrintf(outfP, "%s,", nodeP->value.symP->name);
            if( nodeP->rightP )
            {
                macPrintf(outfP," ");
                // TEXT/ASCII/TYPE340 right children come from labelTrailer
                // matching a text directive on the same line as the label.
                // They cannot be passed to emitOperand, so dispatch to the
                // appropriate text-emit function instead.
                switch( nodeP->rightP->type )
                {
                case TEXT:
                case TYPE340:
                    emitText(outfP, nodeP->rightP->value.flexText);
                    break;

                case ASCII:
                    emitAscii(outfP, nodeP->rightP->value.strP);
                    break;

                default:
                    emitOperand(outfP, nodeP->rightP);
                    break;
                }
            }
            break;

        case LCLLOCATION:
            if( nodeP->rightP )
            {
                macPrintf(outfP, "    ");
                // Same TEXT/ASCII/TYPE340 handling as LOCATION above.
                switch( nodeP->rightP->type )
                {
                case TEXT:
                case TYPE340:
                    emitText(outfP, nodeP->rightP->value.flexText);
                    break;

                case ASCII:
                    emitAscii(outfP, nodeP->rightP->value.strP);
                    break;

                default:
                    emitOperand(outfP, nodeP->rightP);
                    break;
                }
            }
            break;

        case VARS:
            macPrintf(outfP,"/ variables\n");
            emitVars(outfP, (PNodeListP)(nodeP->value.ptr));
            break;

        case BANK:
            if( doWarn(WARN_BANK) )
            {
                fprintf(stderr,"am1: WARNING: banks are not supported by macro1!\n");
            }

            macPrintf(outfP,"/ BANK - following is in bank %d\n", nodeP->value.ival );
            noNl = true;
            break;

        case OPTIMIZE:
        case ENDOPTIMIZE:
            // An optimizer directive prints nothing, and noNl drops its line's
            // terminator, so the output is that of the source without it.
            noNl = true;
            break;

        case CONSTANTS:
            macPrintf(outfP,"/ constants\n");
            emitConstants(outfP, nodeP->value.symP);
            break;

        case TEXT:
            macPrintf(outfP, "/ Text table\n");
            emitText(outfP, nodeP->value.flexText);
            macPrintf(outfP, "/ End\n");
            noNl = true;
            break;

        case ASCII:
            macPrintf(outfP, "/ Ascii table\n");
            emitAscii(outfP, nodeP->value.strP);
            macPrintf(outfP, "/ End\n");
            noNl = true;
            break;

        case TYPE340:
            macPrintf(outfP, "/ Type 340 character table\n");
            emitText(outfP, nodeP->value.flexText);
            macPrintf(outfP, "/ End\n");
            noNl = true;
            break;

        case TABLE:
            macPrintf(outfP, "/ Data table\n");

            if( nodeP->rightP )     // has initializer
            {
                j = evalExpr(nodeP->rightP);

                for( i = 0; i < nodeP->value.ival; ++i )
                {
                    nodeP->pc++;
                    macPrintf(outfP, "    %o\n", j);
                }
            }
            else
            {
                macPrintf(outfP, ".+%o/\n", nodeP->value.ival);
            }

            macPrintf(outfP, "/ End\n");
            noNl = true;
            break;

        case IMPORT:
            if( *(nodeP->value.strP) != '<' )
            {
                macPrintf(outfP, "/ import \"%s\"\n", nodeP->value.strP);
            }
            else
            {
                macPrintf(outfP, "/ import %s\n", nodeP->value.strP);
            }
            break;

        case EXPORT:
            macPrintf(outfP, "/ export ");
            for( node2P = nodeP->rightP; node2P; node2P = node2P->leftP )
            {
                cP = (node2P->type == NAME)?node2P->value.strP:node2P->value.symP->name;
                macPrintf(outfP, "%s%s", cP, (node2P->leftP)?", ":"\n");
            }
            break;

        case OVERLAYDECL:
        case ENDOVERLAY:
        case OVTABLE:
        case OVCALL:
            emitOverlayDirective(outfP, nodeP);
            noNl = true;
            break;

        case ASSERT:
            // macro1 has no such statement: the line, as a comment.  Under -A
            // nothing is written, as for the source without the line.
            if( !ignoreAsserts )
            {
                cP = assertText(nodeP);
                macPrintf(outfP, "/ %s\n", cP);
                free(cP);
            }

            noNl = true;
            break;

        case SEMI:
            macPrintf(outfP, "\n");
            break;

        case EMPTYLINE:
            // A constant reference left unclosed ("lac [5") is closed by the
            // newline, which the lexer hands back as an empty line, so no
            // TERMINATOR ended the statement's line: end it here, unless the constant's
            // trailing comment already has.
            if( prevP && (macColumn != 0) && ((prevP->type == EXPR) ||
                          (((prevP->type == LOCATION) || (prevP->type == LCLLOCATION) ||
                            (prevP->type == ORIGIN)) && prevP->rightP)) )
            {
                macPrintf(outfP, "\n");
            }

            noNl = false;
            break;

        case TERMINATOR:
            if( !noNl )
            {
                macPrintf(outfP, "\n");
            }

            noNl = false;
            break;

        default:
            // just ignore
            noNl = true;
            break;
        }

        prevP = nodeP;
        nodeP = nodeP->leftP;
    }
}

// Very similar to evalExpr(), but doesn't do full reduction
static void
emitOperand(FILE *outfP, PNodeP nodeP)
{
int lval;
int rval;
char ch;
SymNodeP symP;

    if( !nodeP )
    {
        return;
    }

    switch( nodeP->type )
    {
    case BINOP:
        // We don't do the illegal law checks, macro didn't.
        if( nodeP->leftP->type == LAW )
        {
            rval = onesComplAdj(evalExpr(nodeP->rightP));
            if( rval & 010000 )
            {
                macPrintf(outfP,"law i %o", rval & ~010000);
            }
            else
            {
                macPrintf(outfP,"law %o", rval);
            }
        }
        else if( nodeP->value.ival == XOR )
        {
            // there is no xor in macro1, have to reduce everything
            lval = onesComplAdj(evalExpr(nodeP->leftP));
            rval = onesComplAdj(evalExpr(nodeP->rightP));
            macPrintf(outfP,"%o", (lval ^ rval) & WRDMASK);
        }
        else if( (nodeP->value.ival == DIV) || (nodeP->value.ival == MOD) )
        {
            // there is no divide or mod in macro1, have to reduce everything.
            // The value is the one the tape gets, its errors included.
            macPrintf(outfP,"%o", evalExpr(nodeP));
        }
        else if( nodeP->value.ival == LSHIFT )
        {
            // there is no << in macro1, have to reduce everything
            lval = onesComplAdj(evalExpr(nodeP->leftP));
            rval = onesComplAdj(evalExpr(nodeP->rightP));
            macPrintf(outfP,"%o", (lval << rval) & WRDMASK);
        }
        else if( nodeP->value.ival == RSHIFT )
        {
            // there is no >> in macro1, have to reduce everything
            lval = onesComplAdj(evalExpr(nodeP->leftP));
            rval = onesComplAdj(evalExpr(nodeP->rightP));
            macPrintf(outfP,"%o", (lval >> rval) & WRDMASK);
        }
        else
        {
            emitOperand(outfP, nodeP->leftP);

            switch( nodeP->value.ival )
            {
            case PLUS:
                ch = '+';
                break;
            case MINUS:
                ch = '-';
                break;
            case MUL:
                ch = '*';
                break;
            case AND:
                ch = ' ';
                break;
            case OR:
                ch = '!';
                break;
            case SEPARATOR:
                ch = ' ';
                break;
            default:
                verror("unknown binary op %d in emitOperand", nodeP->value.ival);
                // never returns, just to shut up overly-picky c compilers
                return;
            }

            macPrintf(outfP,"%c", ch);
            emitOperand(outfP, nodeP->rightP);
        }
        break;

    case UNOP:
        switch( nodeP->value.ival )
        {
        case PARENS:
            emitOperand(outfP, nodeP->rightP);
            break;
        case UMINUS:
            rval = onesComplAdj(evalExpr(nodeP));
            macPrintf(outfP, "%o", (rval) & WRDMASK);
            break;
        case CMPL:
            // there is no complement in macro1, have to reduce
            rval = onesComplAdj(evalExpr(nodeP));
            macPrintf(outfP, "%o", (rval) & WRDMASK);
            break;
        default:
            verror("unknown unary op %d in emitOperand", nodeP->value.ival);
        }
        break;

    case CONSTANT:
        macPrintf(outfP,"%04o",nodeP->value.symP->value);
        if( nodeP->rightP )
        {
            emitCommentText(outfP, nodeP->rightP->value.strP, false);
        }
        break;

    case DOT:
        macPrintf(outfP,".");
        break;

    case OPORABLE:
    case OPCODE:
    case OPADDR:
        macPrintf(outfP,"%s", nodeP->value.symP->name );
        break;

    case BREF:
        // The binary back end refuses an undefined name:N the same way (eval.c), and
        // -m alone never runs it.
        symP = nodeP->value.symP;
        if( !(symP->flags & SYMF_RESOLVED) )
        {
            verrorlf(nodeP->srcLine, nodeP->srcFileP, "symbol %s in bank %d has no defined value",
                symP->name, symP->bank);
        }

        if( doWarn(WARN_BREF) )
        {
            fprintf(stderr,"am1: WARNING: bank references are not supported by macro1!\n");
        }

        macPrintf(outfP,"BREF %s:%d", nodeP->value.symP->name, nodeP->value2.ival );
        break;

    case LCLADDR:
        // local addrs are relative to the current location unless they were resolved as global
        symP = nodeP->value.symP;
        if( symP->flags & SYMF_RESOLVED )
        {
            if( (symP->flags & SYM_MASK) == SYM_GLOB )
            {
                macPrintf(outfP, "%s", symP->name);
            }
            else
            {
                lval = symP->value;
                rval = nodeP->pc;
                if( lval >= rval )
                {
                    macPrintf(outfP, ".+%o", lval - rval );
                }
                else
                {
                    macPrintf(outfP, ".-%o", rval - lval );
                }
            }
            break;
        }
        else
        {
            verrorlf(nodeP->srcLine, nodeP->srcFileP, "local symbol %s has no defined value", symP->name);
        }
        break;

    case ADDR:
        symP = nodeP->value.symP;
        if( symP->flags & SYMF_RESOLVED )
        {
            macPrintf(outfP, "%s", symP->name );
        }
        else
        {
            verrorlf(nodeP->srcLine, nodeP->srcFileP, "symbol %s has no defined value", symP->name);
        }
        break;

    case LITCHAR:
    case CHAR:
    case FLEXO:
        macPrintf(outfP, "%06o", nodeP->value.ival & WRDMASK);
        break;

    case INTEGER:
        macPrintf(outfP, "%o", nodeP->value.ival & WRDMASK);
        break;

    case VALUESPEC:
    case IMOD:
        macPrintf(outfP, "%s", nodeP->value.symP->name);
        break;

    case LOCAL:
    case PRIVATE:
    case ADDLOCAL:
    case ENDLOC:
    case FORCELOC:
        break;      // ignore

    case TERMINATOR:
        macPrintf(outfP, "\n");
        break;

    default:
        verror("unknown op %d in emitOperand", nodeP->type);
    }
}

// Emit packed ascii
static void
emitAscii(FILE *outfP, char *strP)
{
int i;

    for( i = 0; *strP != 0; i ^= 1 )
    {
        if( !i )
        {
            macPrintf(outfP, "    ");
        }

        macPrintf(outfP, "%03o", *strP++);

        if( i )
        {
            macPrintf(outfP, "\n");
        }
    }

    if( i )
    {
        macPrintf(outfP, "000\n");
    }
    else
    {
        macPrintf(outfP, "    000000\n");
    }
}

// Emit packed flexo code
static void
emitText(FILE *outfP, FlexText flexText)
{
int i;
int val;
char *bufP;

    bufP = flexText.bufP;

    for( val = i = 0; i < flexText.nchars; i++ )
    {
        if( i && !(i % 3) )
        {
            macPrintf(outfP, "    %06o\n", val);
            val = 0;
        }

        val <<= 6;
        val |= *bufP++;
    }

    if( i % 3 )     // had leftovers, finish the word
    {
        while( i++ % 3 )
        {
            val <<= 6;
        }
        macPrintf(outfP, "    %06o\n", val);
    }
    else if( (i >= flexText.nchars) && !( i % 3) )
    {
        macPrintf(outfP, "    %06o\n", val);      // didn't emit the word yet
    }
}

// Walk a list of variables, emit the storage
static void
emitVars(FILE *fP, PNodeListP listP)
{
PNodeP nodeP;
SymNodeP symP;

    while( listP )
    {
        nodeP = listP->nodeP;
        symP = nodeP->value.symP;
        macPrintf(fP,"%s, %06o\n", symP->name, (nodeP->leftP)?evalExpr(nodeP->leftP):0);

        listP = listP->nextP;
    }
}

// An overlay directive, annotated as a bank is: macro1 has no overlays, so the
// result is not runnable.  An overlay is given an origin at its first word, and
// the root another after it; an %%ovcall's or %%ovload's words are emitted as they are.
static void
emitOverlayDirective(FILE *outfP, PNodeP nodeP)
{
int i;
OverlayContextP ovP;
PNodeP node2P;

    switch( nodeP->type )
    {
    case OVERLAYDECL:
        ovP = (OverlayContextP)(nodeP->value.ptr);
        macPrintf(outfP, "/ OVERLAY %s - following runs in window %s, bank %d, loaded from the drum\n",
            ovP->nameP, ovP->windowP->nameP, ovP->windowP->bank);
        macPrintf(outfP, "%o/\n", ovP->windowP->first + OVHEADERWORDS);
        break;

    case ENDOVERLAY:
        ovP = (OverlayContextP)(nodeP->value.ptr);

        if( ovP->ctx.constSymP )
        {
            macPrintf(outfP, "/ Constants for overlay %s\n", ovP->nameP);
            emitConstants(outfP, ovP->ctx.constSymP);
        }

        if( ovP->ctx.varNodesP )
        {
            macPrintf(outfP, "/ Vars for overlay %s\n", ovP->nameP);
            emitVars(outfP, ovP->ctx.varNodesP);
        }

        macPrintf(outfP, "/ END OVERLAY %s\n", ovP->nameP);

        // The first root node after it has the root's pc.
        for( node2P = nodeP->leftP; node2P && node2P->overlay; node2P = node2P->leftP )
        {
            ;
        }

        if( node2P )
        {
            macPrintf(outfP, "%o/\n", node2P->pc);
        }
        break;

    case OVTABLE:
        macPrintf(outfP, "/ Overlay descriptor table, ovTable\n");

        for( i = 0; i < nodeP->value.ival; ++i )
        {
            macPrintf(outfP, "    %o\n", ovTableWordsP[i]);
        }
        break;

    case OVCALL:
        macPrintf(outfP, "/ %%%%%s %s%s%s\n", OVCALLNAME(nodeP), nodeP->value.strP, OVCALLSEP(nodeP),
            OVCALLENTRY(nodeP));

        for( node2P = nodeP->rightP; node2P; node2P = node2P->leftP )
        {
            macPrintf(outfP, "    ");
            emitOperand(outfP, node2P->rightP);
            macPrintf(outfP, "\n");
        }
        break;

    default:
        break;
    }
}

// Walk a symbol table of constants, emit the values
static int
emitConstants(FILE *fP, SymNodeP symP)
{
int val;

    if( !symP )
    {
        return(0);
    }

    macPrintf(fP,"    %06o\n", symP->value2 & 0777777);

    val = emitConstants(fP, symP->leftP);
    val += emitConstants(fP, symP->rightP);

    return(val);
}

// Write a comment node's text, its leading and trailing blanks dropped.  The text is the
// listing's too, so it is only read.  A comment that began its source line begins its own
// line here too; any other trails what is already on the line.
static void
emitCommentText(FILE *outfP, const char *strP, bool ownLine)
{
int len;

    while( isspace((unsigned char)*strP) )
    {
        ++strP;
    }

    len = strlen(strP);
    while( (len > 0) && isspace((unsigned char)strP[len - 1]) )
    {
        --len;
    }

    if( ownLine && (macColumn != 0) )
    {
        macPrintf(outfP, "\n");
    }

    emitComment(outfP, strP, len, (macColumn != 0));
}

// Write a comment of len characters as "/ " lines no wider than MACLINEWIDTH, broken at
// blanks, and inside a word only when the word is wider than a line.  A trailing comment
// starts on its statement's line, after a tab, when its first word fits there.
static void
emitComment(FILE *outfP, const char *textP, int len, bool trailing)
{
const char *endP;
const char *lineEndP;
const char *cP;
int col;

    endP = (textP + len);

    if( trailing )
    {
        // the column the first word would end in, after the tab and the "/ "
        col = nextColumn(macColumn, '\t') + 2;
        for( cP = textP; (cP < endP) && !isblank((unsigned char)*cP); ++cP )
        {
            col = nextColumn(col, *cP);
        }

        if( col <= MACLINEWIDTH )
        {
            macPrintf(outfP, "\t");     // macro1 needs a tab before a trailing comment
        }
        else
        {
            macPrintf(outfP, "\n");
        }
    }

    do
    {
        macPrintf(outfP, "/ ");
        lineEndP = commentLineEnd(textP, endP, macColumn);
        macPrintf(outfP, "%.*s\n", (int)(lineEndP - textP), textP);
        textP = lineEndP;

        // A break at a blank takes that one blank, so joining the lines with one blank
        // gives back the comment as written.
        if( (textP < endP) && isblank((unsigned char)*textP) )
        {
            ++textP;
        }
    } while( textP < endP );
}

// Write a block comment's lines as "/ " comments, the "/*" and each line's leading and
// trailing blanks dropped.  Its first and last lines are left out when nothing else is on
// them, so "/*" and "*/" alone on their lines write nothing.
static void
emitBlockComment(FILE *outfP, const char *textP)
{
const char *lineP;
const char *nlP;
const char *endP;
const char *cP;
bool first;
bool last;

    if( macColumn != 0 )
    {
        macPrintf(outfP, "\n");
    }

    first = true;
    for( lineP = textP; ; lineP = (nlP + 1) )
    {
        // the text holds the lines up to, not including, the "*/"
        nlP = strchr(lineP, '\n');
        last = (nlP == NULL);
        endP = (last)? (lineP + strlen(lineP)): nlP;

        for( cP = lineP; (cP < endP) && isspace((unsigned char)*cP); ++cP )
        {
            ;
        }

        if( first && (strncmp(cP, "/*", 2) == 0) )
        {
            for( cP += 2; (cP < endP) && isspace((unsigned char)*cP); ++cP )
            {
                ;
            }
        }

        while( (endP > cP) && isspace((unsigned char)endP[-1]) )
        {
            --endP;
        }

        if( (cP < endP) || !(first || last) )
        {
            emitComment(outfP, cP, (int)(endP - cP), false);
        }

        if( last )
        {
            break;
        }

        first = false;
    }
}

// The end of the part of a comment, from textP, that fits on a line whose text starts in
// column col: the last blank before the line would be too wide, or, with no blank, the
// last character that fits.  At least one character is taken, so a comment always ends.
static const char *
commentLineEnd(const char *textP, const char *endP, int col)
{
const char *cP;
const char *breakP;

    breakP = NULL;
    for( cP = textP; cP < endP; ++cP )
    {
        if( isblank((unsigned char)*cP) && (cP > textP) )
        {
            breakP = cP;
        }

        col = nextColumn(col, *cP);
        if( col > MACLINEWIDTH )
        {
            if( breakP )
            {
                return(breakP);
            }

            return((cP > textP)? cP: (cP + 1));
        }
    }

    return(endP);
}

// The column after ch is written in column col.
static int
nextColumn(int col, char ch)
{
    if( ch == '\n' )
    {
        return(0);
    }

    if( ch == '\t' )
    {
        return(((col / MACTABSTOP) + 1) * MACTABSTOP);
    }

    return(col + 1);
}

// fprintf to the .mac, keeping macColumn, which a comment's wrapping starts from.
static void
macPrintf(FILE *outfP, const char *fmtP, ...)
{
va_list args;
va_list argsCopy;
char buf[256];
char *textP;
char *cP;
int len;

    va_start(args, fmtP);
    va_copy(argsCopy, args);
    len = vsnprintf(buf, sizeof(buf), fmtP, args);
    textP = buf;

    if( (len >= (int)sizeof(buf)) && (textP = malloc(len + 1)) )
    {
        vsnprintf(textP, (len + 1), fmtP, argsCopy);
    }

    va_end(argsCopy);
    va_end(args);

    if( (len < 0) || !textP )
    {
        verror("can't write the macro1 source");
        return;
    }

    for( cP = textP; *cP; ++cP )
    {
        macColumn = nextColumn(macColumn, *cP);
    }

    fputs(textP, outfP);

    if( textP != buf )
    {
        free(textP);
    }
}
