/* am1.c - another macro1 assembler
 *
 * Usage: am1 [-Wbmnv[ykp]] [-i path] [-Dsymbol[=value]]... [-I path]... sourcefile
 *
 * Valid switches are:
 *
 * -W	don't print any warnings
 * -b	generate binary source
 * -m	generate macro1 source
 * -l	generate a listing
 * -n	don't run cpp on input source
 * -v   print the current version number and exit
 *
 * -i path
 *	set the root for all includes not specified by -I
 *	also accepts -ipath syntax
 * -D symbol=value
 *	add a #define, also accepts -Dsym.. syntax
 * -I path
 *	add a directory to the cpp #include search list
 *	also accepts -Ipath syntax
 * The following are for debugging, not of genaral use.
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
 *
 * The environment variable 'AM1INCDIR' overrides the default system
 * include directory, which is overridden itself by -i.
 * If not set, it defaults to the predefined AM1INCDIR in am1.h.
 *
*/
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>

#include "am1.h"
#include "symtab.h"

typedef struct inc_item
{
    struct inc_item *nextP;
    char *incP;
    char type;                  // I, D, etc 
} Inc_item, *Inc_itemP;

Inc_itemP incsP;                // the list of cpp stuff 

FILE *outfP;                    // where we put our code 

char *filenameP;                // input am1 file 

char pfilename[128];            // cpp tmp file name
char ofilename[128];            // output file 
char basename[64];              // base name 
char incroot[129];              // root of includes
char str1[256];                 // scratch strings 
char str2[256];

bool doMacro;
bool doBinary;
bool doListing;
bool doCpp;
bool keepCpp;
bool dumpTree;
bool sawBank;
bool noWarn;
int lineno;

extern int yydebug;
extern int yy_flex_debug;

PNodeP rootP;                   // root of the parse tree
SymNodeP globalSymP;            // global addresses 
SymNodeP localSymP;             // local addresses 
SymNodeP constSymP;             // constants
SymListP constsListP;           // the list of all constant groups

extern int cur_pc;

extern char *am1_version;
extern FILE *yyin;              // lex input file 
extern int yyparse();
extern BankContextP findBank(int bankNo);
extern void setConstVal(SymNodeP);

int evalExpr(PNodeP);
int macCodegen(FILE *, PNodeP);
int binCodegen(FILE *, PNodeP);
int listCodegen(FILE *, PNodeP);

void add_cpp(char, char *);
void leave(int);
int run_cpp(char *, char *);
int usage();
void dumpParseTree(PNodeP);
void dumpExpr(PNodeP);

int
main(int argc, char **argv)
{
int i;
char *cP, *cP2;
SymNodeP symP;

    yydebug = 0;
    yy_flex_debug = 0;

    for(i = 1; i < NSIG;)
    {
        if(signal(i++, leave) == SIG_IGN)
        {
            /* catch all sigs */
            signal(i - 1, SIG_IGN);                       /* unless ignored */
        }
    }

    signal(SIGCHLD, SIG_DFL);                             /* special case */

    doCpp = 1;

    /* do the command line processing */
    ++argv;
    --argc;

    while(argc && (**argv == '-'))                        /* look for directives */
    {
        for(cP = *argv + 1; *cP;)
        {
            switch(*cP++)
            {
            case 'k':
                keepCpp = true;
                break;

            case 'p':
                dumpTree = true;
                break;

            case 'm':
                doMacro = true;
                break;

            case 'b':
                doBinary = true;
                break;

            case 'l':
                doListing = true;
                break;

            case 'n':
                doCpp = true;
                break;

            case 'v':
                puts(AM1VERSION);
                exit(0);

            case 'i':                                       /* accept either ixxx or i xxx */
                if(!*cP)
                {
                    --argc;
                    ++argv;
                    cP = *argv;
                }

                strcpy(incroot, cP);
                break;

            case 'x':
                yy_flex_debug = 1;
                break;

            case 'y':
                yydebug = 1;
                break;

            case 'I':                                       /* accept either Ixxx or I xxx */
                if(!*cP)
                {
                    --argc;
                    ++argv;
                    cP = *argv;
                }

                add_cpp('I', cP);
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
                cP = "";
                break;

            case 'W':
                noWarn = 1;
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

    if( !doMacro && !doBinary )
    {
        doBinary = true;                                    // default is binary
    }

    filenameP = *argv;                                      // src file name */

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

    if(doCpp)
    {
        sprintf(pfilename, "%s.cpp", basename);
    }

    if( doCpp )
    {
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
    sym_init(&globalSymP);
    sym_init(&localSymP);
    sym_init(&constSymP);

    if(yyparse())
    {
        fprintf(stderr, "Compilation failed.\n");
        leave(0);
    }

    // Now resolve all const values
    for( SymListP listP = constsListP; listP; listP = listP->nextP )
    {
        setConstVal(listP->symP);
    }

    fclose(yyin);
    
    if(dumpTree)
    {
        dumpParseTree(rootP);
    }

    if( doMacro && sawBank )
    {
        fprintf(stderr, "am1: WARNING - 'bank' was used, macro1 does not support it.\n");
        fprintf(stderr, "Your code will not do what you expect and should just be for reference.\n");
    }

    if( doMacro )
    {
        strcpy(ofilename, basename);                         /* output file */
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

    if( doBinary )
    {
        strcpy(ofilename, basename);                         /* output file */
        strcat(ofilename, ".rim");

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

    if(doCpp && !keepCpp)
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
        return("ascii");
        break;
    case FLEXO:
        return("flexo");
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
    case FORCELOC:
        return("force locals");
        break;
    case ENDLOC:
        return("endloc");
        break;
    case START:
        return("start");
        break;
    case CONSTANTS:
        return("constants");
        break;
    case RELOC:
        return("reloc");
        break;
    case ENDRELOC:
        return("endreloc");
        break;
    case SEPARATOR:
        return("<separator>");
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
    case ENDLOC:
    case CONSTANTS:
    case RELOC:
    case ENDRELOC:
        return(nameP);

    case EXPR:
       sprintf(rsltP, "%s %0o", nameP, evalExpr(nodeP->rightP) & WRDMASK);
       dumpExpr(nodeP->rightP);
       break;

    case ORIGIN:
        sprintf(rsltP, "%s %0o", nameP, nodeP->value.ival);
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
        sprintf(rsltP, "%s", nameP);
        break;

    case NAME:
    case LCLNAME:
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
        if(nodeP->type != TERMINATOR)
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
dumpExpr(PNodeP nodeP)
{
PNodeP nodeP2;
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
        printf("]");
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
PNodeP nodeP2;
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

        case EXPR:
            printf("%s ", nameP);
            dumpExpr(nodeP->rightP);
            printf("\n");
            break;

        case TEXT:
        case ASCII:
            printf("%s (%s)\n", nameP, nodeP->value.strP);
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

        case TERMINATOR:
            printf("<terminator>\n");
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
    fprintf(stderr, "Usage: am1 [-Wbmlnv[xykp]] [-Dsymbol]... [-Ipath]... [-ipath] sourcefile\n");
    fprintf(stderr, "  -W don't print warnings\n");
    fprintf(stderr, "  -b generate binary code\n");
    fprintf(stderr, "  -m generate macro1 code\n");
    fprintf(stderr, "  -l generate listing\n");
    fprintf(stderr, "  -n don't run cpp\n");
    fprintf(stderr, "  -v print the am1 version number and exit\n");
    fprintf(stderr, "  -D define a symbol to cpp\n");
    fprintf(stderr, "  -I add an include path to cpp\n");
    fprintf(stderr, "  -i define the include root\n");
    fprintf(stderr, "  -x enable flex debug output on stderr\n");
    fprintf(stderr, "  -y enable yacc debug output on stderr\n");
    fprintf(stderr, "  -k don't delete cpp tmp file\n");
    fprintf(stderr, "  -p dump parse tree to stdout\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "If neither -b nor -m are given, -b is assumed.\n");
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

    sprintf(tmpstr, "%s -DAM1 -nostdinc -isystem %s -traditional-cpp ", CPP_PATH, incroot);
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
