/* am1.c - another macro1 assembler
 *
 * Usage: am1 [-abdlmMnNrsSTvz[xykp]] [-O[1|2]] [-O=modifier]... [-i path] [-Dsymbol[=value]]... [-W[=warning]] ... [-I path]... sourcefile
 *
 * Valid switches are:
 *
 * -a	treat space in expressions as add, not or
 * -b	generate binary code
 * -d	same as giving both -s and -l, generates all the files needed for ad1
 * -l	generate a listing
 * -m	generate macro1 source
 * -M	memory overwrite by code is a warning, not a fatal error
 * -n	don't run cpp on input source
 * -N	don't keep any contents from include files in the listing
 * -O   run the optimizer advisor, write its report to sourcefile.opt
 * -O=dump
 *      as -O, and also print the optimizer's word table on stdout in -T format
 * -O=decode
 *      as -O, and also print the optimizer's decoded word table on stdout
 * -O=refs
 *      as -O, and also print the optimizer's reference edges and word flags on stdout
 * -O=flow
 *      as -O, and also print the optimizer's basic blocks and reachability on stdout
 * -O=calls
 *      as -O, and also print the optimizer's routines and the call graph on stdout
 * -O=share
 *      as -O, and also print the optimizer's return-word sharing assignment on stdout
 * -O=regions
 *      as -O, and also print the optimizer's %%optimize/%%endoptimize regions on stdout
 * -O=rules
 *      as -O, and also print the optimizer's findings on stdout, one per line
 * -O=xform
 *      as -O, and also print on stdout what -O1 or -O2 does with each live finding;
 *      without either it is a dry run of the one-word-for-one-word rewrites alone,
 *      and rewrites nothing
 * -O=source
 *      as -O, and also write the program, as -O1 or -O2 left it, as am1 source to
 *      sourcefile.opt.am1 in the source's directory.  A line the optimizer did not
 *      change is copied from the source, so the file is assembled with the same -D
 *      and -I as the source was
 * -O=values
 *      as -O, and also print on stdout what each basic block already knows about AC
 *      and IO: a measurement, with no finding and nothing added to the report
 * -O=scratch
 *      as -O, and also print on stdout whether the storage locals could share words:
 *      a measurement, with no finding and nothing added to the report
 * -O=guess
 *      as -O, and also print on stdout the heuristics -O2 would use in place of P5
 *      and the findings each would refuse: a measurement, which refuses nothing
 * -O=speed
 *      as -O, and also print on stdout the speed-mode candidates (inlining, unrolling,
 *      T3 chains, fall-through placement), their sites and what each would save and
 *      spend: a measurement, which rewrites nothing and adds nothing to the report
 * -O=relayout
 *      as -O, and lay the program out again after the -O=edits edits, or with none,
 *      and print on stdout what happened: the segments, each edit, the address map,
 *      the pool slots the rebuilt pools gained
 * -O=window
 *      as -O, and also print on stdout the extend-window analysis: every eem and lem
 *      as redundant, dead, freed, needed or refused, and every hazard, an indirect
 *      reference made with the window possibly open through a pointer not proved
 *      to hold a 16-bit address.  Advisory; proved hazards are warned on stderr
 *      at every -O run
 * -O=check
 *      as -O, and run the optimizer's decoder self-check first, on stdout
 * -O1  as -O, and also rewrite, only where the source declares it may.  Inside an
 *      %%optimize/%%endoptimize region: the one-word-for-one-word transforms (T3,
 *      T8a-d), space mode's deletions, the eem/lem deletions, fall-through placement
 *      and loop unrolling, with the pool words they free reclaimed.  Inside a
 *      %%speed/%%endspeed region, or for a routine named by %%inline: inline
 *      expansion.  A source with none of these assembles exactly what -O does
 * -O2  as -O1, and also make the one-word-for-one-word transforms with no region
 *      needed: in place of P5 it guesses, by heuristics H1 to H5, where code is
 *      shared with a device, a handler or a timing requirement, and leaves those
 *      findings alone.  It warns on stderr that the guess can be wrong.  A rewrite
 *      that changes the program's length still needs a declaration, or
 *      -O=undeclared, which licenses the deletions, the pool reclaim, the
 *      extend-window deletions and fall-through placement on the guess.
 *      Words inside a %%nooptimize/%%endnooptimize span are never rewritten at any level.
 *      There is no other level: -O2s, -O2t and -O3 and above are refused, since
 *      -O1 and -O2 make the space and speed rewrites themselves, inside declarations
 * -O=upto=N
 * -O=range=B:LO-HI
 * -O=off=FILE
 *      with -O1 or -O2 only, for bisection: keep only the first N rewrites in -O=xform's
 *      order; only those in bank B (decimal), addresses LO to HI (octal), repeatable;
 *      every rewrite but those FILE lists, one "bank addr" per line, repeatable.  A
 *      rewrite stays on only if every one given keeps it
 * -O=edits=FILE
 *      a testing instrument: make the edits FILE lists, one per line,
 *      "delete BANK ADDR", "move BANK FROM TO after ADDR" or "copy BANK FROM TO over
 *      ADDR", the bank decimal and the
 *      addresses octal as assembled, and lay the program out again.  An edit relayout
 *      cannot show safe is refused, and -O=relayout names the reason.  Repeatable
 * -r	don't write a loader at the beginning of a tape
 * -s	generate a symbol table file
 * -S	print a summary of per-bank highest address used
 * -T   special mode for testing, output the generated binary as ascii octal numbers
 * -v   print the current version number and exit
 * -z   don't convert 1's complement -0 to +0 in math operations
 *
 * -W	print all warnings
 * -W=[-]warning
 *      enable just the given warning, or with a leading -, disable it (useful with -W),
 *      see the documentation; can be repeated as needed
 * -i path
 *	set the root for all includes not specified by -I
 *	also accepts -ipath syntax
 * -D symbol=value
 *	add a #define, also accepts -Dsym.. syntax
 * -I path
 *	add a directory to the cpp #include search list
 *	also accepts -Ipath syntax
 *
 * The following are for debugging, not of general use.
 *
 * -x   enable (f)lex debugging output on stderr
 * -y   enable yacc (bison) debugging output on stderr
 * -k   keep the intermediate cpp file
 * -p   dump the parse tree in readable form on stdout
 *
 * The resulting code will be assembled.
 * If neither -b nor -m are given, -b is assumed.
 *
 * If successful, one or more result files are created.
 *
 * sourcefile.mac is the macro1 source output file
 * sourcefile.rim is the loadable executable file in rim/bin format
 * sourcefile.lst is the listing output file
 * sourcefile.cpp is the cpp output file
 * sourcefile.sym is the symbol table output file
 *
 * The environment variable 'AM1INCDIR' overrides the default system
 * include directory, which is overridden itself by -i.
 * If not set, it defaults to the predefined AM1INCDIR in am1.h.
 *
 * If -r, no rim loader, is used, the binary output file will have a .bin extension, not .rim.
 * Original author: Bill Ezell (wje) pdp1@quackers.net
 * Free to use for any purpose as long as attribution is kept.
 *
 * Revision history:
 *
 * 18-Dec-2025 wje - initial version
 * 02-Jan-2026 wje - 'production' release
 * 04-Jan-2026 wje - added tables, masking of math results to 18 bits
 * 05-Jan-2026 wje - added -z, -a, cleanup, doc updates
 * 06-Jan-2026 wje - added -s and symbol table output
 * 06-Jan-2026 wje - final code cleanup, make bank use vs no use consistent instead of no use being a special case
 * 08-Jan-2026 wje - clean up line numbering in listing, add bank ref wildcard, a:*
 * 12-Jan-2026 wje - more listing cleanup, rename pause to stop, add import/export, add lshift and rshift,
 *                   make opr precedence same as C, update docs
 * 14-Jan-2026 wje - fix serious bug introduced by the mod operator, can't use a percent
 * 20-Jan-2026 wje - fix bug in forced locals, rename mod to %%
 * 22-Jan-2026 wje - add ioh, same as iot i
 * 23-Jan-2026 wje - fix bug in constants stmt, would clear pc if no constants to emit, fix text stmt
 * 03-Feb-2026 wje - add C, dpyc, sdb
 * 04-Feb-2026 wje - predefine AM1 as an ifdef marker for include files, not defined if macro is being produced
 * 06-Feb-2026 wje - add -r flag
 * 12-Feb-2026 wje - fix a case of duplicate symbols across banks not resolving correctly
 * 15-Feb-2026 wje - addlocal directive added, allow nested local to have same name as outer local
 * 16-Feb-2026 wje - just some warnings about locals hiding other locals or globals
 * 18-Feb-2026 wje - fix command line parsing, add -W=xxx error control, add line number and version to symbol file
 * 26-Feb-2026 wje - use extended addresses, bank and pc, in listings
 * 05-Mar-2026 wje - change import to use V2 symtabs, set correct max bank, 16 not 32
 * 08-Mar-2026 wje - fix obscure issue with a symbol being used with and without a bank ref in constants
 * 09-Mar-2026 wje - change constant hash to be sure the last fix returns a 64 bit hash, not a 32 bit hash
 * 10-Mar-2026 wje - change constant hash again, if it ain't broke, fix it anyway just to hash
 *                   resolved symbol values better.
 * 17-Mar-2026 wje - general cleanup, eliminate empty lines in listing for clarity, make -0 to 0 conversion the default
 * 28-Mar-2026 wje - add code passing 4K boundary, code overwriting other code checks and error msgs
 * 02-Apr-2026 wje - fix bug in mem check bitmap, make overwrite by code selectable warning/failure
 * 07-Apr-2026 wje - finally figured out how to keep cpp from inserting line info markers, can now use std cpp
 * 08-Apr-2026 wje - fix some formatting issue with trailing comments in macro output
 * 12-Apr-2026 wje - minor change, turn off cpp warning about unterminated quotes in flex text strings
 * 26-Apr-2026 wje - add c-style block comments
 * 26-Apr-2026 wje - add -N to suppress included file text in listing
 * 12-Jun-2026 wje - fix incorrect opcode for cmi, should be 770000
 * 20-Jun-2026 wje - add special sequences in flexo and text for ribbon color and carriage return
 * 21-Jun-2026 wje - labeled location, label and text/ascii/type340 now allowed on same line
 * 24-Jun-2026 wje - fix WARN_BANK/WARN_BANKS name swap in warnings[] table
 * 25-Jun-2026 wje - swapBanks refactor, replace cur_pc, globalSymP, constSymP, varNodesP with
 *                   BankContextP curBankP->x references, it's just cleaner code
 * 26-Jun-2026 wje - add test output mode
 * 26-Jun-2026 wje - adjust type340 nl to just insert a cr, the t340 emulator changed, it does cr/lf for cr
 * 30-Jun-2026 wje - add thisbank keyword
 * 13-Jul-2026 wje - add private keyword
 * 19-Aug-2026 wje - fix use of wrong pc when emitting an automatically-emitted constant in multibank use
 * 28-Aug-2026 fab - make output deterministic using a serial number instead of a memory address for hashing
 * 30-Aug-2026 wje - add -S, memory usage summary
 * 31-Aug-2026 wje - replace accidentally deleted lex pattern for DOTEXT
 * 3-Sep-2026 wje - just some formatting cleanup, no code change, no version change
 * 3-Sep-2026 wje - add out-of-bounds warning for law -n or law (i) > 07777
 * 5-Sep-2026 wje - more law cases handled, all should be covered now
 * 5-Sep-2026 wje - various fixes for dangling constants and vars not being emitted correctly
 * 6-Sep-2026 wje - minor fix in listcodegen to fix some constants being listed incorrectly
 * 6-Sep-2026 wje - trivial change, show mem usage in sorted low bank to high bank order
 * 8-Sep-2026 wje - change symtab version number, fix testcodegen to handle text, ascii, type340
 * 8-Sep-2026 claude - add -O, the optimizer advisor, see Docs/UsingTheAm1Optimizer.md.
 * 9-Sep-2026 wje - fixes (finally) for line numbers sometimes being off by one in error messages, fix some of the
 *    directives, e.g. table, not allowing use of location 07777
 * 9-Sep-2026 claude - -O=calls and -O=share, the call-graph and return-word dumps.
 * 14-Sep-2026 claude - -O1 and its -O=xform dump, -O=values, and typo fixes.
 * 14-Sep-2026 wje - clean up usage and explicitly initialize doMacro and doBinary
 * 15-Sep-2026 claude - -O=scratch, and fixes to hashExpr and resolveWildcard.
 * 16-Sep-2026 claude - -O=guess, and -O2, the heuristic level; -O2s and -O2t refused.
 * 17-Sep-2026 claude - -O=speed, the bisection modifiers, -O=relayout and -O=edits.
 * 18-Sep-2026 claude - -O=place=undeclared, needing -O2, and the -O=inline usage lines.
 * 19-Sep-2026 wje - rework lexer, % is now mod like it should be, %% is a directive, the old use of % in locals is gone
 * 19-Sep-2026 claude - -O=unroll=cap usage, -Wall -Wextra warnings cleared, and the %%ceiling directive.
 * 20-Sep-2026 claude - usage text for -O=reclaim=off.
 * 21-Sep-2026 wje - and finally fix xxx/ stmt for an origin statement
 * 21-Sep-2026 wje - some edge-case fixes in eval.c and in type340chars
 * 21-Sep-2026 claude - -O=window usage
 * 21-Sep-2026 claude - usage text for -O1, -O2, -O=xform and -O=edits says what they do now
 * 21-Sep-2026 claude - -O=source, the program written back out as am1 source
 * 22-Sep-2026 claude - -O=source copies unchanged lines; usage for -O=source=render and -O=source=tree
 * 25-Sep-2026 claude - %% followed by a name that is no directive is diagnosed by name again
 * 25-Sep-2026 claude - v3.0, AM1VERSION and AM1SHORTVERSION move from v1.50 to v3.0
 * 29-Sep-2026 claude - -O=undeclared, needing -O2, with -O=place=undeclared its alias
 * 29-Sep-2026 wje - minor fix, line number one off in error message for cross-bank ref to nonexistent symbol 
*/
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>

#include "am1.h"
#include "symtab.h"
#include "optimizer.h"

typedef struct inc_item
{
    struct inc_item *nextP;
    char *incP;
    char type;                  // I, D, etc
} Inc_item, *Inc_itemP;

// Define the various warnings that can be enabled and disabled.
// Flags are enabled, repeats, has been issued
Warning warnings[] = {
    {"1Dop", WARN_1D, false, true, false},
    {"banks", WARN_BANKS, false, false, false},
    {"locals", WARN_LOCALS, false, true, false},
    {"flex", WARN_FLEX, false, true, false},
    {"vars", WARN_VARS, false, true, false},
    {"stop", WARN_STOP, false, false, false},
    {"bank", WARN_BANK, false, false, false},
    {"bref", WARN_BREF, false, false, false},
    {"memory", WARN_MEMORY, false, true, false},
    {"law", WARN_LAW, true, true, false},
    {"type340", WARN_T340, true, true, false},
    {"", 0, false, false, false}  // end marker
};

Inc_itemP incsP;                // the list of cpp stuff
FILE *outfP;                    // where we put our code

char *origFilenameP;            // command line input am1 file
char *filenameP;                // current input am1 file
char pfilename[128];            // cpp tmp file name
char ofilename[1024];           // output file; -O=source's carries the source's directory
char basename[64];              // base name
char incroot[256];              // root of includes
char str1[256];                 // scratch strings
char str2[256];

bool doMacro;
bool doBinary;                  // a misnaming, it actually means 'full am1 syntax, am1 loader in rim'
bool keepMinusZero;
bool spaceIsAdd;
bool doListing;
bool doSymtab;
bool doCpp;
bool noRim;
bool keepCpp;
bool dumpTree;
bool sawBank;
bool noWarn;
bool noMemFatal;
bool dropIncludeText;
bool showMemUsage;
bool doOptimize;                // -O, run the optimizer advisor
int lineno;

extern int yydebug;
extern int yy_flex_debug;

PNodeP rootP;                   // root of the parse tree
PNodeListP wildcardsP;          // any wildcarded cross-bank refs
SymNodeP localSymP;             // local addresses
SymListP constsListP;           // the list of all constant groups

extern BankContextP banksP;
extern BankContextP curBankP;
extern void initParser(void);
extern FILE *yyin;              // lex input file
extern int yyparse();

int run_cpp(char *, char *);
int usage();
bool doWarn(int warnID);
void add_cpp(char, char *);
bool fileIsMain(char *nameP);
void enableAllWarnings(void);
void enableWarning(char *nameP);
void leave(int);
void dumpParseTree(PNodeP);
void dumpExpr(PNodeP);

extern int evalExpr(PNodeP);
extern int macCodegen(FILE *, PNodeP);
extern int binCodegen(FILE *, PNodeP);
extern int listCodegen(FILE *, PNodeP);
extern int testCodegen(FILE *, PNodeP);
extern BankContextP findBank(int bankNo);
extern void listSymtab(FILE *outfP, char* filenameP, BankContextP banksP);
extern void vwarn(int errtype, const char *msgP, ...);

int
main(int argc, char **argv)
{
int i;
bool testMode;
char *cP;
BankContextP bankP, lastBankP;

    yydebug = 0;
    yy_flex_debug = 0;
    keepMinusZero = false;
    spaceIsAdd = false;
    noWarn = true;
    dropIncludeText = false;
    showMemUsage = false;
    testMode = false;
    doMacro = doBinary = false;

    for(i = 1; i < NSIG;)
    {
        if(signal(i++, leave) == SIG_IGN)
        {
            /* catch all sigs */
            signal(i - 1, SIG_IGN);                       /* unless ignored */
        }
    }

    signal(SIGCHLD, SIG_DFL);                             /* special case */

    doCpp = true;

    /* do the command line processing */
    ++argv;
    --argc;

    while(argc && (**argv == '-'))                        /* look for directives */
    {
        for(cP = *argv + 1; *cP;)
        {
            switch(*cP++)
            {
            case 'a':
                spaceIsAdd = true;
                break;

            case 'k':
                keepCpp = true;
                break;

            case 'b':
                doBinary = true;
                break;

            case 'd':
                doListing = true;
                doSymtab = true;
                break;

            case 'i':                                       /* accept either ixxx or i xxx */
                if( !*cP)
                {
                    --argc;
                    ++argv;
                    cP = *argv;
                }

                strcpy(incroot, cP);
                cP = "";
                break;

            case 'l':
                doListing = true;
                break;

            case 'm':
                doMacro = true;
                break;

            case 'M':
                noMemFatal = true;
                break;

            case 'n':
                doCpp = false;
                break;

            case 'N':
                dropIncludeText = true;
                break;

            case 'O':                                       /* -O, -O1, -O2 or -O=modifier */
                doOptimize = true;
                if( *cP == '1' )
                {
                    optimizeSetLevel(1);                    // -O1 implies -O: a run that rewrites
                    ++cP;                                   // the program writes the report saying so
                }
                else if( *cP == '2' )
                {
                    // -O2s and -O2t, space and speed spellings, are not built;
                    // without this -O2s would parse as -O2 and -s, silently.
                    if( (cP[1] == 's') || (cP[1] == 't') )
                    {
                        fprintf(stderr, "am1: -O2%c, a %s mode, does not exist; -O2 is the level that exists\n",
                            cP[1], (cP[1] == 's')?"space":"speed");
                        usage();
                    }

                    optimizeSetLevel(2);
                    ++cP;
                }
                else if( isdigit((unsigned char)*cP) )
                {
                    usage();                                // there is no other level
                }
                else if( *cP == '=' )
                {
                    if( !optimizeSetOption(++cP) )
                    {
                        usage();
                    }

                    cP = "";
                }
                break;

            case 'p':
                dumpTree = true;
                break;

            case 'r':
                noRim = true;
                break;

            case 's':
                doSymtab = true;
                break;

            case 'S':
                showMemUsage = true;
                break;

            case 'T':
                testMode = true;
                break;

            case 'v':
                puts(AM1VERSION);
                exit(0);

            case 'x':
                yy_flex_debug = 1;
                break;

            case 'y':
                yydebug = 1;
                break;

            case 'z':
                keepMinusZero = true;
                break;

            case 'I':                                       /* accept either Ixxx or I xxx */
                if(!*cP)
                {
                    --argc;
                    ++argv;
                    cP = *argv;
                }

                add_cpp('I', cP);
                srcNoteInclude(cP);                         // for -O=source's header
                cP = "";
                break;

            case 'D':                                       /* accept either Dxxx or D xxx */
                if( !*cP )
                {
                    --argc;
                    ++argv;
                    cP = *argv;
                }

                add_cpp( 'D', cP );
                srcNoteDefine(cP);                          // for -O=source's header
                cP = "";
                break;

            case 'W':
                if( *cP == '=' )
                {
                    enableWarning(++cP);
                    cP = "";
                }
                else
                {
                    noWarn = 0;
                    enableAllWarnings();
                }
                break;

            default:
                usage();
                break;
            }
        }

        --argc;
        ++argv;
    }

    if(argc != 1)
    {
        usage();
    }

    // The bisection modifiers switch rewrites off, so without a level they are
    // refused rather than ignored: a developer who left the level out would
    // otherwise believe the build was bisected.
    if( optBisectGiven() && (optTransformLevel() == 0) )
    {
        fprintf(stderr, "am1: -O=upto, -O=range and -O=off switch rewrites off, and need -O1 or -O2\n");
        usage();
    }

    // -O=undeclared lets -O2's guess license a length change no region declares;
    // under -O1 nothing is licensed by a guess.
    if( optUndeclaredGiven() && (optTransformLevel() < 2) )
    {
        fprintf(stderr, "am1: %s lets -O2's guess license the length-changing rewrites, and needs -O2\n",
            optUndeclaredSpelling());
        usage();
    }

    if( !doMacro && !doBinary )
    {
        doBinary = true;                                    // default is binary
    }

    if( testMode )                      // this forces no listing, no macro, no binary, no symtab, do cpp
    {
        doCpp = true;
        doSymtab = doListing = doMacro = doBinary = false;
    }

    origFilenameP = filenameP = *argv;                      // src file name */

    if((cP = strrchr(filenameP, '/')))
    {
        strcpy(basename, cP + 1);
    }
    else
    {
        strcpy(basename, filenameP);
    }

    if((cP = strrchr(basename, '.')))
    {
        *cP = '\0';
    }

    if( !doMacro )
    {
        add_cpp( 'D', "AM1" );                   // used by some includes
    }

    if(doCpp)
    {
        sprintf(pfilename, "%s.cpp", basename);
        if(!run_cpp(filenameP, pfilename))
        {
            fprintf(stderr, "am1: cpp failed\n");
            leave(0);
        }

        if(!(yyin = fopen(pfilename, "r")))
        {
            fprintf(stderr, "am1: can't open tmp file '%s'\n", pfilename);
            leave(0);
        }
    }
    else
    {
        if(!(yyin = fopen(filenameP, "r")))
        {
            fprintf(stderr, "am1: can't open source file '%s'\n", filenameP);
            leave(0);
        }
    }

    /* initialize everything */
    initParser();
    sym_init(&localSymP);

    if(yyparse())
    {
        fprintf(stderr, "Compilation failed.\n");
        leave(0);
    }

    fclose(yyin);

    if(dumpTree)
    {
        dumpParseTree(rootP);
    }

    if( doMacro && sawBank  )
    {
        vwarn(WARN_BANKS, "'bank' was used, macro1 does not support it.\n");
    }

    // The optimizer runs on the finished tree before any back end.  With -O
    // alone it changes nothing but writes its report; with -O1 or -O2 it also
    // rewrites the tree, so every back end below writes the rewritten program.
    // See opttransform.c.
    if( doOptimize )
    {
        strcpy(ofilename, basename);                         /* output file */
        strcat(ofilename, ".opt");

        if(!(outfP = fopen(ofilename, "w")))
        {
            fprintf(stderr, "am1: can't open output file '%s'\n", ofilename);
            leave(0);
        }

        i = optimize(rootP, basename);

        fclose(outfP);

        if(!i)                    // optimizer failed
        {
            unlink(ofilename);      // get rid of output
        }

        // -O=source: the program as it now stands, beside the source rather
        // than in the working directory, where every other output goes.
        if( i && optSourceWanted() )
        {
            outfP = NILP;           // closed above; leave() closes whatever it holds

            if( (cP = strrchr(origFilenameP, '/')) )
            {
                i = snprintf(ofilename, sizeof(ofilename), "%.*s/%s.opt.am1",
                    (int)(cP - origFilenameP), origFilenameP, basename);
            }
            else
            {
                i = snprintf(ofilename, sizeof(ofilename), "%s.opt.am1", basename);
            }

            if( (i < 0) || (i >= (int)sizeof(ofilename)) )
            {
                ofilename[0] = '\0';
                fprintf(stderr, "am1: -O=source: the source's path is too long\n");
                leave(0);
            }

            if(!(outfP = fopen(ofilename, "w")))
            {
                fprintf(stderr, "am1: can't open output file '%s'\n", ofilename);
                leave(0);
            }

            i = srcCodegen(outfP, rootP, origFilenameP);

            fclose(outfP);
            outfP = NILP;

            // A node it could not write makes the file wrong, not just short.
            if(!i)
            {
                fprintf(stderr, "am1: -O=source could not write %s\n", ofilename);
                leave(0);
            }
        }
    }

    if( doMacro )
    {
        strcpy(ofilename, basename);                // output file
        strcat(ofilename, ".mac");

        if(!(outfP = fopen(ofilename, "w")))
        {
            fprintf(stderr, "am1: can't open output file '%s'\n", ofilename);
            leave(0);
        }

        i = macCodegen(outfP, rootP);

        fclose(outfP);

        if(!i)                    // codegen failed
        {
            unlink(ofilename);      // get rid of output
        }
    }

    if( testMode )
    {
        strcpy(ofilename, basename);                         /* output file */
        strcat(ofilename, ".dmp");

        if(!(outfP = fopen(ofilename, "w")))
        {
            fprintf(stderr, "am1: can't open output file '%s'\n", ofilename);
            leave(0);
        }

        testCodegen(outfP, rootP);
        fclose(outfP);      // we don't unlink even if it fails
    }

    if( doBinary )
    {
        strcpy(ofilename, basename);                         /* output file */
        strcat(ofilename, (noRim)?".bin":".rim");

        if(!(outfP = fopen(ofilename, "w")))
        {
            fprintf(stderr, "am1: can't open output file '%s'\n", ofilename);
            leave(0);
        }

        i = binCodegen(outfP, rootP);

        fclose(outfP);

        if(!i)                    // codegen failed
        {
            unlink(ofilename);      // get rid of output
        }
    }

    if( doListing )
    {
        strcpy(ofilename, basename);                         /* output file */
        strcat(ofilename, ".lst");

        if(!(outfP = fopen(ofilename, "w")))
        {
            fprintf(stderr, "am1: can't open output file '%s'\n", ofilename);
            leave(0);
        }

        i = listCodegen(outfP, rootP);

        fclose(outfP);

        if(!i)                    // codegen failed
        {
            unlink(ofilename);      // get rid of output
        }
    }

    if( doSymtab )
    {
        strcpy(ofilename, basename);                         /* output file */
        strcat(ofilename, ".sym");

        if(!(outfP = fopen(ofilename, "w")))
        {
            fprintf(stderr, "am1: can't open output file '%s'\n", ofilename);
            leave(0);
        }

        listSymtab(outfP, filenameP, banksP);
        fclose(outfP);
    }

    if( showMemUsage )
    {
        printf("Highest address used:\n");

        // Go thru all the used banks, print the high address.
        // We iterate to do it in the correct order, too small a list to sort.
        for( i = 0, lastBankP = 0; banksP; ++i )
        {
            for( bankP = banksP, lastBankP = 0; bankP; bankP = bankP->nextP )
            {
                if( bankP->bank == i )
                {
                    printf("Bank %d, 0%04o (%d decimal)\n",
                        bankP->bank, bankP->cur_pc - 1, bankP->cur_pc - 1);
                    // Unlink this one
                    if( lastBankP )
                    {
                        lastBankP->nextP = bankP->nextP;
                    }
                    else
                    {
                        banksP = bankP->nextP;
                    }
                }
                else
                {
                    lastBankP = bankP;
                }
            }
        }
    }

    if( doCpp && !keepCpp)
    {
        unlink(pfilename);
    }

    exit(0);
}

#include "y.tab.h"

char *
typeToName(int type)
{
    switch(type)
    {
    case EXPR:
        return("expr");
        break;
    case OPCODE:
        return("opcode");
        break;
    case OPADDR:
        return("opaddr");
        break;
    case OPORABLE:
        return("oporable");
        break;
    case LOCATION:
        return("location");
        break;
    case LCLLOCATION:
        return("local location");
        break;
    case CONSTANT:
        return("constant ");
        break;
    case ENDCONST:
        return("endconst");
        break;
    case ADDR:
        return("addr");
        break;
    case BREF:
        return("bref");
        break;
    case LCLADDR:
        return("lcladdr");
        break;
    case ASCII:
        return("ascii");
        break;
    case TEXT:
        return("text");
        break;
    case FLEXO:
        return("flexo");
        break;
    case TYPE340:
        return("type340");
        break;
    case CHAR:
        return("char");
        break;
    case NAME:
        return("name");
        break;
    case COMMENT:
        return("comment");
        break;
    case HEADER:
        return("header");
        break;
    case ORIGIN:
        return("origin");
        break;
    case BANK:
        return("bank");
        break;
    case OPTIMIZE:
        return("optimize");
        break;
    case ENDOPTIMIZE:
        return("endoptimize");
        break;
    case VALUESPEC:
        return("valuespec");
        break;
    case INTEGER:
        return("integer");
        break;
    case LITCHAR:
        return("litchar");
        break;
    case BAD:
        return("bad");
        break;
    case DOT:
        return("dot");
        break;
    case SLASH:
        return("slash");
        break;
    case AND:
        return("and");
        break;
    case OR:
        return("or");
        break;
    case XOR:
        return("xor");
        break;
    case CMPL:
        return("cmpl");
        break;
    case MINUS:
        return("minus");
        break;
    case PLUS:
        return("plus");
        break;
    case DIV:
        return("div");
        break;
    case MOD:
        return("mod");
        break;
    case LSHIFT:
        return("lshift");
        break;
    case RSHIFT:
        return("rshift");
        break;
    case UNOP:
        return("unop");
        break;
    case UMINUS:
        return("uminus");
        break;
    case BINOP:
        return("binop");
        break;
    case LOCAL:
        return("local");
        break;
    case ADDLOCAL:
        return("addlocal");
        break;
    case FORCELOC:
        return("force locals");
        break;
    case ENDLOC:
        return("endloc");
        break;
    case START:
        return("start");
        break;
    case STOP:
        return("stop");
        break;
    case CONSTANTS:
        return("constants");
        break;
    case SEPARATOR:
        return("<separator>");
        break;
    case IMOD:
        return("i");
        break;
    default:
        return(0);
        break;
    }
}

char *
nodeToName(PNodeP nodeP, char*rsltP)
{
int type;
char *nameP;
SymNodeP symP;

    type = nodeP->type;
    nameP = typeToName(type);

    switch(type)
    {
    case LOCAL:
    case ADDLOCAL:
    case ENDLOC:
    case CONSTANTS:
    case STOP:
        return(nameP);

    case EXPR:
       sprintf(rsltP, "%s %0o", nameP, evalExpr(nodeP->rightP) & WRDMASK);
       dumpExpr(nodeP->rightP);
       break;

    case ORIGIN:
        sprintf(rsltP, "%s %0o", nameP, nodeP->value.ival);
        break;

    case OPTIMIZE:
    case ENDOPTIMIZE:
        // A hands-off span's nodes are region nodes with a flag.
        if( nodeP->flags & PN_HANDSOFF )
        {
            return( (type == OPTIMIZE)?"nooptimize":"endnooptimize" );
        }
        return(nameP);

    case VALUESPEC:
        sprintf(rsltP, "%s (%0o)", nodeP->value.symP->name, nodeP->value.symP->value);
        break;

    case INTEGER:
    case DOT:
    case START:
        sprintf(rsltP, "%s (%0o)", nameP, nodeP->value.ival & WRDMASK);
        break;

    case LOCATION:
    case LCLLOCATION:
        sprintf(rsltP, "%s %s (%0o)", nameP, nodeP->value.symP->name, nodeP->value.symP->value);
        break;

    case OPORABLE:
    case OPCODE:
    case OPADDR:
        sprintf(rsltP, "%s %s", nameP, nodeP->value.symP->name);
        break;

    case ADDR:
        symP = nodeP->value.symP;
        sprintf(rsltP, "%s %s (%0o)", nameP, symP->name, symP->value);
        break;

    case BREF:
        symP = nodeP->value.symP;
        sprintf(rsltP, "%s:%d %s (%0o)", nameP, nodeP->value2.ival, symP->name, symP->value);
        break;

    case LCLADDR:
        sprintf(rsltP, "%s %s%s",
            ((nodeP->value.symP->flags & SYM_MASK) == SYM_GLOB)?"addr":nameP,
            nodeP->value.symP->name,(nodeP->value.symP->flags & SYMF_FORCED)?"(forced)":"");
        break;

    case CONSTANT:
    case IMOD:
        sprintf(rsltP, "%s", nameP);
        break;

    case NAME:
    case COMMENT:
    case HEADER:
        sprintf(rsltP, "%s %s", nameP, nodeP->value.strP);
        break;

    case LITCHAR:
        sprintf(rsltP, "litchar '%c'", nodeP->value.ival);
        break;

    case BINOP:
    case UNOP:
        sprintf(rsltP, " %s ", typeToName(nodeP->value.ival));
        break;

    default:
        return(nameP);
        break;
    }

    return(rsltP);
}

void
dumpNode(int indent, PNodeP nodeP)
{
int i;
char str[128];

    if(nodeP)
    {
        if( (nodeP->type != TERMINATOR) && (nodeP->type != EMPTYLINE) )
        {
            for(i = 0; i < indent; ++i)
            {
                fputc(' ', stdout);
            }

            printf("%s\n", nodeToName(nodeP, str));
        }

        dumpNode(indent + 4, nodeP->leftP);
        dumpNode(indent + 4, nodeP->rightP);
    }
}

void
dumpNodeList(int indent, PNodeListP listP)
{
PNodeP nodeP;
int i;
char str[128];

    while( listP )
    {
        nodeP = listP->nodeP;

        if( (nodeP->type != TERMINATOR) && (nodeP->type != EMPTYLINE) )
        {
            for(i = 0; i < indent; ++i)
            {
                fputc(' ', stdout);
            }

            printf("%s\n", nodeToName(nodeP, str));
        }

        dumpNode(indent + 4, nodeP->leftP);

        listP = listP->nextP;
    }
}

void
dumpExpr(PNodeP nodeP)
{
char str[32];

    if(!nodeP)
    {
        return;
    }

    dumpExpr(nodeP->leftP);

    if((nodeP->type == UNOP) && (nodeP->value.ival == PARENS))
    {
        printf(" (");
        dumpExpr(nodeP->rightP);
        printf(")");
    }
    else if( nodeP->type == SEPARATOR )
    {
        printf("<separator>");
    }
    else if( nodeP->type == CONSTANT )
    {
        printf("constant(pc %o) [", nodeP->value.symP->value);
        dumpExpr((PNodeP)(nodeP->value.symP->ptr));
        printf("] ");
        dumpExpr(nodeP->rightP);
    }
    else
    {
        printf("%s", nodeToName(nodeP, str));
        dumpExpr(nodeP->rightP);
    }
}

void
dumpBody(PNodeP nodeP)
{
int i;
char ch;
char *nameP;
char str[128];

    while(nodeP)
    {
        nameP = typeToName(nodeP->type);

        switch( nodeP->type )
        {
        case ORIGIN:
            printf("%s (%o) ", nameP, nodeP->value.ival);
            dumpExpr(nodeP->rightP);
            printf("\n");
            break;

        case BANK:
            printf("%s (%0o) pc %0o", nameP, nodeP->value.ival, findBank(nodeP->value.ival)->cur_pc);
            printf("\n");
            break;

        case OPTIMIZE:
        case ENDOPTIMIZE:
            // An optimizer directive emits nothing: print its file and the pc the
            // next word will go to.
            printf("%s (%s) pc %0o\n", nameP, nodeP->value.strP, nodeP->pc);
            break;

        case EXPR:
            printf("%s ", nameP);
            dumpExpr(nodeP->rightP);
            printf("\n");
            break;

        case TEXT:
        case ASCII:
            printf("%s (%s)\n", nameP, nodeP->value.strP);
            break;

        case TYPE340:
            printf("%s\n", nameP);
            break;

        case FLEXO:
            i = nodeP->value.ival;
            printf("%s (%03o%03o%03o)", nameP, i >> 12, (i >> 6) & 077, i & 077);
            break;

        case CHAR:
            i = nodeP->value.ival;
            if( i & 077 )
            {
                ch = 'r';
            }
            else if( i & 07700 )
            {
                ch = 'm';
                i >>= 6;
            }
            else
            {
                ch = 'l';
                i >>= 12;
            }

            printf("%s (%03o%c)", nameP, i, ch);
            break;

        case VAR:
            printf("<var>\n");
            dumpNode(4, nodeP->rightP);
            break;

        case VARS:
            printf("<vars>\n");
            dumpNodeList(4, (PNodeListP)(nodeP->value.ptr));
            break;

        case TABLE:
            printf("<table %o>\n", nodeP->value.ival);
            break;

        case IMPORT:
            printf("<import %s>\n", nodeP->value.strP);
            break;

        case EXPORT:
            printf("<export>\n");
            dumpNode(4, nodeP->rightP);
            break;

        case TERMINATOR:
            printf("<terminator>\n");
            break;

        case SEMI:
            printf("<semicolon>\n");
            break;

        case EMPTYLINE:
            printf("<empty line>\n");
            break;

        case FILENAME:
            printf("<filename '%s'>\n", nodeP->value.strP);
            break;

        default:
            if( nameP )
            {
                printf("%s\n", nodeToName(nodeP, str));
            }
            else
            {
                printf("Unknown type %d\n", nodeP->type);
            }

            dumpNode(4, nodeP->rightP);
            break;
        }

        nodeP = nodeP->leftP;
    }
}

void
dumpParseTree(PNodeP nodeP)
{
char str[128];

    // Initial node sb HEADER, lhs is the body, rhs is the start at end
    if(nodeP)
    {
        printf("%s\n", nodeToName(nodeP, str));
        dumpBody(nodeP->leftP);
        dumpNode(0, nodeP->rightP);
    }
}

LocalContextP
newLocalContext()
{
LocalContextP lP;

    lP = (LocalContextP)malloc(sizeof(LocalContext));
    return(lP);
}

void
leave(int signo)
{
    if(signo)                     /* called via signal trap */
    {
        fprintf(stderr, "am1: caught signal %d\n", signo);
        signal(signo, SIG_IGN);   /* ignore the signal */
    }

    if(outfP)
    {
        fclose(outfP);
    }

    if(doCpp &&  !keepCpp)
    {
        unlink(pfilename);
    }

    unlink(ofilename);

    exit(1);
}

int
usage()
{
    // The -O= modifiers for testing the optimizer are described only in a build
    // with AM1_TEST_SWITCHES defined (make am1test); optimizeSetOption() refuses
    // them otherwise.
#ifdef AM1_TEST_SWITCHES
    fprintf(stderr, "Usage: am1 [-abdmMlnNrsSTvz[xykp]] [-O[1|2]] [-O=dump|decode|refs|flow|calls|share|regions|rules|xform|source|values|scratch|guess|speed|relayout|window|check]...\n");
    fprintf(stderr, "  [-O=upto=N] [-O=range=B:LO-HI]... [-O=off=FILE]... [-O=edits=FILE]...\n");
#else
    fprintf(stderr, "Usage: am1 [-abdmMlnNrsSTvz[xykp]] [-O[1|2]] [-O=xform|source]\n");
    fprintf(stderr, "  [-O=upto=N] [-O=range=B:LO-HI]... [-O=off=FILE]...\n");
#endif
    fprintf(stderr, "  [-O=inline=cap:N] [-O=inline=reserve:N] [-O=undeclared] [-O=unroll=cap:N]\n");
    fprintf(stderr, "  [-Dsymbol]... [-Ipath]... [-irootpath] [-W[=warning]]... sourcefile\n\n");
    fprintf(stderr, "  -a treat space in expressions as add, not or\n");
    fprintf(stderr, "  -b generate binary code, the default if neither -b nor -m is given\n");
    fprintf(stderr, "  -d generate both a listing and symbol file, combines -s and -l\n");
    fprintf(stderr, "  -l generate listing\n");
    fprintf(stderr, "  -m generate macro1 code\n");
    fprintf(stderr, "  -M memory overwrite is a warning, not a fatal error\n");
    fprintf(stderr, "  -n don't run cpp\n");
    fprintf(stderr, "  -N drop any include file text from listing\n");
    fprintf(stderr, "  -O run the optimizer advisor, report goes to sourcefile.opt\n");
    fprintf(stderr, "  -O=xform as -O, and print what -O1 or -O2 does with each finding on stdout; without\n");
    fprintf(stderr, "      either, a dry run of the one-word-for-one-word rewrites alone\n");
    fprintf(stderr, "  -O=source as -O, and write the program, as -O1 or -O2 left it, as am1 source to\n");
    fprintf(stderr, "      sourcefile.opt.am1 beside the source; unchanged lines are copied from the source,\n");
    fprintf(stderr, "      so assemble it with the source's -D and -I\n");
    fprintf(stderr, "  -O1 as -O, and rewrite only where the source declares it may: inside %%%%optimize\n");
    fprintf(stderr, "      regions, faster words, deleted words, moved code and unrolled loops; inside\n");
    fprintf(stderr, "      %%%%speed regions or for a routine named by %%%%inline, a call replaced by a copy\n");
    fprintf(stderr, "      of the routine; with none of these it assembles what -O does\n");
    fprintf(stderr, "  -O2 as -O1, and also make the one-word-for-one-word rewrites with no region needed:\n");
    fprintf(stderr, "      it guesses where code must not be touched, and warns that the guess can be wrong;\n");
    fprintf(stderr, "      a rewrite that changes the program's length still needs a declaration, or -O=undeclared;\n");
    fprintf(stderr, "      %%%%nooptimize/%%%%endnooptimize fences code off at every level;\n");
    fprintf(stderr, "      see UsingTheAm1OptimizerQuickReference.md for which level to use\n");
    fprintf(stderr, "  -O=upto=N with -O1 or -O2, keep only the first N rewrites, in -O=xform's order\n");
    fprintf(stderr, "  -O=range=B:LO-HI with -O1 or -O2, keep only rewrites in bank B (decimal), LO to HI (octal)\n");
    fprintf(stderr, "  -O=off=FILE with -O1 or -O2, keep every rewrite but those FILE lists as \"bank addr\"\n");
    fprintf(stderr, "      a rewrite stays on only if every one of these keeps it; see am1bisect.sh\n");
    fprintf(stderr, "  -O=inline=cap:N with -O1 or -O2, copy a declared call's body only up to N words (8)\n");
    fprintf(stderr, "  -O=inline=reserve:N with -O1 or -O2, leave N words free in each bank's inline budget (64)\n");
    fprintf(stderr, "  -O=undeclared with -O2, delete words, reclaim pool words and move code for fall-through\n");
    fprintf(stderr, "      placement outside the optimize and speed regions too, on the guess; -O=place=undeclared\n");
    fprintf(stderr, "      is an alias; see UsingTheAm1Optimizer.md\n");
    fprintf(stderr, "  -O=unroll=cap:N with -O1 or -O2, unroll a declared loop only if it becomes N words or fewer (64)\n");
#ifdef AM1_TEST_SWITCHES
    fprintf(stderr, "  Testing build only (AM1_TEST_SWITCHES), for testing the optimizer:\n");
    fprintf(stderr, "  -O=dump as -O, and print the optimizer's word table on stdout in -T format\n");
    fprintf(stderr, "  -O=decode as -O, and print the optimizer's decoded word table on stdout\n");
    fprintf(stderr, "  -O=refs as -O, and print the optimizer's reference edges and word flags on stdout\n");
    fprintf(stderr, "  -O=flow as -O, and print the optimizer's basic blocks and reachability on stdout\n");
    fprintf(stderr, "  -O=calls as -O, and print the optimizer's routines and the call graph on stdout\n");
    fprintf(stderr, "  -O=share as -O, and print the optimizer's return-word sharing assignment on stdout\n");
    fprintf(stderr, "  -O=regions as -O, and print the optimizer's %%%%optimize/%%%%endoptimize regions on stdout\n");
    fprintf(stderr, "  -O=rules as -O, and print the optimizer's findings on stdout\n");
    fprintf(stderr, "  -O=values as -O, and print what each basic block already knows about AC and IO on stdout\n");
    fprintf(stderr, "  -O=scratch as -O, and print whether the storage locals could share words on stdout\n");
    fprintf(stderr, "  -O=guess as -O, and print what -O2's heuristics would refuse on stdout\n");
    fprintf(stderr, "  -O=speed as -O, and print the speed-mode candidates, what each saves and spends, on stdout\n");
    fprintf(stderr, "  -O=relayout as -O, and lay the program out again, after any -O=edits, and print what happened on stdout\n");
    fprintf(stderr, "  -O=window as -O, and print the extend-window analysis, every eem, lem and hazard, on stdout\n");
    fprintf(stderr, "  -O=check as -O, and run the optimizer's decoder self-check first\n");
    fprintf(stderr, "  -O=edits=FILE for testing relayout: make the edits FILE lists, \"delete BANK ADDR\" or\n");
    fprintf(stderr, "      \"move BANK FROM TO after ADDR\" or \"copy BANK FROM TO over ADDR\", and lay the\n");
    fprintf(stderr, "      program out again\n");
    fprintf(stderr, "  -O=xform with -O1 or -O2 also analyzes the rewritten program again, which must fire nothing\n");
    fprintf(stderr, "  -O=reclaim=off hold the pool reclaim off, so a one-word-for-one-word oracle can still judge\n");
    fprintf(stderr, "      the same-length rewrites; it switches off no rewrite\n");
    fprintf(stderr, "  -O=source=render as -O=source, but every line written from the tree, none copied\n");
    fprintf(stderr, "  -O=source=tree as -O=source=render, with no line map: cpp's output, assembled with -n\n");
#endif
    fprintf(stderr, "  -r don't write a loader at the beginning of the binary file\n");
    fprintf(stderr, "  -s generate a symbol table file\n");
    fprintf(stderr, "  -S summarize memory usage per bank\n");
    fprintf(stderr, "  -v print the am1 version number and exit\n");
    fprintf(stderr, "  -z keep 1's complement -0 as the result of a math operation\n");
    fprintf(stderr, "  -D define a symbol to cpp\n");
    fprintf(stderr, "  -I add an include path to cpp\n");
    fprintf(stderr, "  -i define the include root directory\n");
    fprintf(stderr, "  -W print all warnings\n");
    fprintf(stderr, "  -W=[-]warning (- don't) print this warning\n");
    fprintf(stderr, "  These are primarily for testing, not generally useful:\n");
    fprintf(stderr, "  -x enable flex debug output on stderr\n");
    fprintf(stderr, "  -y enable yacc debug output on stderr\n");
    fprintf(stderr, "  -k don't delete cpp tmp file\n");
    fprintf(stderr, "  -p dump parse tree to stdout\n");
    fprintf(stderr, "  -T special test mode, see docs, disables several other flags\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "If neither -b nor -m are given, -b is assumed.\n");
    fprintf(stderr, "If -m is given, -b must be given if am1 binary is desired also and vice-versa.\n");
    fprintf(stderr, "By default, space is or, -0 is converted to 0.\n");
    fprintf(stderr, "See the documentation for the supported warnings.\n");
    exit(1);
}

void
add_cpp(char type, char *nameP)        // add a name to the cpp list
{
    Inc_itemP iP;
    Inc_itemP next_iP;

    if(!(iP = (Inc_itemP) malloc(sizeof(Inc_item))))
    {
        fprintf(stderr, "am1: out of memory processing -I\n");
        leave(0);
    }

    iP->type = type;

    for(next_iP = incsP; next_iP && next_iP->nextP;)
    {
        next_iP = next_iP->nextP;   // get to end of list
    }

    iP->nextP = (Inc_itemP) 0;

    if(incsP)
    {
        next_iP->nextP = iP;
    }
    else
    {
        incsP = iP;
    }

    iP->incP = nameP;
}

int
run_cpp(char *filenameP, char *pfilename)
{
    char *cP;
    char tmpstr[2048];

    if(!incroot[0])
    {
        if((cP = getenv("AM1INCDIR")))
        {
            strcpy(incroot,cP);
        }
        else
        {
            strcpy(incroot,AM1INCDIR);
        }
    }

    // Multiple tries, finally ok
    // Real balancing act between obnoxious cpp noise about defines from include files, etc. and
    // readable code.
    //sprintf(tmpstr, "%s -nostdinc -isystem %s -traditional-cpp ", CPP_PATH, incroot);
    //sprintf(tmpstr, "%s -nostdinc -isystem %s -C -x assembler-with-cpp --no-line-commands ", CPP_PATH, incroot);
    sprintf(tmpstr, "%s -nostdinc -isystem %s -C -x assembler-with-cpp ", CPP_PATH, incroot);
    cP = tmpstr + strlen(tmpstr);

    while(incsP)
    {
        sprintf(cP, "-%c%s ", incsP->type, incsP->incP);
        incsP = incsP->nextP;
        cP += strlen(cP);
    }

    sprintf(cP, "%s %s", filenameP, pfilename);

    return(!system(tmpstr));
}

void
enableWarning(char *nameP)
{
int i;
bool negate;

    negate = (*nameP == '-');
    if( negate )
    {
        ++nameP;
    }

    for( i = 0; warnings[i].id; ++i )
    {
        if( !strcmp(nameP, warnings[i].name) )
        {
            if( negate )
            {
                // They never want to see it 
                warnings[i].enabled = false;
                warnings[i].repeats = false;
                warnings[i].issued = true;
            }
            else
            {
                warnings[i].enabled = true;
            }
            return;
        }
    }

    fprintf(stderr,"No such warning '%s', ignored\n", nameP);
}

void
enableAllWarnings()
{
int i;

    for( i = 0; warnings[i].id; ++i )
    {
        warnings[i].enabled = true;
    }
}

// See if a particular warning should be issued.
// If noWarn is false, a warning will always be issued subject to repeats and issued.
// If so, return true, else false.
bool
doWarn(int id)
{
WarningP warnP;

    // First get the state
    for( warnP = warnings; warnP->id != 0; ++warnP )
    {
        if( warnP->id == id )
        {
            break;
        }
    }

    if( warnP->id == 0 )
    {
        fprintf(stderr,"Internal error bad warning id %d, ignored\n", id);
        return(false);
    }

    if( !warnP->repeats && warnP->issued )
    {
        return(false);
    }

    warnP->issued = true;

    if( noWarn && warnP->enabled )
    {
        return(true);
    }

    return( !noWarn );
}

// True if the passed name is the same as the name given on the command line, else false.
bool
fileIsMain(char *nameP)
{
    return( !strcmp(nameP, origFilenameP) );
}
