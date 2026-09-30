/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 1 "parser.y"

/* parser.y - yacc for the PDP-1 new macro assembler */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>

#include "am1.h"
#include "symtab.h"


// We maintain a stack of local symtab ptrs for nested local scopes
int localDepth = 0;
int maxLocalDepth = 0;                  // the deepest nexting we've seen
LocalContextP localContextP;            // used while a local scope is enabled
LocalContextP localStack[MAXLOCALS];
bool sawForceLocal;
bool atSol = true;                      // initially true

// Bank contexts are kept as a linked list, not used often enough to need preallocation
int curBank;
BankContextP banksP;
BankContextP curBankP;
static SymNodeP pendingLabelSymP;
static int pendingLabelLocType;
static int pendingLabelPC;

static char scratchStr[128];            // and string

extern PNodeListP wildcardsP;           // any wildcarded cross-bank refs
extern SymNodeP permSymP;            // the instructions and other permanent values
extern SymListP constsListP;            // the list of all constant groups

extern bool noWarn;
extern bool doMacro;
extern bool sawBank;
extern bool dropIncludeText;
extern bool doSymtab;
extern int lineno;
extern char *filenameP;
extern char *incroot;
extern PNodeP rootP;

int setConstPC(int pc, SymNodeP symP);
void setConstVal(SymNodeP symP);
void setVarsPC(int bank, PNodeListP listP);
void addExports(PNodeP nodesP);
void importSymbols(char *filenameP);
void resolveWildcards(PNodeListP wildsP, BankContextP banksP);
bool resolveWildcard(PNodeListP itemP, BankContextP bankP);
BankContextP findBank(int bank);
BankContextP addBank(int bank);
BankContextP swapBanks(int newBank);
void initParser(void);
SymNodeP addLocalSymbol(char *nameP);
SymListP addToSymlist(SymListP listP, SymNodeP symP, int bank, int pc);
SymNodeP findSymbolInBank(int bank, char *nameP);

int yyerror(const char *errstr);
void verror(const char *msgP, ...);
void vwarn(int errtype, const char *msgP, ...);
void vwarnl(int errType, int lineno, const char *msgP, ...);
void verrorl(int lineno, const char *msgP, ...);

int yylex(void);

extern int evalExpr(PNodeP);
extern int countAscii(char *strP);
extern int countType340(char *strP);
extern int countText(FlexText text);
extern long int hashExpr(PNodeP);
extern bool doWarn(int errType);
extern bool fileIsMain(char *nameP);
extern void checkPCBound(char *msgP, int pc, int lineNo);
extern void leave(int);
extern LocalContextP newLocalContext(void);
extern PNodeP binop(int lineNo, int pc, int value, PNodeP leftP, PNodeP rightP);
extern PNodeP unop(int lineNo, int pc, int value, PNodeP expP);
extern PNodeP newnode(int lineNo, int pc, int val, PNodeP leftP, PNodeP rightP);


#line 157 "y.tab.c"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

/* Use api.header.include to #include this header
   instead of duplicating it here.  */
#ifndef YY_YY_Y_TAB_H_INCLUDED
# define YY_YY_Y_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    START = 258,                   /* START  */
    STOP = 259,                    /* STOP  */
    CONSTANT = 260,                /* CONSTANT  */
    NAMES = 261,                   /* NAMES  */
    VAR = 262,                     /* VAR  */
    VARS = 263,                    /* VARS  */
    TABLE = 264,                   /* TABLE  */
    ASCII = 265,                   /* ASCII  */
    TYPE340 = 266,                 /* TYPE340  */
    EXPORT = 267,                  /* EXPORT  */
    IMPORT = 268,                  /* IMPORT  */
    OPCODE = 269,                  /* OPCODE  */
    OPADDR = 270,                  /* OPADDR  */
    OPORABLE = 271,                /* OPORABLE  */
    VALUESPEC = 272,               /* VALUESPEC  */
    IMOD = 273,                    /* IMOD  */
    ADDR = 274,                    /* ADDR  */
    LCLADDR = 275,                 /* LCLADDR  */
    LAW = 276,                     /* LAW  */
    NAME = 277,                    /* NAME  */
    LCLNAME = 278,                 /* LCLNAME  */
    COMMENT = 279,                 /* COMMENT  */
    CSCOMMENT = 280,               /* CSCOMMENT  */
    ENDLOC = 281,                  /* ENDLOC  */
    HEADER = 282,                  /* HEADER  */
    STRING = 283,                  /* STRING  */
    T340STRING = 284,              /* T340STRING  */
    TEXT = 285,                    /* TEXT  */
    FILENAME = 286,                /* FILENAME  */
    LIBFILE = 287,                 /* LIBFILE  */
    CSSTART = 288,                 /* CSSTART  */
    CHAR = 289,                    /* CHAR  */
    FLEXO = 290,                   /* FLEXO  */
    INTEGER = 291,                 /* INTEGER  */
    LITCHAR = 292,                 /* LITCHAR  */
    BAD = 293,                     /* BAD  */
    ORIGIN = 294,                  /* ORIGIN  */
    EXPR = 295,                    /* EXPR  */
    BANK = 296,                    /* BANK  */
    THISBANK = 297,                /* THISBANK  */
    LOCATION = 298,                /* LOCATION  */
    LCLLOCATION = 299,             /* LCLLOCATION  */
    LOCAL = 300,                   /* LOCAL  */
    ADDLOCAL = 301,                /* ADDLOCAL  */
    FORCELOC = 302,                /* FORCELOC  */
    PRIVATE = 303,                 /* PRIVATE  */
    DOT = 304,                     /* DOT  */
    SLASH = 305,                   /* SLASH  */
    AND = 306,                     /* AND  */
    OR = 307,                      /* OR  */
    XOR = 308,                     /* XOR  */
    CMPL = 309,                    /* CMPL  */
    MINUS = 310,                   /* MINUS  */
    PLUS = 311,                    /* PLUS  */
    DIV = 312,                     /* DIV  */
    MOD = 313,                     /* MOD  */
    UNOP = 314,                    /* UNOP  */
    BINOP = 315,                   /* BINOP  */
    PARENS = 316,                  /* PARENS  */
    BREF = 317,                    /* BREF  */
    WILDREF = 318,                 /* WILDREF  */
    ENDCONST = 319,                /* ENDCONST  */
    SEPARATOR = 320,               /* SEPARATOR  */
    TERMINATOR = 321,              /* TERMINATOR  */
    SEMI = 322,                    /* SEMI  */
    EMPTYLINE = 323,               /* EMPTYLINE  */
    CONSTANTS = 324,               /* CONSTANTS  */
    LSHIFT = 325,                  /* LSHIFT  */
    RSHIFT = 326,                  /* RSHIFT  */
    MUL = 327,                     /* MUL  */
    UMINUS = 328                   /* UMINUS  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 89 "parser.y"

    long int ival;
    char *strP;
    SymNodeP symP;
    PNodeP pnodeP;
    FlexText flexText;
    

#line 289 "y.tab.c"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void);


#endif /* !YY_YY_Y_TAB_H_INCLUDED  */
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_START = 3,                      /* START  */
  YYSYMBOL_STOP = 4,                       /* STOP  */
  YYSYMBOL_CONSTANT = 5,                   /* CONSTANT  */
  YYSYMBOL_NAMES = 6,                      /* NAMES  */
  YYSYMBOL_VAR = 7,                        /* VAR  */
  YYSYMBOL_VARS = 8,                       /* VARS  */
  YYSYMBOL_TABLE = 9,                      /* TABLE  */
  YYSYMBOL_ASCII = 10,                     /* ASCII  */
  YYSYMBOL_TYPE340 = 11,                   /* TYPE340  */
  YYSYMBOL_EXPORT = 12,                    /* EXPORT  */
  YYSYMBOL_IMPORT = 13,                    /* IMPORT  */
  YYSYMBOL_OPCODE = 14,                    /* OPCODE  */
  YYSYMBOL_OPADDR = 15,                    /* OPADDR  */
  YYSYMBOL_OPORABLE = 16,                  /* OPORABLE  */
  YYSYMBOL_VALUESPEC = 17,                 /* VALUESPEC  */
  YYSYMBOL_IMOD = 18,                      /* IMOD  */
  YYSYMBOL_ADDR = 19,                      /* ADDR  */
  YYSYMBOL_LCLADDR = 20,                   /* LCLADDR  */
  YYSYMBOL_LAW = 21,                       /* LAW  */
  YYSYMBOL_NAME = 22,                      /* NAME  */
  YYSYMBOL_LCLNAME = 23,                   /* LCLNAME  */
  YYSYMBOL_COMMENT = 24,                   /* COMMENT  */
  YYSYMBOL_CSCOMMENT = 25,                 /* CSCOMMENT  */
  YYSYMBOL_ENDLOC = 26,                    /* ENDLOC  */
  YYSYMBOL_HEADER = 27,                    /* HEADER  */
  YYSYMBOL_STRING = 28,                    /* STRING  */
  YYSYMBOL_T340STRING = 29,                /* T340STRING  */
  YYSYMBOL_TEXT = 30,                      /* TEXT  */
  YYSYMBOL_FILENAME = 31,                  /* FILENAME  */
  YYSYMBOL_LIBFILE = 32,                   /* LIBFILE  */
  YYSYMBOL_CSSTART = 33,                   /* CSSTART  */
  YYSYMBOL_CHAR = 34,                      /* CHAR  */
  YYSYMBOL_FLEXO = 35,                     /* FLEXO  */
  YYSYMBOL_INTEGER = 36,                   /* INTEGER  */
  YYSYMBOL_LITCHAR = 37,                   /* LITCHAR  */
  YYSYMBOL_BAD = 38,                       /* BAD  */
  YYSYMBOL_ORIGIN = 39,                    /* ORIGIN  */
  YYSYMBOL_EXPR = 40,                      /* EXPR  */
  YYSYMBOL_BANK = 41,                      /* BANK  */
  YYSYMBOL_THISBANK = 42,                  /* THISBANK  */
  YYSYMBOL_LOCATION = 43,                  /* LOCATION  */
  YYSYMBOL_LCLLOCATION = 44,               /* LCLLOCATION  */
  YYSYMBOL_LOCAL = 45,                     /* LOCAL  */
  YYSYMBOL_ADDLOCAL = 46,                  /* ADDLOCAL  */
  YYSYMBOL_FORCELOC = 47,                  /* FORCELOC  */
  YYSYMBOL_PRIVATE = 48,                   /* PRIVATE  */
  YYSYMBOL_DOT = 49,                       /* DOT  */
  YYSYMBOL_SLASH = 50,                     /* SLASH  */
  YYSYMBOL_AND = 51,                       /* AND  */
  YYSYMBOL_OR = 52,                        /* OR  */
  YYSYMBOL_XOR = 53,                       /* XOR  */
  YYSYMBOL_CMPL = 54,                      /* CMPL  */
  YYSYMBOL_MINUS = 55,                     /* MINUS  */
  YYSYMBOL_PLUS = 56,                      /* PLUS  */
  YYSYMBOL_DIV = 57,                       /* DIV  */
  YYSYMBOL_MOD = 58,                       /* MOD  */
  YYSYMBOL_UNOP = 59,                      /* UNOP  */
  YYSYMBOL_BINOP = 60,                     /* BINOP  */
  YYSYMBOL_PARENS = 61,                    /* PARENS  */
  YYSYMBOL_BREF = 62,                      /* BREF  */
  YYSYMBOL_WILDREF = 63,                   /* WILDREF  */
  YYSYMBOL_ENDCONST = 64,                  /* ENDCONST  */
  YYSYMBOL_SEPARATOR = 65,                 /* SEPARATOR  */
  YYSYMBOL_TERMINATOR = 66,                /* TERMINATOR  */
  YYSYMBOL_SEMI = 67,                      /* SEMI  */
  YYSYMBOL_EMPTYLINE = 68,                 /* EMPTYLINE  */
  YYSYMBOL_CONSTANTS = 69,                 /* CONSTANTS  */
  YYSYMBOL_LSHIFT = 70,                    /* LSHIFT  */
  YYSYMBOL_RSHIFT = 71,                    /* RSHIFT  */
  YYSYMBOL_MUL = 72,                       /* MUL  */
  YYSYMBOL_UMINUS = 73,                    /* UMINUS  */
  YYSYMBOL_74_ = 74,                       /* '='  */
  YYSYMBOL_75_ = 75,                       /* '('  */
  YYSYMBOL_76_ = 76,                       /* ')'  */
  YYSYMBOL_YYACCEPT = 77,                  /* $accept  */
  YYSYMBOL_program = 78,                   /* program  */
  YYSYMBOL_optfilenames = 79,              /* optfilenames  */
  YYSYMBOL_filenames = 80,                 /* filenames  */
  YYSYMBOL_start = 81,                     /* start  */
  YYSYMBOL_body = 82,                      /* body  */
  YYSYMBOL_stmt_list = 83,                 /* stmt_list  */
  YYSYMBOL_stmt = 84,                      /* stmt  */
  YYSYMBOL_one_stmt = 85,                  /* one_stmt  */
  YYSYMBOL_86_1 = 86,                      /* $@1  */
  YYSYMBOL_87_2 = 87,                      /* $@2  */
  YYSYMBOL_88_3 = 88,                      /* $@3  */
  YYSYMBOL_89_4 = 89,                      /* $@4  */
  YYSYMBOL_terminator = 90,                /* terminator  */
  YYSYMBOL_terminators = 91,               /* terminators  */
  YYSYMBOL_optExpr = 92,                   /* optExpr  */
  YYSYMBOL_labelTrailer = 93,              /* labelTrailer  */
  YYSYMBOL_optINTEGER = 94,                /* optINTEGER  */
  YYSYMBOL_expr = 95,                      /* expr  */
  YYSYMBOL_optSEPARATOR = 96,              /* optSEPARATOR  */
  YYSYMBOL_simple_expr = 97,               /* simple_expr  */
  YYSYMBOL_directive_expr = 98,            /* directive_expr  */
  YYSYMBOL_endConst = 99,                  /* endConst  */
  YYSYMBOL_LOCorADDLOCorPRIVATE = 100,     /* LOCorADDLOCorPRIVATE  */
  YYSYMBOL_optLocals = 101,                /* optLocals  */
  YYSYMBOL_localSymDef = 102,              /* localSymDef  */
  YYSYMBOL_symList = 103,                  /* symList  */
  YYSYMBOL_symbol = 104,                   /* symbol  */
  YYSYMBOL_varnames = 105,                 /* varnames  */
  YYSYMBOL_bref = 106,                     /* bref  */
  YYSYMBOL_wildref = 107,                  /* wildref  */
  YYSYMBOL_var = 108,                      /* var  */
  YYSYMBOL_varname = 109                   /* varname  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_uint8 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if !defined yyoverflow

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* !defined yyoverflow */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  5
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   346

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  77
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  33
/* YYNRULES -- Number of rules.  */
#define YYNRULES  119
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  171

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   328


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      75,    76,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,    74,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    70,    71,    72,    73
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,   214,   214,   220,   221,   224,   225,   228,   233,   238,
     289,   303,   308,   317,   348,   354,   363,   381,   385,   400,
     406,   405,   452,   451,   488,   487,   511,   510,   539,   560,
     567,   576,   583,   590,   597,   610,   616,   622,   629,   634,
     638,   648,   660,   664,   668,   672,   678,   683,   689,   693,
     701,   710,   720,   725,   730,   731,   732,   737,   745,   746,
     749,   750,   751,   752,   753,   754,   755,   756,   757,   758,
     759,   760,   761,   762,   763,   768,   773,   778,   783,   792,
     805,   833,   838,   843,   848,   870,   888,   901,   927,   956,
     971,   976,   981,   986,   993,  1005,  1086,  1113,  1117,  1124,
    1128,  1132,  1138,  1143,  1150,  1155,  1164,  1183,  1188,  1196,
    1201,  1208,  1220,  1235,  1244,  1250,  1253,  1257,  1263,  1274
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if YYDEBUG || 0
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "START", "STOP",
  "CONSTANT", "NAMES", "VAR", "VARS", "TABLE", "ASCII", "TYPE340",
  "EXPORT", "IMPORT", "OPCODE", "OPADDR", "OPORABLE", "VALUESPEC", "IMOD",
  "ADDR", "LCLADDR", "LAW", "NAME", "LCLNAME", "COMMENT", "CSCOMMENT",
  "ENDLOC", "HEADER", "STRING", "T340STRING", "TEXT", "FILENAME",
  "LIBFILE", "CSSTART", "CHAR", "FLEXO", "INTEGER", "LITCHAR", "BAD",
  "ORIGIN", "EXPR", "BANK", "THISBANK", "LOCATION", "LCLLOCATION", "LOCAL",
  "ADDLOCAL", "FORCELOC", "PRIVATE", "DOT", "SLASH", "AND", "OR", "XOR",
  "CMPL", "MINUS", "PLUS", "DIV", "MOD", "UNOP", "BINOP", "PARENS", "BREF",
  "WILDREF", "ENDCONST", "SEPARATOR", "TERMINATOR", "SEMI", "EMPTYLINE",
  "CONSTANTS", "LSHIFT", "RSHIFT", "MUL", "UMINUS", "'='", "'('", "')'",
  "$accept", "program", "optfilenames", "filenames", "start", "body",
  "stmt_list", "stmt", "one_stmt", "$@1", "$@2", "$@3", "$@4",
  "terminator", "terminators", "optExpr", "labelTrailer", "optINTEGER",
  "expr", "optSEPARATOR", "simple_expr", "directive_expr", "endConst",
  "LOCorADDLOCorPRIVATE", "optLocals", "localSymDef", "symList", "symbol",
  "varnames", "bref", "wildref", "var", "varname", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-64)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-57)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
     -17,   -64,    20,     4,    32,   -64,     6,   -64,   115,   172,
      94,   -64,   172,    63,    69,    95,    -2,   -64,   -64,   -64,
     -64,   -33,    78,    21,   -16,    90,   -64,   104,   -64,   -64,
     117,   -64,   -64,    81,   -64,   108,   -64,   -64,   -64,   -64,
     -64,   -64,   172,   172,   -64,   -64,   -64,   -64,   172,    93,
     115,    44,   -64,   -64,   -63,   -64,   146,   -64,    73,    85,
     -64,    85,   -64,     9,   -64,   -64,   110,   -64,    80,   179,
     -64,   -64,   -64,   -64,   112,   -64,   -64,   -64,   -64,    22,
     -64,   -64,   -64,   -64,   172,   -64,   -64,   -64,   -64,   -64,
     -64,   -64,    34,   -64,   -64,   -64,   -64,   201,   172,    92,
     -64,    44,   -64,   -64,   -64,   -64,   -64,   172,   172,   172,
     172,   172,   172,   172,   172,   172,   172,   172,   -64,   116,
     -64,   -64,   -64,   -64,   -64,    94,   172,   172,    95,     2,
     -64,   -64,   -64,     2,   245,     2,     2,   -64,   223,   -64,
     -64,   274,   256,   267,    -3,    -3,   -64,   -64,   212,   183,
     183,   -64,    73,   -64,   245,   245,   -64,   137,   138,   -64,
     -64,   -64,   -64,   245,   -64,   -64,   -64,   -64,   -64,   -64,
     -64
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       4,     5,     0,     0,     3,     1,     0,     6,     0,     0,
       0,    18,     0,     0,     0,     0,     0,    76,    77,    78,
      79,    82,    90,    59,    88,    89,    40,    53,    31,    41,
       0,    92,    91,    74,    93,     0,    75,    99,   100,    94,
     101,    81,     0,     0,    42,    39,    43,    28,     0,     0,
       9,     0,    14,    11,    38,    15,    54,    55,   104,    82,
      90,    88,    89,     0,   119,   118,    17,   111,   116,    32,
      29,    30,   110,   109,    34,   107,    35,    36,    22,     0,
      85,    87,    26,    58,     0,    20,    84,    86,    24,    52,
      96,    37,     0,    83,    16,    73,    61,     0,     0,     0,
       2,     0,    12,    10,    44,    45,    19,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,   106,    95,
     102,   105,    98,    97,    80,     0,     0,     0,     0,    47,
     113,   114,   115,    47,    57,    47,    47,    72,     0,     8,
      13,    67,    68,    69,    63,    62,    65,    66,    60,    70,
      71,    64,     0,   112,   117,    33,   108,     0,     0,    51,
      48,    23,    46,    54,    27,    21,    25,     7,   103,    49,
      50
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int8 yypgoto[] =
{
     -64,   -64,   -64,   -64,   -64,   -64,   -64,   118,   -64,   -64,
     -64,   -64,   -64,   -42,   -64,   -64,   -51,   -64,     3,   -64,
      -8,   -64,   -64,   -64,   -64,    14,   -64,   -13,   -64,   -18,
      17,    46,   -64
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_uint8 yydefgoto[] =
{
       0,     2,     3,     4,   100,    49,    50,    51,    52,   135,
     129,   136,   133,    53,    54,   160,   161,    90,   162,    84,
     163,    57,   124,    58,   119,   120,    74,   121,    66,    80,
      81,    67,    68
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
      56,    63,    75,   104,    69,   105,    86,     9,   102,   103,
      78,    55,   157,   158,     1,    93,    17,    18,    19,    20,
       5,    59,    60,    23,    61,    62,    76,    85,    27,    79,
      77,     6,   159,   122,    95,    96,    31,    32,    33,    34,
      97,    87,    56,    86,    36,   -56,    79,    37,    38,    39,
      40,    41,   -56,    55,   112,   113,    42,    43,   130,   140,
     107,   108,   109,     7,   110,   111,   112,   113,    26,   117,
     130,   131,     8,   123,   114,    29,   134,    48,    87,   115,
     116,   117,   164,   131,   165,   166,    83,   -56,   -56,   -56,
     138,    70,    72,   118,   132,    73,    98,    99,    71,   141,
     142,   143,   144,   145,   146,   147,   148,   149,   150,   151,
      44,    45,    46,    64,    72,   156,    65,    73,   154,   155,
       9,    82,    10,    11,    12,    13,    14,    15,    16,    17,
      18,    19,    20,    88,    21,    22,    23,    24,    25,    26,
      89,    27,    91,    92,    94,    28,    29,    79,    30,    31,
      32,    33,    34,   125,   126,   128,    35,    36,   139,   152,
      37,    38,    39,    40,    41,   169,   168,   170,   101,    42,
      43,   153,     0,     0,     0,     0,     0,     9,     0,     0,
       0,    44,    45,    46,    47,   106,    17,    18,    19,    20,
      48,    59,    60,     0,    61,    62,     0,   107,   108,   109,
       0,   110,   111,   112,   113,     0,    31,    32,    33,    34,
       0,   114,     0,     0,    36,     0,   115,   116,   117,     0,
       0,    41,   127,     0,     0,     0,    42,    43,     0,     0,
     107,   108,   109,     0,   110,   111,   112,   113,   110,   111,
     112,   113,     0,     0,   114,     0,     0,    48,     0,   115,
     116,   117,   107,   108,   109,   117,   110,   111,   112,   113,
       0,     0,     0,   107,   108,   109,   114,   110,   111,   112,
     113,   115,   116,   117,   107,   108,   109,   137,   110,   111,
     112,   113,   115,   116,   117,     0,     0,     0,   114,   167,
       0,     0,     0,   115,   116,   117,   107,   108,   109,     0,
     110,   111,   112,   113,     0,     0,     0,   107,     0,   109,
     114,   110,   111,   112,   113,   115,   116,   117,   107,     0,
       0,     0,   110,   111,   112,   113,   115,   116,   117,   110,
     111,   112,   113,     0,     0,     0,     0,   115,   116,   117,
       0,     0,     0,     0,   115,   116,   117
};

static const yytype_int16 yycheck[] =
{
       8,     9,    15,    66,    12,    68,    24,     5,    50,    51,
      43,     8,    10,    11,    31,    33,    14,    15,    16,    17,
       0,    19,    20,    21,    22,    23,    28,    43,    26,    62,
      32,    27,    30,    24,    42,    43,    34,    35,    36,    37,
      48,    24,    50,    61,    42,    24,    62,    45,    46,    47,
      48,    49,    31,    50,    57,    58,    54,    55,    36,   101,
      51,    52,    53,    31,    55,    56,    57,    58,    24,    72,
      36,    49,    66,    64,    65,    31,    84,    75,    61,    70,
      71,    72,   133,    49,   135,   136,    65,    66,    67,    68,
      98,    28,    19,    20,    72,    22,     3,     4,    29,   107,
     108,   109,   110,   111,   112,   113,   114,   115,   116,   117,
      66,    67,    68,    19,    19,   128,    22,    22,   126,   127,
       5,    43,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    43,    19,    20,    21,    22,    23,    24,
      36,    26,    25,    62,    36,    30,    31,    62,    33,    34,
      35,    36,    37,    43,    74,    43,    41,    42,    66,    43,
      45,    46,    47,    48,    49,    28,   152,    29,    50,    54,
      55,   125,    -1,    -1,    -1,    -1,    -1,     5,    -1,    -1,
      -1,    66,    67,    68,    69,    39,    14,    15,    16,    17,
      75,    19,    20,    -1,    22,    23,    -1,    51,    52,    53,
      -1,    55,    56,    57,    58,    -1,    34,    35,    36,    37,
      -1,    65,    -1,    -1,    42,    -1,    70,    71,    72,    -1,
      -1,    49,    43,    -1,    -1,    -1,    54,    55,    -1,    -1,
      51,    52,    53,    -1,    55,    56,    57,    58,    55,    56,
      57,    58,    -1,    -1,    65,    -1,    -1,    75,    -1,    70,
      71,    72,    51,    52,    53,    72,    55,    56,    57,    58,
      -1,    -1,    -1,    51,    52,    53,    65,    55,    56,    57,
      58,    70,    71,    72,    51,    52,    53,    76,    55,    56,
      57,    58,    70,    71,    72,    -1,    -1,    -1,    65,    66,
      -1,    -1,    -1,    70,    71,    72,    51,    52,    53,    -1,
      55,    56,    57,    58,    -1,    -1,    -1,    51,    -1,    53,
      65,    55,    56,    57,    58,    70,    71,    72,    51,    -1,
      -1,    -1,    55,    56,    57,    58,    70,    71,    72,    55,
      56,    57,    58,    -1,    -1,    -1,    -1,    70,    71,    72,
      -1,    -1,    -1,    -1,    70,    71,    72
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,    31,    78,    79,    80,     0,    27,    31,    66,     5,
       7,     8,     9,    10,    11,    12,    13,    14,    15,    16,
      17,    19,    20,    21,    22,    23,    24,    26,    30,    31,
      33,    34,    35,    36,    37,    41,    42,    45,    46,    47,
      48,    49,    54,    55,    66,    67,    68,    69,    75,    82,
      83,    84,    85,    90,    91,    95,    97,    98,   100,    19,
      20,    22,    23,    97,    19,    22,   105,   108,   109,    97,
      28,    29,    19,    22,   103,   104,    28,    32,    43,    62,
     106,   107,    43,    65,    96,    43,   106,   107,    43,    36,
      94,    25,    62,   106,    36,    97,    97,    97,     3,     4,
      81,    84,    90,    90,    66,    68,    39,    51,    52,    53,
      55,    56,    57,    58,    65,    70,    71,    72,    20,   101,
     102,   104,    24,    64,    99,    43,    74,    43,    43,    87,
      36,    49,    72,    89,    97,    86,    88,    76,    97,    66,
      90,    97,    97,    97,    97,    97,    97,    97,    97,    97,
      97,    97,    43,   108,    97,    97,   104,    10,    11,    30,
      92,    93,    95,    97,    93,    93,    93,    66,   102,    28,
      29
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    77,    78,    79,    79,    80,    80,    81,    81,    82,
      83,    83,    83,    83,    84,    85,    85,    85,    85,    85,
      86,    85,    87,    85,    88,    85,    89,    85,    85,    85,
      85,    85,    85,    85,    85,    85,    85,    85,    90,    90,
      90,    90,    91,    91,    91,    91,    92,    92,    93,    93,
      93,    93,    94,    94,    95,    95,    95,    95,    96,    96,
      97,    97,    97,    97,    97,    97,    97,    97,    97,    97,
      97,    97,    97,    97,    97,    97,    97,    97,    97,    97,
      97,    97,    97,    97,    97,    97,    97,    97,    97,    97,
      97,    97,    97,    97,    98,    98,    98,    99,    99,   100,
     100,   100,   101,   101,   101,   102,   102,   103,   103,   104,
     104,   105,   105,   106,   106,   107,   108,   108,   109,   109
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     5,     1,     0,     1,     2,     3,     2,     1,
       2,     1,     2,     3,     1,     1,     2,     2,     1,     2,
       0,     4,     0,     4,     0,     4,     0,     4,     1,     2,
       2,     1,     2,     4,     2,     2,     2,     2,     1,     1,
       1,     1,     1,     1,     2,     2,     1,     0,     1,     2,
       2,     1,     1,     0,     1,     1,     1,     3,     1,     0,
       3,     2,     3,     3,     3,     3,     3,     3,     3,     3,
       3,     3,     3,     2,     1,     1,     1,     1,     1,     1,
       3,     1,     1,     2,     2,     2,     2,     2,     1,     1,
       1,     1,     1,     1,     1,     2,     2,     1,     1,     1,
       1,     1,     1,     3,     0,     1,     1,     1,     3,     1,
       1,     1,     3,     2,     2,     2,     1,     3,     1,     1
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)




# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  yy_symbol_value_print (yyo, yykind, yyvaluep);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp,
                 int yyrule)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)]);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif






/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep)
{
  YY_USE (yyvaluep);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/* Lookahead token kind.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Number of syntax errors so far.  */
int yynerrs;




/*----------.
| yyparse.  |
`----------*/

int
yyparse (void)
{
    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;



#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */

  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex ();
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 2: /* program: optfilenames HEADER TERMINATOR body start  */
#line 215 "parser.y"
                {
                    rootP = newnode(lineno, curBankP->cur_pc, HEADER, (yyvsp[-1].pnodeP), (yyvsp[0].pnodeP));
                    rootP->value.strP = (yyvsp[-3].strP);
                }
#line 1546 "y.tab.c"
    break;

  case 7: /* start: START simple_expr TERMINATOR  */
#line 229 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, START, NILP, NILP);
                    (yyval.pnodeP)->value.ival = evalExpr((yyvsp[-1].pnodeP));
                }
#line 1555 "y.tab.c"
    break;

  case 8: /* start: STOP TERMINATOR  */
#line 234 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, STOP, NILP, NILP);
                }
#line 1563 "y.tab.c"
    break;

  case 9: /* body: stmt_list  */
#line 239 "parser.y"
                {
                SymListP symlistP;

                    if( (yyvsp[0].pnodeP) )
                    {
                        // Place any constants and variables that were never emitted by an
                        // explicit directive.
                        // They go at the end of their own bank, constants first, then variables.
                        // That is the order every code generator emits them in,
                        // and the start of each block is recorded in the bank context
                        // so no generator has to infer it from cur_pc.
                        for(BankContextP bp = banksP; bp; bp = bp->nextP)
                        {
                            curBankP = bp;

                            if( bp->constSymP )
                            {
                                bp->constPC = bp->cur_pc;
                                bp->cur_pc = setConstPC(bp->cur_pc, bp->constSymP);
                                constsListP = addToSymlist(constsListP, bp->constSymP,
                                    bp->bank, bp->constPC);
                            }

                            if( bp->varNodesP )
                            {
                                bp->varPC = bp->cur_pc;
                                setVarsPC(bp->bank, bp->varNodesP);
                            }
                        }
                        curBankP = findBank(curBank);

                        // Fix any wildcarded brefs
                        resolveWildcards(wildcardsP, banksP);

                        // Resolve all constant values
                        for( symlistP = constsListP; symlistP; symlistP = symlistP->nextP )
                        {
                            setConstVal(symlistP->symP);
                        }

                        (yyval.pnodeP) = (yyvsp[0].pnodeP)->leftP;        /* recover head link */
                        (yyvsp[0].pnodeP)->leftP = NILP;
                    }
                    else
                    {
                        (yyval.pnodeP) = NILP;
                    }
                }
#line 1616 "y.tab.c"
    break;

  case 10: /* stmt_list: stmt terminator  */
#line 290 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);

                    if( (yyvsp[-1].pnodeP) )
                    {
                        (yyvsp[0].pnodeP)->leftP = (yyvsp[-1].pnodeP);        /* keep head */
                        (yyvsp[-1].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    }
                    else
                    {
                        (yyvsp[0].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    }
                }
#line 1634 "y.tab.c"
    break;

  case 11: /* stmt_list: terminator  */
#line 304 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    (yyvsp[0].pnodeP)->leftP = (yyvsp[0].pnodeP);
                }
#line 1643 "y.tab.c"
    break;

  case 12: /* stmt_list: stmt_list terminator  */
#line 309 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    if( (yyvsp[-1].pnodeP) )
                    {
                        (yyval.pnodeP)->leftP = (yyvsp[-1].pnodeP)->leftP;
                        (yyvsp[-1].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    }
                }
#line 1656 "y.tab.c"
    break;

  case 13: /* stmt_list: stmt_list stmt terminator  */
#line 318 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    if( (yyvsp[-1].pnodeP) )
                    {
                        (yyvsp[0].pnodeP)->leftP = (yyvsp[-1].pnodeP);
                        (yyvsp[-1].pnodeP)->leftP = (yyvsp[0].pnodeP);
                        if( (yyvsp[-2].pnodeP) )
                        {
                            (yyval.pnodeP)->leftP = (yyvsp[-2].pnodeP)->leftP;
                            (yyvsp[-2].pnodeP)->leftP = (yyvsp[-1].pnodeP);
                        }
                    }
                    else if( (yyvsp[-2].pnodeP) )
                    {
                        (yyval.pnodeP)->leftP = (yyvsp[-2].pnodeP)->leftP;
                        (yyvsp[-2].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    }
                    else
                    {
                        (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    }
                }
#line 1683 "y.tab.c"
    break;

  case 14: /* stmt: one_stmt  */
#line 349 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    atSol = false;
                }
#line 1692 "y.tab.c"
    break;

  case 15: /* one_stmt: expr  */
#line 355 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, EXPR, NILP, (yyvsp[0].pnodeP));
                    checkPCBound("Code", curBankP->cur_pc, (yyval.pnodeP)->lineNo);
                    if( (yyvsp[0].pnodeP) && !((yyvsp[0].pnodeP)->flags & PN_NOINC) )
                    {
                        ++curBankP->cur_pc;
                    }
                }
#line 1705 "y.tab.c"
    break;

  case 16: /* one_stmt: BANK INTEGER  */
#line 364 "parser.y"
                {
                    if( ((yyvsp[0].ival) < 0) || ((yyvsp[0].ival) > MAXBANK) )
                    {
                        verror("Bank number must be between 0-%d decimal, %o octal, %x hex",
                            MAXBANK, MAXBANK, MAXBANK);
                    }

                    if( localContextP )
                    {
                        verror("Bank cannot be used inside a local context");
                    }

                    (yyval.pnodeP) = newnode(lineno+1, curBankP->cur_pc, BANK, NILP, NILP);
                    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
                    swapBanks((yyvsp[0].ival));
                    (yyval.pnodeP)->value2.ival = curBankP->cur_pc;   // is the pc for the new bank
                }
#line 1727 "y.tab.c"
    break;

  case 17: /* one_stmt: VAR varnames  */
#line 382 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, VAR, NILP, (yyvsp[0].pnodeP));
                }
#line 1735 "y.tab.c"
    break;

  case 18: /* one_stmt: VARS  */
#line 386 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno+1, curBankP->cur_pc, VARS, NILP, NILP);
                    (yyval.pnodeP)->value.ptr = curBankP->varNodesP;
                    if( !curBankP->varNodesP )
                    {
                        vwarn(WARN_VARS, "no variables have been declareed, variables ignored");
                    }
                    else
                    {
                        setVarsPC(curBank, curBankP->varNodesP);
                        checkPCBound("Variables", curBankP->cur_pc - 1, (yyval.pnodeP)->lineNo);
                        curBankP->varNodesP = 0;
                    }
                }
#line 1754 "y.tab.c"
    break;

  case 19: /* one_stmt: simple_expr ORIGIN  */
#line 401 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, ORIGIN, NILP, NILP);
                    (yyval.pnodeP)->value.ival = curBankP->cur_pc = evalExpr((yyvsp[-1].pnodeP));
                }
#line 1763 "y.tab.c"
    break;

  case 20: /* $@1: %empty  */
#line 406 "parser.y"
                {
                    // Register the label's symbol here, before labelTrailer is parsed.
                    // This matters for a same-line self-reference like "foo, jmp foo".
                    // If registration were deferred until after labelTrailer,
                    // the lexer would not yet know "foo" when it scans the second occurrence.
                    //
                    // pendingLabelPC is saved here so the final action can
                    // use the label's starting address even if labelTrailer's
                    // TEXT/ASCII/TYPE340 alternatives advance curBankP->cur_pc before
                    // that action fires.
                    //
                    // Hack used by mactoam1 for symbols in defines.
                    // All new symbols are assumed local.
                    // We're defining this regular symbol in the local context.
                    pendingLabelPC = curBankP->cur_pc;
                    if( localContextP && (localContextP->flags == CTX_FORCELOCAL) )
                    {
                        pendingLabelSymP = addLocalSymbol((yyvsp[-1].strP));
                        pendingLabelSymP->flags = SYMF_RESOLVED | SYMF_FORCED | SYM_LOC;
                        pendingLabelSymP->value = curBankP->cur_pc;
                        pendingLabelLocType = LCLLOCATION;
                    }
                    else
                    {
                        pendingLabelSymP = sym_make((yyvsp[-1].strP), 0);
                        pendingLabelSymP->flags |= SYMF_RESOLVED | SYM_GLOB;
                        pendingLabelSymP->lineno = lineno - 1;
                        pendingLabelSymP->value = curBankP->cur_pc;
                        pendingLabelSymP->bank = curBank;
                        sym_add(&(curBankP->globalSymP), pendingLabelSymP);
                        pendingLabelLocType = LOCATION;
                    }
                }
#line 1801 "y.tab.c"
    break;

  case 21: /* one_stmt: NAME LOCATION $@1 labelTrailer  */
#line 440 "parser.y"
                {
                    // Use pendingLabelPC rather than curBankP->cur_pc here.
                    // If labelTrailer matched a TEXT/ASCII/TYPE340 alternative it already
                    // advanced curBankP->cur_pc and also set PN_NOINC on the node.
                    if( (yyvsp[0].pnodeP) && !((yyvsp[0].pnodeP)->flags & PN_NOINC) )
                    {
                        ++curBankP->cur_pc;
                    }
                    (yyval.pnodeP) = newnode(lineno, pendingLabelPC, pendingLabelLocType, NILP, (yyvsp[0].pnodeP));
                    (yyval.pnodeP)->value.symP = pendingLabelSymP;
                }
#line 1817 "y.tab.c"
    break;

  case 22: /* $@2: %empty  */
#line 452 "parser.y"
                {
                    // Capture curBankP->cur_pc before labelTrailer is parsed; a
                    // TEXT/ASCII/TYPE340 in labelTrailer will
                    // advance curBankP->cur_pc, and we need the label's starting
                    // address for both the symbol value and the node pc.
                    pendingLabelPC = curBankP->cur_pc;
                }
#line 1829 "y.tab.c"
    break;

  case 23: /* one_stmt: ADDR LOCATION $@2 labelTrailer  */
#line 460 "parser.y"
                {
                    if( (yyvsp[-3].symP)->flags & SYMF_RESOLVED )
                    {
                        verror("Duplicate label %s", (yyvsp[-3].symP)->name);
                    }
                    else
                    {
                        if( ((yyvsp[-3].symP)->flags & SYM_MASK) == SYM_LOC )
                        {
                            // This was from a local context when forced was in effect,
                            // fix it up.
                            (yyvsp[-3].symP)->flags = SYM_GLOB;
                            (yyvsp[-3].symP)->symP->flags = SYM_GLOB | SYMF_RESOLVED;
                            (yyvsp[-3].symP)->symP->value = pendingLabelPC;
                        }

                        (yyvsp[-3].symP)->lineno = lineno - 1;
                        (yyvsp[-3].symP)->flags |= SYMF_RESOLVED;
                        (yyvsp[-3].symP)->value = pendingLabelPC;
                        if( (yyvsp[0].pnodeP) && !((yyvsp[0].pnodeP)->flags & PN_NOINC) )
                        {
                            ++curBankP->cur_pc;
                        }
                        (yyval.pnodeP) = newnode(lineno, pendingLabelPC, LOCATION, NILP, (yyvsp[0].pnodeP));
                        (yyval.pnodeP)->value.symP = (yyvsp[-3].symP);
                    }
                }
#line 1861 "y.tab.c"
    break;

  case 24: /* $@3: %empty  */
#line 488 "parser.y"
                {
                    // Capture curBankP->cur_pc before labelTrailer is parsed.
                    pendingLabelPC = curBankP->cur_pc;
                }
#line 1870 "y.tab.c"
    break;

  case 25: /* one_stmt: LCLNAME LOCATION $@3 labelTrailer  */
#line 493 "parser.y"
                {
                SymNodeP symP;

                    if( !(symP = addLocalSymbol((yyvsp[-3].strP))) )
                    {
                        verror("local symbol used, but not inside a local scope");
                    }

                    symP->flags = SYMF_RESOLVED | SYM_LOC;
                    symP->value = pendingLabelPC;
                    if( (yyvsp[0].pnodeP) && !((yyvsp[0].pnodeP)->flags & PN_NOINC) )
                    {
                        ++curBankP->cur_pc;
                    }
                    (yyval.pnodeP) = newnode(lineno, pendingLabelPC, LCLLOCATION, NILP, (yyvsp[0].pnodeP));
                    (yyval.pnodeP)->value.symP = symP;
                }
#line 1892 "y.tab.c"
    break;

  case 26: /* $@4: %empty  */
#line 511 "parser.y"
                {  
                    // Capture curBankP->cur_pc before labelTrailer is parsed.
                    pendingLabelPC = curBankP->cur_pc;
                }
#line 1901 "y.tab.c"
    break;

  case 27: /* one_stmt: LCLADDR LOCATION $@4 labelTrailer  */
#line 516 "parser.y"
                {
                    if( ((yyvsp[-3].symP)->value2 < localDepth) && !((yyvsp[-3].symP)->flags & SYMF_PRIVATE) )
                    {
                        verror(
                    "local label %s is defined in outer scope %d, this is scope %d, can't be declared here",
                            (yyvsp[-3].symP)->name, (yyvsp[-3].symP)->value2, localDepth);
                    }
                    else if( (yyvsp[-3].symP)->flags & SYMF_RESOLVED )
                    {
                        verror("Duplicate local label %s", (yyvsp[-3].symP)->name);
                    }
                    else
                    {
                        (yyvsp[-3].symP)->flags |= SYMF_RESOLVED;
                        (yyvsp[-3].symP)->value = pendingLabelPC;
                        if( (yyvsp[0].pnodeP) && !((yyvsp[0].pnodeP)->flags & PN_NOINC) )
                        {
                            ++curBankP->cur_pc;
                        }
                        (yyval.pnodeP) = newnode(lineno, pendingLabelPC, LCLLOCATION, NILP, (yyvsp[0].pnodeP));
                        (yyval.pnodeP)->value.symP = (yyvsp[-3].symP);
                    }
                }
#line 1929 "y.tab.c"
    break;

  case 28: /* one_stmt: CONSTANTS  */
#line 540 "parser.y"
                {
                SymListP symlistP;
                BankContextP ctxP;
                    // End this constant scope, if there is one, but include the node for listings
                    (yyval.pnodeP) = newnode(lineno+1, curBankP->cur_pc, CONSTANTS, NILP, NILP);

                    if( curBankP->constSymP )
                    {
                        constsListP = addToSymlist(constsListP, curBankP->constSymP, curBank, curBankP->cur_pc);
                        (yyval.pnodeP)->value.symP = curBankP->constSymP;
                        curBankP->cur_pc = setConstPC(curBankP->cur_pc, curBankP->constSymP);
                        sym_init(&(curBankP->constSymP));

                        // Be sure we clear from our bank context, if we have one
                        if( (ctxP = findBank(curBank)) )
                        {
                            ctxP->constSymP = NILP;
                        }
                    }
                }
#line 1954 "y.tab.c"
    break;

  case 29: /* one_stmt: ASCII STRING  */
#line 561 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno+1, curBankP->cur_pc, ASCII, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                    curBankP->cur_pc += countAscii((yyvsp[0].strP));
                    checkPCBound("Ascii", curBankP->cur_pc - 1, (yyval.pnodeP)->lineNo);
                }
#line 1965 "y.tab.c"
    break;

  case 30: /* one_stmt: TYPE340 T340STRING  */
#line 568 "parser.y"
                {
                    // Will aready have been converted in the lexer.
                    // We reuse the Flex struct because this is also a counted-length string.
                    (yyval.pnodeP) = newnode(lineno+1, curBankP->cur_pc, TYPE340, NILP, NILP);
                    (yyval.pnodeP)->value.flexText = (yyvsp[0].flexText);
                    curBankP->cur_pc += countText((yyvsp[0].flexText));
                    checkPCBound("Type340", curBankP->cur_pc - 1, (yyval.pnodeP)->lineNo);
                }
#line 1978 "y.tab.c"
    break;

  case 31: /* one_stmt: TEXT  */
#line 577 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno+1, curBankP->cur_pc, TEXT, NILP, NILP);
                    (yyval.pnodeP)->value.flexText = (yyvsp[0].flexText);
                    curBankP->cur_pc += countText((yyvsp[0].flexText));
                    checkPCBound("Text", curBankP->cur_pc - 1, (yyval.pnodeP)->lineNo);
                }
#line 1989 "y.tab.c"
    break;

  case 32: /* one_stmt: TABLE simple_expr  */
#line 584 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, TABLE, NILP, NILP);
                    (yyval.pnodeP)->value.ival = evalExpr((yyvsp[0].pnodeP));
                    curBankP->cur_pc += (yyval.pnodeP)->value.ival;
                    checkPCBound("Table", curBankP->cur_pc - 1, (yyval.pnodeP)->lineNo);
                }
#line 2000 "y.tab.c"
    break;

  case 33: /* one_stmt: TABLE simple_expr LOCATION simple_expr  */
#line 591 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, TABLE, NILP, (yyvsp[0].pnodeP));
                    (yyval.pnodeP)->value.ival = evalExpr((yyvsp[-2].pnodeP));
                    curBankP->cur_pc += (yyval.pnodeP)->value.ival;
                    checkPCBound("Table", curBankP->cur_pc - 1, (yyval.pnodeP)->lineNo);
                }
#line 2011 "y.tab.c"
    break;

  case 34: /* one_stmt: EXPORT symList  */
#line 598 "parser.y"
                {
                SymNodeP symP;
                PNodeP nodeP;

                    nodeP = (yyvsp[0].pnodeP)->leftP;      // recover head link
                    (yyvsp[0].pnodeP)->leftP = NILP;

                    addExports(nodeP);
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, EXPORT, NILP, nodeP);
                    (yyval.pnodeP)->flags |= PN_NOINC;
                    doSymtab = true;        // and force output
                }
#line 2028 "y.tab.c"
    break;

  case 35: /* one_stmt: IMPORT STRING  */
#line 611 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, IMPORT, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                    importSymbols((yyvsp[0].strP));
                }
#line 2038 "y.tab.c"
    break;

  case 36: /* one_stmt: IMPORT LIBFILE  */
#line 617 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, IMPORT, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                    importSymbols((yyvsp[0].strP));
                }
#line 2048 "y.tab.c"
    break;

  case 37: /* one_stmt: CSSTART CSCOMMENT  */
#line 623 "parser.y"
                {
                    (yyval.pnodeP) = newnode((yyvsp[-1].ival), curBankP->cur_pc, CSCOMMENT, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                }
#line 2057 "y.tab.c"
    break;

  case 38: /* terminator: terminators  */
#line 630 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    atSol = true;
                }
#line 2066 "y.tab.c"
    break;

  case 39: /* terminator: SEMI  */
#line 635 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, SEMI, NILP, NILP);
                }
#line 2074 "y.tab.c"
    break;

  case 40: /* terminator: COMMENT  */
#line 639 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, COMMENT, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                    if( atSol )
                    {
                        (yyval.pnodeP)->flags |= PN_SOL;
                    }
                    atSol = true;
                }
#line 2088 "y.tab.c"
    break;

  case 41: /* terminator: FILENAME  */
#line 649 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, FILENAME, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                    if( dropIncludeText && !fileIsMain((yyvsp[0].strP)) )
                    {
                        (yyval.pnodeP)->flags |= PN_NOTEXT;
                    }
                    atSol = true;
                }
#line 2102 "y.tab.c"
    break;

  case 42: /* terminators: TERMINATOR  */
#line 661 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, TERMINATOR, NILP, NILP);
                }
#line 2110 "y.tab.c"
    break;

  case 43: /* terminators: EMPTYLINE  */
#line 665 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, EMPTYLINE, NILP, NILP);
                }
#line 2118 "y.tab.c"
    break;

  case 44: /* terminators: terminators TERMINATOR  */
#line 669 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[-1].pnodeP);
                }
#line 2126 "y.tab.c"
    break;

  case 45: /* terminators: terminators EMPTYLINE  */
#line 673 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[-1].pnodeP);
                }
#line 2134 "y.tab.c"
    break;

  case 46: /* optExpr: expr  */
#line 679 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                }
#line 2142 "y.tab.c"
    break;

  case 47: /* optExpr: %empty  */
#line 683 "parser.y"
                {
                    // empty
            (yyval.pnodeP) = NILP;
                }
#line 2151 "y.tab.c"
    break;

  case 48: /* labelTrailer: optExpr  */
#line 690 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                }
#line 2159 "y.tab.c"
    break;

  case 49: /* labelTrailer: ASCII STRING  */
#line 694 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno+1, pendingLabelPC, ASCII, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                    curBankP->cur_pc += countAscii((yyvsp[0].strP));
                    checkPCBound("Ascii", curBankP->cur_pc - 1, (yyval.pnodeP)->lineNo);
                    (yyval.pnodeP)->flags |= PN_NOINC;
                }
#line 2171 "y.tab.c"
    break;

  case 50: /* labelTrailer: TYPE340 T340STRING  */
#line 702 "parser.y"
                {
                    // Will already have been converted in the lexer.
                    (yyval.pnodeP) = newnode(lineno+1, pendingLabelPC, TYPE340, NILP, NILP);
                    (yyval.pnodeP)->value.flexText = (yyvsp[0].flexText);
                    curBankP->cur_pc += countText((yyvsp[0].flexText));
                    checkPCBound("Type340", curBankP->cur_pc - 1, (yyval.pnodeP)->lineNo);
                    (yyval.pnodeP)->flags |= PN_NOINC;
                }
#line 2184 "y.tab.c"
    break;

  case 51: /* labelTrailer: TEXT  */
#line 711 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno+1, pendingLabelPC, TEXT, NILP, NILP);
                    (yyval.pnodeP)->value.flexText = (yyvsp[0].flexText);
                    curBankP->cur_pc += countText((yyvsp[0].flexText));
                    checkPCBound("Text", curBankP->cur_pc - 1, (yyval.pnodeP)->lineNo);
                    (yyval.pnodeP)->flags |= PN_NOINC;
                }
#line 2196 "y.tab.c"
    break;

  case 52: /* optINTEGER: INTEGER  */
#line 721 "parser.y"
                {
                    (yyval.ival) = (yyvsp[0].ival);
                }
#line 2204 "y.tab.c"
    break;

  case 53: /* optINTEGER: %empty  */
#line 725 "parser.y"
                {
                    (yyval.ival) = -1;
                }
#line 2212 "y.tab.c"
    break;

  case 54: /* expr: simple_expr  */
#line 730 "parser.y"
                                            { (yyval.pnodeP) = (yyvsp[0].pnodeP); }
#line 2218 "y.tab.c"
    break;

  case 55: /* expr: directive_expr  */
#line 731 "parser.y"
                                            { (yyval.pnodeP) = (yyvsp[0].pnodeP); }
#line 2224 "y.tab.c"
    break;

  case 56: /* expr: LAW  */
#line 733 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, LAW, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2233 "y.tab.c"
    break;

  case 57: /* expr: LAW optSEPARATOR simple_expr  */
#line 738 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, LAW, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[-2].symP);
                    (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, SEPARATOR, (yyval.pnodeP), (yyvsp[0].pnodeP));
                }
#line 2243 "y.tab.c"
    break;

  case 60: /* simple_expr: simple_expr SEPARATOR simple_expr  */
#line 749 "parser.y"
                                                    { (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, SEPARATOR, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2249 "y.tab.c"
    break;

  case 61: /* simple_expr: MINUS simple_expr  */
#line 750 "parser.y"
                                                   { (yyval.pnodeP) = unop(lineno, curBankP->cur_pc, UMINUS, (yyvsp[0].pnodeP)); }
#line 2255 "y.tab.c"
    break;

  case 62: /* simple_expr: simple_expr PLUS simple_expr  */
#line 751 "parser.y"
                                                   { (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, PLUS, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2261 "y.tab.c"
    break;

  case 63: /* simple_expr: simple_expr MINUS simple_expr  */
#line 752 "parser.y"
                                                   { (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, MINUS, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2267 "y.tab.c"
    break;

  case 64: /* simple_expr: simple_expr MUL simple_expr  */
#line 753 "parser.y"
                                                   { (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, MUL, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2273 "y.tab.c"
    break;

  case 65: /* simple_expr: simple_expr DIV simple_expr  */
#line 754 "parser.y"
                                                   { (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, DIV, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2279 "y.tab.c"
    break;

  case 66: /* simple_expr: simple_expr MOD simple_expr  */
#line 755 "parser.y"
                                                   { (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, MOD, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2285 "y.tab.c"
    break;

  case 67: /* simple_expr: simple_expr AND simple_expr  */
#line 756 "parser.y"
                                                   { (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, AND, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2291 "y.tab.c"
    break;

  case 68: /* simple_expr: simple_expr OR simple_expr  */
#line 757 "parser.y"
                                                   { (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, OR, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2297 "y.tab.c"
    break;

  case 69: /* simple_expr: simple_expr XOR simple_expr  */
#line 758 "parser.y"
                                                   { (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, XOR, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2303 "y.tab.c"
    break;

  case 70: /* simple_expr: simple_expr LSHIFT simple_expr  */
#line 759 "parser.y"
                                                   { (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, LSHIFT, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2309 "y.tab.c"
    break;

  case 71: /* simple_expr: simple_expr RSHIFT simple_expr  */
#line 760 "parser.y"
                                                   { (yyval.pnodeP) = binop(lineno, curBankP->cur_pc, RSHIFT, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2315 "y.tab.c"
    break;

  case 72: /* simple_expr: '(' simple_expr ')'  */
#line 761 "parser.y"
                                                   { (yyval.pnodeP) = unop(lineno, curBankP->cur_pc, PARENS, (yyvsp[-1].pnodeP)); }
#line 2321 "y.tab.c"
    break;

  case 73: /* simple_expr: CMPL simple_expr  */
#line 762 "parser.y"
                                                   { (yyval.pnodeP) = unop(lineno, curBankP->cur_pc, CMPL, (yyvsp[0].pnodeP)); }
#line 2327 "y.tab.c"
    break;

  case 74: /* simple_expr: INTEGER  */
#line 764 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, INTEGER, NILP, NILP);
                    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
                }
#line 2336 "y.tab.c"
    break;

  case 75: /* simple_expr: THISBANK  */
#line 769 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, INTEGER, NILP, NILP);
                    (yyval.pnodeP)->value.ival = curBankP->bank << 12;
                }
#line 2345 "y.tab.c"
    break;

  case 76: /* simple_expr: OPCODE  */
#line 774 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, OPCODE, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2354 "y.tab.c"
    break;

  case 77: /* simple_expr: OPADDR  */
#line 779 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, OPADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2363 "y.tab.c"
    break;

  case 78: /* simple_expr: OPORABLE  */
#line 784 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, OPORABLE, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                    if( doMacro && ((yyvsp[0].symP)->flags & SYMF_1DOP) )
                    {
                        vwarn(WARN_1D, "%s is a PDP-1D instruction", (yyvsp[0].symP)->name );
                    }
                }
#line 2376 "y.tab.c"
    break;

  case 79: /* simple_expr: VALUESPEC  */
#line 793 "parser.y"
                {
                    if( (yyvsp[0].symP)->flags & SYMF_INDIRECT )
                    {
                        (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, IMOD, NILP, NILP);
                    }
                    else
                    {
                        (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, VALUESPEC, NILP, NILP);
                    }

                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2393 "y.tab.c"
    break;

  case 80: /* simple_expr: CONSTANT simple_expr endConst  */
#line 806 "parser.y"
                {
                int hash;
                SymNodeP symP;
                char *nameP;

                    // Jump thru hoops for constant compression
                    sprintf(scratchStr,"%ld",hashExpr((yyvsp[-1].pnodeP)));
                    if( !(symP = sym_find(&(curBankP->constSymP), scratchStr)) )
                    {
                        nameP = malloc(strlen(scratchStr) + 1);
                        strcpy(nameP, scratchStr);
                        symP = sym_make(nameP, 0);
                        sym_add(&(curBankP->constSymP), symP);
                        symP->ptr = (yyvsp[-1].pnodeP);
                    }
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, CONSTANT, NILP, (yyvsp[0].pnodeP));
                    (yyval.pnodeP)->value.symP = symP;

                    // Keep this reference's expression tree on the node.
                    // The pooled symbol's ->ptr holds only the expression that
                    // was placed into the slot first, and literals are pooled
                    // by value, so every later reference whose value coincides
                    // shares the same slot and would otherwise be listed
                    // under an unrelated symbol, complete with the wrong bank
                    // qualifier.
                    (yyval.pnodeP)->value2.ptr = (yyvsp[-1].pnodeP);
                }
#line 2425 "y.tab.c"
    break;

  case 81: /* simple_expr: DOT  */
#line 834 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, DOT, NILP, NILP);
                    (yyval.pnodeP)->value.ival = curBankP->cur_pc;
                }
#line 2434 "y.tab.c"
    break;

  case 82: /* simple_expr: ADDR  */
#line 839 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, ADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2443 "y.tab.c"
    break;

  case 83: /* simple_expr: INTEGER bref  */
#line 844 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, INTEGER, NILP, NILP);
                    (yyval.pnodeP)->value.ival = (yyvsp[-1].ival) + ((yyvsp[0].ival) << 12);
                }
#line 2452 "y.tab.c"
    break;

  case 84: /* simple_expr: NAME bref  */
#line 849 "parser.y"
                {
                BankContextP bankP;
                SymNodeP symP;

                    if( (yyvsp[0].ival) != curBank )
                    {
                        symP = findSymbolInBank((yyvsp[0].ival), (yyvsp[-1].strP));
                    }
                    else
                    {
                       // This is just a regular global in the current bank
                        symP = sym_make((yyvsp[-1].strP), 0);
                        symP->bank = curBank;
                        sym_add(&(curBankP->globalSymP), symP);
                        symP->flags = SYM_GLOB;
                    }

                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, BREF, NILP, NILP);
                    (yyval.pnodeP)->value.symP = symP;
                    (yyval.pnodeP)->value2.ival = (yyvsp[0].ival);
                }
#line 2478 "y.tab.c"
    break;

  case 85: /* simple_expr: ADDR bref  */
#line 871 "parser.y"
                {
                SymNodeP symP;

                    if( (yyvsp[0].ival) != curBank )
                    {
                        symP = findSymbolInBank((yyvsp[0].ival), (yyvsp[-1].symP)->name);
                    }
                    else
                    {
                        // This is a symbol in our own bank, but that's ok
                        symP = (yyvsp[-1].symP);
                    }

                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, BREF, NILP, NILP);
                    (yyval.pnodeP)->value.symP = symP;
                    (yyval.pnodeP)->value2.ival = (yyvsp[0].ival);
                }
#line 2500 "y.tab.c"
    break;

  case 86: /* simple_expr: NAME wildref  */
#line 889 "parser.y"
                {
                PNodeListP wildP;

                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, WILDREF, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[-1].strP);

                    // We need this for fixup later
                    wildP = (PNodeListP)malloc(sizeof(PNodeListItem));
                    wildP->nodeP = (yyval.pnodeP);
                    wildP->nextP = wildcardsP;
                    wildcardsP = wildP;
                }
#line 2517 "y.tab.c"
    break;

  case 87: /* simple_expr: ADDR wildref  */
#line 902 "parser.y"
                {
                SymNodeP symP;
                PNodeListP wildP;

                    if( (yyvsp[-1].symP)->bank == curBank )
                    {
                        // This is a symbol in our own bank, resolve it now
                        symP = (yyvsp[-1].symP);
                        (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, BREF, NILP, NILP);
                        (yyval.pnodeP)->value.symP = (yyvsp[-1].symP);
                        (yyval.pnodeP)->value2.ival = curBank;
                    }
                    else
                    {
                        // In another bank, usual wildcard processing
                        (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, WILDREF, NILP, NILP);
                        (yyval.pnodeP)->value.strP = (yyvsp[-1].symP)->name;

                        // We need this for fixup later
                        wildP = (PNodeListP)malloc(sizeof(PNodeListItem));
                        wildP->nodeP = (yyval.pnodeP);
                        wildP->nextP = wildcardsP;
                        wildcardsP = wildP;
                    }
                }
#line 2547 "y.tab.c"
    break;

  case 88: /* simple_expr: NAME  */
#line 928 "parser.y"
                {
                SymNodeP symP, symP2;

                    // a symbol we haven't seen yet, add to the global symtab
                    // unless forcelocal is in effect, then add to locals and globals
                    if( localContextP && (localContextP->flags == CTX_FORCELOCAL) )
                    {
                        symP = addLocalSymbol((yyvsp[0].strP));
                        // We add a new sym to globals with a ref to the local
                        symP2 = sym_make((yyvsp[0].strP), 0);
                        symP2->symP = symP;
                        symP2->bank = curBank;
                        symP2->flags = SYM_LOC;
                        sym_add(&(curBankP->globalSymP), symP2);
                        symP->flags = symP2->flags = SYMF_FORCED | SYM_LOC;
                        (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, LCLADDR, NILP, NILP);
                    }
                    else
                    {
                        symP = sym_make((yyvsp[0].strP), 0);
                        symP->bank = curBank;
                        sym_add(&(curBankP->globalSymP), symP);
                        symP->flags = SYM_GLOB;
                        (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, ADDR, NILP, NILP);
                    }

                    (yyval.pnodeP)->value.symP = symP;
                }
#line 2580 "y.tab.c"
    break;

  case 89: /* simple_expr: LCLNAME  */
#line 957 "parser.y"
                {
                SymNodeP symP;

                    // a symbol we haven't seen yet, add to the local symtab
                    if( !localContextP )
                    {
                        verror("local %s used outside a local scope", (yyvsp[0].strP));
                    }

                    symP = addLocalSymbol((yyvsp[0].strP));
                    symP->flags = SYM_LOC;
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, LCLADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = symP;
                }
#line 2599 "y.tab.c"
    break;

  case 90: /* simple_expr: LCLADDR  */
#line 972 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, LCLADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2608 "y.tab.c"
    break;

  case 91: /* simple_expr: FLEXO  */
#line 977 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, FLEXO, NILP, NILP);
                    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
                }
#line 2617 "y.tab.c"
    break;

  case 92: /* simple_expr: CHAR  */
#line 982 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, CHAR, NILP, NILP);
                    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
                }
#line 2626 "y.tab.c"
    break;

  case 93: /* simple_expr: LITCHAR  */
#line 987 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, LITCHAR, NILP, NILP);
                    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
                }
#line 2635 "y.tab.c"
    break;

  case 94: /* directive_expr: FORCELOC  */
#line 994 "parser.y"
                {
                    if( localDepth == 0 )
                    {
                        verror("%%forcelocal without an opening local");
                    }

                    localContextP->flags = CTX_FORCELOCAL;
                    sawForceLocal = true;
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, FORCELOC, NILP, NILP);
                    (yyval.pnodeP)->flags |= PN_NOINC;
                }
#line 2651 "y.tab.c"
    break;

  case 95: /* directive_expr: LOCorADDLOCorPRIVATE optLocals  */
#line 1006 "parser.y"
                {
                SymNodeP symP;
                PNodeP nodeP;
                char *cP;

                    // This can be a local, private, or an addlocal.
                    // If local, push any current local scope, establish a new one.
                    // locaSymlPP can be null if there is no current scope.
                    // If addlocal, the scope must exist and the symbols are added to it
                    // If private, if a scope exists, add to it, otherwise establish a new one.
                    if( (yyvsp[0].pnodeP) )
                    {
                        nodeP = (yyvsp[0].pnodeP)->leftP;      // recover head link
                        (yyvsp[0].pnodeP)->leftP = NILP;
                    }
                    else
                    {
                        nodeP = NILP;
                    }

                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, (yyvsp[-1].ival), NILP, nodeP);
                    (yyval.pnodeP)->flags |= PN_NOINC;

                    if( (yyvsp[-1].ival) == LOCAL )
                    {
                        localStack[localDepth++] = localContextP;
                        if( localDepth > maxLocalDepth )
                        {
                            maxLocalDepth = localDepth;
                        }

                        localContextP = newLocalContext();
                        localContextP->pc = curBankP->cur_pc;    // will be the origin for any local relative refs
                        sym_init( &(localContextP->symRootP) );
                    }
                    else if( (yyvsp[-1].ival) == ADDLOCAL )
                    {
                        if( !localContextP )
                        {
                            verror("addlocal but not in a local context");
                        }
                    }
                    else if( (yyvsp[-1].ival) == PRIVATE )
                    {
                        // If there's not a context, start one.
                        if( !localContextP )
                        {
                            localStack[localDepth++] = localContextP;
                            if( localDepth > maxLocalDepth )
                            {
                                maxLocalDepth = localDepth;
                            }

                            localContextP = newLocalContext();
                            localContextP->pc = curBankP->cur_pc;
                            sym_init( &(localContextP->symRootP) );
                        }
                    }

                    while( nodeP )         // add local predefines
                    {
                        if( nodeP->type == ADDR )
                        {
                            cP = nodeP->value.symP->name;   // already a global by this name
                        }
                        else
                        {
                            cP = nodeP->value.strP;
                        }

                        symP = addLocalSymbol(cP);
                        symP->flags = SYM_LOC;
                        if( (yyvsp[-1].ival) == PRIVATE )
                        {
                            symP->flags |= SYMF_PRIVATE;
                        }

                        nodeP = nodeP->leftP;
                    }
                }
#line 2736 "y.tab.c"
    break;

  case 96: /* directive_expr: ENDLOC optINTEGER  */
#line 1087 "parser.y"
                {
                    if( (yyvsp[0].ival) > 0 )
                    {
                        if( (yyvsp[0].ival) != localDepth )
                        {
                            vwarn(WARN_LOCALS, "endloc says ending level %d but the current level is %d",
                                (yyvsp[0].ival), localDepth);
                        }
                    }

                    // We pop the local stack
                    if( localDepth == 0 )
                    {
                        verror("endloc without an opening local");
                    }
                    else
                    {
                        localContextP = localStack[--localDepth];
                    }

                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, ENDLOC, NILP, NILP);
                    (yyval.pnodeP)->flags |= PN_NOINC;
                    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
                }
#line 2765 "y.tab.c"
    break;

  case 97: /* endConst: ENDCONST  */
#line 1114 "parser.y"
                {
                    (yyval.pnodeP) = NILP;
                }
#line 2773 "y.tab.c"
    break;

  case 98: /* endConst: COMMENT  */
#line 1118 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, COMMENT, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                }
#line 2782 "y.tab.c"
    break;

  case 99: /* LOCorADDLOCorPRIVATE: LOCAL  */
#line 1125 "parser.y"
                {
                    (yyval.ival) = LOCAL;
                }
#line 2790 "y.tab.c"
    break;

  case 100: /* LOCorADDLOCorPRIVATE: ADDLOCAL  */
#line 1129 "parser.y"
                {
                    (yyval.ival) = ADDLOCAL;
                }
#line 2798 "y.tab.c"
    break;

  case 101: /* LOCorADDLOCorPRIVATE: PRIVATE  */
#line 1133 "parser.y"
                {
                    (yyval.ival) = PRIVATE;
                }
#line 2806 "y.tab.c"
    break;

  case 102: /* optLocals: localSymDef  */
#line 1139 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    (yyval.pnodeP)->leftP = (yyval.pnodeP); // keep head
                }
#line 2815 "y.tab.c"
    break;

  case 103: /* optLocals: optLocals LOCATION localSymDef  */
#line 1144 "parser.y"
                {
                    (yyvsp[0].pnodeP)->leftP = (yyvsp[-2].pnodeP)->leftP;
                    (yyvsp[-2].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                }
#line 2825 "y.tab.c"
    break;

  case 104: /* optLocals: %empty  */
#line 1150 "parser.y"
                {
                    (yyval.pnodeP) = NILP;
                }
#line 2833 "y.tab.c"
    break;

  case 105: /* localSymDef: symbol  */
#line 1156 "parser.y"
                {
                    if( (yyvsp[0].pnodeP)->type == ADDR )
                    {
                        vwarn(WARN_LOCALS, "local %s will hide a global of the same name",(yyvsp[0].pnodeP)->value.symP->name);
                    }

                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                }
#line 2846 "y.tab.c"
    break;

  case 106: /* localSymDef: LCLADDR  */
#line 1165 "parser.y"
                {
                    if( (yyvsp[0].symP)->value2 > localDepth )
                    {
                        verror(
                            "local %s is already defined in this scope %d, can't be declared here",
                            (yyvsp[0].symP)->name, (yyvsp[0].symP)->value2, localDepth);
                    }

                    // We are defining a nested local with the same name as an outer one,
                    // that's fine, but warn them.
                    vwarn(WARN_LOCALS, "local %s will hide a local of the same name defined in scope %d",
                        (yyvsp[0].symP)->name, (yyvsp[0].symP)->value2);

                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, NAME, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].symP)->name;
                }
#line 2867 "y.tab.c"
    break;

  case 107: /* symList: symbol  */
#line 1184 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    (yyval.pnodeP)->leftP = (yyval.pnodeP); // keep head
                }
#line 2876 "y.tab.c"
    break;

  case 108: /* symList: symList LOCATION symbol  */
#line 1189 "parser.y"
                {
                    (yyvsp[0].pnodeP)->leftP = (yyvsp[-2].pnodeP)->leftP;
                    (yyvsp[-2].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                }
#line 2886 "y.tab.c"
    break;

  case 109: /* symbol: NAME  */
#line 1197 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, NAME, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                }
#line 2895 "y.tab.c"
    break;

  case 110: /* symbol: ADDR  */
#line 1202 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, ADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2904 "y.tab.c"
    break;

  case 111: /* varnames: var  */
#line 1209 "parser.y"
                {
                PNodeListP varP;

                    (yyval.pnodeP) = (yyvsp[0].pnodeP);

                    // Chain it into the list of unemitted vars
                    varP = (PNodeListP)malloc(sizeof(PNodeListItem));
                    varP->nodeP = (yyval.pnodeP);
                    varP->nextP = curBankP->varNodesP;
                    curBankP->varNodesP = varP;
                }
#line 2920 "y.tab.c"
    break;

  case 112: /* varnames: varnames LOCATION var  */
#line 1221 "parser.y"
                {
                PNodeListP varP;

                    (yyvsp[0].pnodeP)->rightP = (yyvsp[-2].pnodeP);
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);

                    // And chain in the new var
                    varP = (PNodeListP)malloc(sizeof(PNodeListItem));
                    varP->nodeP = (yyvsp[0].pnodeP);
                    varP->nextP = curBankP->varNodesP;
                    curBankP->varNodesP = varP;
                }
#line 2937 "y.tab.c"
    break;

  case 113: /* bref: BREF INTEGER  */
#line 1236 "parser.y"
                {
                    if( ((yyvsp[0].ival) < 0) || ((yyvsp[0].ival) > 15) )
                    {
                        verror("bank number must be 0-15 decimal, 0-17 octal");
                    }

                    (yyval.ival) = (yyvsp[0].ival);
                }
#line 2950 "y.tab.c"
    break;

  case 114: /* bref: BREF DOT  */
#line 1245 "parser.y"
                {
                    (yyval.ival) = curBank;        // dot is a marker to indicate 'this bank'
                }
#line 2958 "y.tab.c"
    break;

  case 116: /* var: varname  */
#line 1254 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                }
#line 2966 "y.tab.c"
    break;

  case 117: /* var: varname '=' simple_expr  */
#line 1258 "parser.y"
                {
                    (yyvsp[-2].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    (yyval.pnodeP) = (yyvsp[-2].pnodeP);
                }
#line 2975 "y.tab.c"
    break;

  case 118: /* varname: NAME  */
#line 1264 "parser.y"
                {
                SymNodeP symP;

                    symP = sym_make((yyvsp[0].strP), 0);
                    symP->bank = curBank;
                    sym_add(&(curBankP->globalSymP), symP);
                    symP->flags = SYM_GLOB | SYMF_VAR;
                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, ADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = symP;
                }
#line 2990 "y.tab.c"
    break;

  case 119: /* varname: ADDR  */
#line 1275 "parser.y"
                {
                    if( (yyvsp[0].symP)->flags & SYMF_RESOLVED )
                    {
                        verror("variable %s is already declared", (yyvsp[0].symP)->name);
                    }

                    (yyval.pnodeP) = newnode(lineno, curBankP->cur_pc, ADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                    (yyvsp[0].symP)->lineno = lineno - 1;
                    (yyvsp[0].symP)->flags = SYM_GLOB | SYMF_VAR;
                }
#line 3006 "y.tab.c"
    break;


#line 3010 "y.tab.c"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      yyerror (YY_("syntax error"));
    }

  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif

  return yyresult;
}

#line 1286 "parser.y"


// Add exported symbols to the current global symtab
void
addExports(PNodeP nodesP)
{
SymNodeP symP;

    while( nodesP )
    {
        switch( nodesP->type )
        {
        case NAME:
            symP = sym_make(nodesP->value.strP, 0);     // first time seen, declare it as a global
            symP->bank = curBank;
            sym_add(&(curBankP->globalSymP), symP);
            symP->flags = SYM_GLOB | SYMF_EXPORTED;
            break;

        case ADDR:
            nodesP->value.symP->flags |= SYMF_EXPORTED;
            break;
        }

        nodesP = nodesP->leftP;
    }
}

// Walk a symbol table of constants, set the pc for each,
// return the updated pc.
int
setConstPC(int pc, SymNodeP symP)
{
    if( !symP )
    {
        return(pc);
    }

    if( !(symP->flags & SYMF_ASSIGNED) )
    {
        symP->value = pc++;
        symP->flags |= SYMF_ASSIGNED;
    }

    pc = setConstPC(pc, symP->leftP);
    pc = setConstPC(pc, symP->rightP);

    checkPCBound("Constants", pc - 1, lineno);
    return( pc );
}

// Add a new entry to the passed symlist, return new head.
SymListP
addToSymlist(SymListP listP, SymNodeP symP, int bank, int pc)
{
SymListP newP;

    newP = (SymListP)malloc(sizeof(SymList));
    newP->nextP = listP;
    newP->symP = symP;
    newP->bank = bank;
    newP->pc = pc;
    return( newP );
}

// Walk a symbol table of constants, set the value for each
void
setConstVal(SymNodeP symP)
{
    if( !symP )
    {
        return;
    }

    if( !(symP->flags & SYMF_EVALED) )
    {
        symP->value2 = evalExpr((PNodeP)(symP->ptr));
        symP->flags |= SYMF_EVALED;
    }

    setConstVal(symP->leftP);
    setConstVal(symP->rightP);
}

// Walk a list of var decls, set the pc for each VAR type found
void
setVarsPC(int bank, PNodeListP listP)
{
PNodeP nodeP;
SymNodeP symP;

    while( listP )
    {
        nodeP = listP->nodeP;
        symP = nodeP->value.symP;

        if( (symP->flags & SYMF_VAR) && !(symP->flags & SYMF_RESOLVED) )
        {
            symP->lineno = lineno - 1;
            symP->flags |= SYMF_RESOLVED;
            symP->value = curBankP->cur_pc++;
            symP->bank = bank;
            nodeP->pc = symP->value;
            nodeP->value2.ival = bank;
        }

        listP = listP->nextP;
    }

    checkPCBound("Variables", curBankP->cur_pc - 1, lineno);
}

// Process a symbol file, bring in all exported ones.
// If the filename starts with <, it will be terminated with > and means system include.
// Exported symbols found are created as globals in the bank they were defined in, a bank context
// will be created if needed.
void
importSymbols(char *filenameP)
{
int bank = curBank;
int origBank;
int lastBank;
int lineno;
int address;
char typec;
char *cP;
FILE *infP;
SymNodeP symP;
BankContextP bankP;
char str[256];
char symbol[256];

    if( *filenameP == '<' )
    {
        sprintf(str, "%s/%s", incroot, filenameP + 1);
        *strchr(str, '>') = 0;
        cP = str;
    }
    else
    {
        cP = filenameP;
    }

    if( !(infP = fopen(filenameP, "r")) )
    {
        verror("can't open import file '%s'", filenameP);
    }

    origBank = curBank;                     // will need this later
    lastBank = curBank;

    // discard first line, the file name
    if( !fgets(str, sizeof(str), infP) )
    {
        verror("file '%s' is empty", filenameP);
    }

    if( strcmp(str, "%%am1 symtab file%%\n") )
    {
        verror("file '%s' is not a symbol file", filenameP);
    }

    if( !fgets(str, sizeof(str), infP) )          // second line, the version number
    {
        verror("file '%s' is not a symbol file", filenameP);
    }

    if( strncmp(str, SYMFILEVERSION, strlen(SYMFILEVERSION)) )
    {
        verror("file '%s' is not a version %s symbol file", filenameP, SYMFILEVERSION);
    }

    if( !fgets(str, sizeof(str), infP) )              // third line, file name, just skip it
    {
        verror("error reading file '%s'", filenameP, SYMFILEVERSION);
    }

    // Ok, now the symbols
    while( fgets(str, sizeof(str), infP) )
    {
        // Remember, symbol addresses in the sym file are always octal.
        if( sscanf(str, "%o %c %s %d\n", &address, &typec, symbol, &lineno) != 4 )
        {
            verror("symbol file '%s' has an incorrect line format", filenameP);
        }

        bank = (address >> 12) & MAXBANK;
        address &= 07777;

        if( typec != 'X' )
        {
            continue;                       // not an exported symbol
        }

        if( bank != lastBank )
        {
            bankP = swapBanks(bank);
            lastBank = bank;
        }

        if( sym_find(&(curBankP->globalSymP), symbol) )
        {
            fclose(infP);
            verror("imported symbol '%s' has already been defined", cP);
        }

        symP = sym_make(symbol, 0);
        symP->lineno = lineno;
        symP->flags |= SYMF_IMPORTED | SYMF_RESOLVED | SYM_GLOB;
        symP->value = address;
        symP->bank = bank;
        sym_add(&(curBankP->globalSymP), symP);
    }

    if( bank != origBank )
    {
        swapBanks(origBank);             // back to where we were
    }

    fclose(infP);
}

// Resolve cross-bank wildcards, turn into BREFs.
// Not the most efficient, O(wildcards*banks), but there won't be that many of them.
void
resolveWildcards(PNodeListP listP, BankContextP banksP)
{
    while( listP )
    {
        if( !resolveWildcard(listP, banksP) )
        {
            // another hack to get offending line, we're at the end of the program
            lineno = listP->nodeP->lineNo + 1;
            verror("wildcarded symbol '%s' cannot be found", listP->nodeP->value.strP);
        }

        listP = listP->nextP;
    }
}

// We search the banks backwards because the banks are listed in reverse order of firs use.
// Returns true if found, else false.
bool
resolveWildcard(PNodeListP itemP, BankContextP bankP)
{
SymNodeP symP;

    if( bankP->nextP )
    {
        if( resolveWildcard(itemP, bankP->nextP) )
        {
            return(true);
        }
    }

    if( (symP = sym_find(&(bankP->globalSymP), itemP->nodeP->value.strP)) )
    {
        if( !(symP->flags | SYMF_RESOLVED) )
        {
            verror("wildcarded symbol '%s' in bank %d was never resolved", symP->name, bankP->bank);
        }

        // A bit of a hack, we turn it into a BREF
        itemP->nodeP->type = BREF;
        itemP->nodeP->value.symP = symP;
        itemP->nodeP->value2.ival = symP->bank;
        return( true );
    }

    return( false );
}

// Add a local symbol, setting the scope level
SymNodeP
addLocalSymbol(char *nameP)
{
SymNodeP symP;

    if( !localContextP )
    {
        return( NILP );
    }

    symP = sym_make(nameP, 0);
    symP->value2 = localDepth;
    sym_add(&(localContextP->symRootP), symP);
    return( symP );
}

// Pre-allocate bank 0 context so curBankP is valid before any grammar action fires.
void
initParser(void)
{
    curBankP = addBank(0);
    curBankP->cur_pc = 4;
    sym_init(&(curBankP->globalSymP));
    sym_init(&(curBankP->constSymP));
    curBankP->varNodesP = NILP;
}

// Switch to newBank: curBankP is already current; just redirect to the new context.
BankContextP
swapBanks(int newBank)
{
BankContextP newP;

    if( !(newP = findBank(newBank)) )
    {
        newP = addBank(newBank);
        sym_init(&(newP->globalSymP));
        sym_init(&(newP->constSymP));
        newP->varNodesP = NILP;
        newP->cur_pc = 0;
    }

    curBankP = newP;
    sawBank = true;
    curBank = newBank;
    return( curBankP );
}

BankContextP
addBank(int bank)
{
BankContextP newP;

    newP = (BankContextP)calloc(1, sizeof(BankContext));
    newP->bank = bank;
    newP->cur_pc = 0;       // new banks start at 0
    newP->constPC = -1;     // nothing placed automatically yet
    newP->varPC = -1;
    newP->nextP = banksP;
    banksP = newP;
    return( newP );
}

// Return the bank context if one exists for the given bank, else NILP
BankContextP
findBank(int bank)
{
BankContextP ctxP;

    for( ctxP = banksP; ctxP; ctxP = ctxP->nextP )
    {
        if( ctxP->bank == bank )
        {
            break;
        }
    }

    return( ctxP );
}

// Look up a symbol in a bank, create it if not found
SymNodeP
findSymbolInBank(int bank, char *nameP)
{
BankContextP bankP;
SymNodeP symP;

    // See if it's already defined
    if( !(bankP = findBank(bank)) )
    {
        // First time we've seen this bank, add an entry.
        bankP = addBank(bank);
        sawBank = true;
    }

    if( !(symP = sym_find(&(bankP->globalSymP), nameP)) )
    {
        // Nothing in that bank, go ahead and create it.
        // If it's never resolved there, an error will be reported later.
        symP = sym_make(nameP, 0);
        symP->flags = SYM_GLOB;
        symP->bank = bank;
        sym_add(&(bankP->globalSymP), symP);
        vwarn(WARN_BANKS, "creating symbol %s in bank %d, be sure to resolve it", nameP, bank);
    }

    return( symP );
}


int
yywrap()                /* tell lex to clean up */
{
    return(1);
}

void
vwarn(int errType, const char *msgP, ...)
{
va_list argP;
char format[1024];

    if( !doWarn(errType) )
    {
        return;
    }

    va_start(argP, msgP);
    sprintf(format,"am1: WARNING: %s\nat line %d, file %s\n",
        msgP,lineno,filenameP);
    vfprintf(stderr,format,argP);
    va_end(argP);
}

// Same as above except the line number is passed explicitly
void
vwarnl(int errType, int lineno, const char *msgP, ...)
{
va_list argP;
char format[1024];

    if( !doWarn(errType) )
    {
        return;
    }

    va_start(argP, msgP);
    sprintf(format,"am1: WARNING: %s\nat line %d, file %s\n",
        msgP,lineno,filenameP);
    vfprintf(stderr,format,argP);
    va_end(argP);
}

// Same as verror except the line number is passed explicitly.
// A diagnostic raised from a parser action cannot rely on the global
// lineno: if bison read the statement terminator as lookahead before the
// rule reduced, the lexer has already advanced it to the next line.
// The node's own lineNo, set by newnode(), is the line the user wrote.
void
verrorl(int lineno, const char *msgP, ...)
{
va_list argP;
char format[1024];

    va_start(argP, msgP);
    sprintf(format,"am1: %s\nat line %d, file %s\n",
        msgP,lineno,filenameP);
    vfprintf(stderr,format,argP);
    va_end(argP);
    leave(0);
}

void
verror(const char *msgP, ...)
{
va_list argP;
char format[1024];

    va_start(argP, msgP);
    sprintf(format,"am1: %s\nat line %d, file %s\n",
        msgP,lineno,filenameP);
    vfprintf(stderr,format,argP);
    va_end(argP);
    leave(0);
}

int
yyerror(const char *errstr)
{
    fprintf(stderr,"am1: %s\nat line %d, file %s\n",
    errstr,lineno,filenameP);
    leave(0);
    // never returns, just to shut up overly-picky c compilers
    return(0);
}
