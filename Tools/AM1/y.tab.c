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

int cur_pc = 4;                         // the default if not set

// We maintain a stack of local symtab ptrs for nested local scopes
int localDepth = 0;
int maxLocalDepth = 0;                  // the deepest nexting we've seen
LocalContextP localContextP;	        // used while a local scope is enabled
LocalContextP localStack[MAXLOCALS];
bool sawForceLocal;
bool didBrefWarn;
bool atSol = true;                      // initially true

// Bank contexts are kept as a linked list, not used often enough to need preallocation
int curBank;
BankContextP banksP;

static char scratchStr[128];            // and string

static PNodeListP varNodesP;            // unemitted vars
extern PNodeListP wildcardsP;           // any wildcarded cross-bank refs
extern SymNodeP globalSymP;		// global symtab
extern SymNodeP constSymP;		// literal constants
extern SymNodeP permSymP;	        // the instructions and other permanent values
extern SymListP constsListP;            // the list of all constant groups

extern bool noWarn;
extern bool sawBank;
extern bool doSymtab;
extern int lineno;
extern char *filenameP;
extern char *incroot;
extern PNodeP rootP;

SymNodeP addLocalSymbol(char *nameP);
LocalContextP newLocalContext(void);
BankContextP swapBanks(int newBank);
int countAscii(char *strP);
int countText(FlexText text);
int setConstPC(int pc, SymNodeP constSymP);
void setConstVal(SymNodeP constSymP);
void setVarsPC(int bank, PNodeListP varNodesP);
void addExports(PNodeP nodesP);
void importSymbols(char *filenameP);
void resolveWildcards(PNodeListP wildsP, BankContextP banksP);
bool resolveWildcard(PNodeListP itemP, BankContextP bankP);
BankContextP findBank(int bank);
BankContextP addBank(int bank);
SymListP addToSymlist(SymListP listP, SymNodeP symP, int bank, int pc);

extern SymNodeP resolveLocalSymbol(char *);

int yyerror(const char *errstr);
void verror(const char *msgP, ...);
void vwarn(const char *msgP, ...);

int yylex(void);

extern PNodeP binop(int lineNo, int pc, int value, PNodeP leftP, PNodeP rightP);
extern PNodeP unop(int lineNo, int pc, int value, PNodeP expP);
extern PNodeP newnode(int lineNo, int pc, int val, PNodeP leftP, PNodeP rightP);
extern int evalExpr(PNodeP);
extern long int hashExpr(PNodeP);
extern void leave(int);


#line 150 "y.tab.c"

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
    EXPORT = 266,                  /* EXPORT  */
    IMPORT = 267,                  /* IMPORT  */
    OPCODE = 268,                  /* OPCODE  */
    OPADDR = 269,                  /* OPADDR  */
    OPORABLE = 270,                /* OPORABLE  */
    VALUESPEC = 271,               /* VALUESPEC  */
    LOCAL = 272,                   /* LOCAL  */
    ADDR = 273,                    /* ADDR  */
    LCLADDR = 274,                 /* LCLADDR  */
    NAME = 275,                    /* NAME  */
    LCLNAME = 276,                 /* LCLNAME  */
    COMMENT = 277,                 /* COMMENT  */
    ENDLOC = 278,                  /* ENDLOC  */
    HEADER = 279,                  /* HEADER  */
    STRING = 280,                  /* STRING  */
    TEXT = 281,                    /* TEXT  */
    FILENAME = 282,                /* FILENAME  */
    LIBFILE = 283,                 /* LIBFILE  */
    CHAR = 284,                    /* CHAR  */
    FLEXO = 285,                   /* FLEXO  */
    INTEGER = 286,                 /* INTEGER  */
    LITCHAR = 287,                 /* LITCHAR  */
    BAD = 288,                     /* BAD  */
    ORIGIN = 289,                  /* ORIGIN  */
    EXPR = 290,                    /* EXPR  */
    BANK = 291,                    /* BANK  */
    LOCATION = 292,                /* LOCATION  */
    LCLLOCATION = 293,             /* LCLLOCATION  */
    FORCELOC = 294,                /* FORCELOC  */
    DOT = 295,                     /* DOT  */
    SLASH = 296,                   /* SLASH  */
    AND = 297,                     /* AND  */
    OR = 298,                      /* OR  */
    XOR = 299,                     /* XOR  */
    CMPL = 300,                    /* CMPL  */
    MINUS = 301,                   /* MINUS  */
    PLUS = 302,                    /* PLUS  */
    DIV = 303,                     /* DIV  */
    MOD = 304,                     /* MOD  */
    UNOP = 305,                    /* UNOP  */
    BINOP = 306,                   /* BINOP  */
    PARENS = 307,                  /* PARENS  */
    BREF = 308,                    /* BREF  */
    WILDREF = 309,                 /* WILDREF  */
    ENDCONST = 310,                /* ENDCONST  */
    SEPARATOR = 311,               /* SEPARATOR  */
    TERMINATOR = 312,              /* TERMINATOR  */
    SEMI = 313,                    /* SEMI  */
    CONSTANTS = 314,               /* CONSTANTS  */
    RELOC = 315,                   /* RELOC  */
    ENDRELOC = 316,                /* ENDRELOC  */
    LSHIFT = 317,                  /* LSHIFT  */
    RSHIFT = 318,                  /* RSHIFT  */
    MUL = 319,                     /* MUL  */
    UMINUS = 320                   /* UMINUS  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 82 "parser.y"

    long int ival;
    char *strP;
    SymNodeP symP;
    PNodeP pnodeP;
    FlexText flexText;
    

#line 274 "y.tab.c"

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
  YYSYMBOL_EXPORT = 11,                    /* EXPORT  */
  YYSYMBOL_IMPORT = 12,                    /* IMPORT  */
  YYSYMBOL_OPCODE = 13,                    /* OPCODE  */
  YYSYMBOL_OPADDR = 14,                    /* OPADDR  */
  YYSYMBOL_OPORABLE = 15,                  /* OPORABLE  */
  YYSYMBOL_VALUESPEC = 16,                 /* VALUESPEC  */
  YYSYMBOL_LOCAL = 17,                     /* LOCAL  */
  YYSYMBOL_ADDR = 18,                      /* ADDR  */
  YYSYMBOL_LCLADDR = 19,                   /* LCLADDR  */
  YYSYMBOL_NAME = 20,                      /* NAME  */
  YYSYMBOL_LCLNAME = 21,                   /* LCLNAME  */
  YYSYMBOL_COMMENT = 22,                   /* COMMENT  */
  YYSYMBOL_ENDLOC = 23,                    /* ENDLOC  */
  YYSYMBOL_HEADER = 24,                    /* HEADER  */
  YYSYMBOL_STRING = 25,                    /* STRING  */
  YYSYMBOL_TEXT = 26,                      /* TEXT  */
  YYSYMBOL_FILENAME = 27,                  /* FILENAME  */
  YYSYMBOL_LIBFILE = 28,                   /* LIBFILE  */
  YYSYMBOL_CHAR = 29,                      /* CHAR  */
  YYSYMBOL_FLEXO = 30,                     /* FLEXO  */
  YYSYMBOL_INTEGER = 31,                   /* INTEGER  */
  YYSYMBOL_LITCHAR = 32,                   /* LITCHAR  */
  YYSYMBOL_BAD = 33,                       /* BAD  */
  YYSYMBOL_ORIGIN = 34,                    /* ORIGIN  */
  YYSYMBOL_EXPR = 35,                      /* EXPR  */
  YYSYMBOL_BANK = 36,                      /* BANK  */
  YYSYMBOL_LOCATION = 37,                  /* LOCATION  */
  YYSYMBOL_LCLLOCATION = 38,               /* LCLLOCATION  */
  YYSYMBOL_FORCELOC = 39,                  /* FORCELOC  */
  YYSYMBOL_DOT = 40,                       /* DOT  */
  YYSYMBOL_SLASH = 41,                     /* SLASH  */
  YYSYMBOL_AND = 42,                       /* AND  */
  YYSYMBOL_OR = 43,                        /* OR  */
  YYSYMBOL_XOR = 44,                       /* XOR  */
  YYSYMBOL_CMPL = 45,                      /* CMPL  */
  YYSYMBOL_MINUS = 46,                     /* MINUS  */
  YYSYMBOL_PLUS = 47,                      /* PLUS  */
  YYSYMBOL_DIV = 48,                       /* DIV  */
  YYSYMBOL_MOD = 49,                       /* MOD  */
  YYSYMBOL_UNOP = 50,                      /* UNOP  */
  YYSYMBOL_BINOP = 51,                     /* BINOP  */
  YYSYMBOL_PARENS = 52,                    /* PARENS  */
  YYSYMBOL_BREF = 53,                      /* BREF  */
  YYSYMBOL_WILDREF = 54,                   /* WILDREF  */
  YYSYMBOL_ENDCONST = 55,                  /* ENDCONST  */
  YYSYMBOL_SEPARATOR = 56,                 /* SEPARATOR  */
  YYSYMBOL_TERMINATOR = 57,                /* TERMINATOR  */
  YYSYMBOL_SEMI = 58,                      /* SEMI  */
  YYSYMBOL_CONSTANTS = 59,                 /* CONSTANTS  */
  YYSYMBOL_RELOC = 60,                     /* RELOC  */
  YYSYMBOL_ENDRELOC = 61,                  /* ENDRELOC  */
  YYSYMBOL_LSHIFT = 62,                    /* LSHIFT  */
  YYSYMBOL_RSHIFT = 63,                    /* RSHIFT  */
  YYSYMBOL_MUL = 64,                       /* MUL  */
  YYSYMBOL_UMINUS = 65,                    /* UMINUS  */
  YYSYMBOL_66_ = 66,                       /* '='  */
  YYSYMBOL_67_ = 67,                       /* '('  */
  YYSYMBOL_68_ = 68,                       /* ')'  */
  YYSYMBOL_YYACCEPT = 69,                  /* $accept  */
  YYSYMBOL_program = 70,                   /* program  */
  YYSYMBOL_optfilenames = 71,              /* optfilenames  */
  YYSYMBOL_filenames = 72,                 /* filenames  */
  YYSYMBOL_start = 73,                     /* start  */
  YYSYMBOL_body = 74,                      /* body  */
  YYSYMBOL_stmt_list = 75,                 /* stmt_list  */
  YYSYMBOL_stmt = 76,                      /* stmt  */
  YYSYMBOL_one_stmt = 77,                  /* one_stmt  */
  YYSYMBOL_terminator = 78,                /* terminator  */
  YYSYMBOL_terminators = 79,               /* terminators  */
  YYSYMBOL_optExpr = 80,                   /* optExpr  */
  YYSYMBOL_optINTEGER = 81,                /* optINTEGER  */
  YYSYMBOL_expr = 82,                      /* expr  */
  YYSYMBOL_simple_expr = 83,               /* simple_expr  */
  YYSYMBOL_directive_expr = 84,            /* directive_expr  */
  YYSYMBOL_endConst = 85,                  /* endConst  */
  YYSYMBOL_optLocals = 86,                 /* optLocals  */
  YYSYMBOL_symList = 87,                   /* symList  */
  YYSYMBOL_symbol = 88,                    /* symbol  */
  YYSYMBOL_varnames = 89,                  /* varnames  */
  YYSYMBOL_bref = 90,                      /* bref  */
  YYSYMBOL_wildref = 91,                   /* wildref  */
  YYSYMBOL_var = 92,                       /* var  */
  YYSYMBOL_varname = 93                    /* varname  */
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
#define YYLAST   335

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  69
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  25
/* YYNRULES -- Number of rules.  */
#define YYNRULES  97
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  145

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   320


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
      67,    68,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,    66,     2,     2,     2,     2,     2,     2,     2,     2,
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
      65
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,   196,   196,   202,   203,   206,   207,   210,   215,   220,
     279,   293,   298,   307,   336,   342,   350,   367,   371,   385,
     390,   418,   441,   455,   475,   496,   502,   508,   514,   520,
     533,   539,   547,   552,   556,   566,   574,   578,   584,   589,
     595,   600,   605,   606,   609,   610,   611,   612,   613,   614,
     615,   616,   617,   618,   619,   620,   621,   622,   623,   628,
     633,   638,   643,   648,   667,   672,   677,   682,   714,   721,
     742,   749,   778,   793,   798,   803,   808,   815,   827,   874,
     900,   904,   911,   916,   923,   928,   933,   941,   946,   953,
     965,   981,   990,   996,   999,  1003,  1009,  1020
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
  "CONSTANT", "NAMES", "VAR", "VARS", "TABLE", "ASCII", "EXPORT", "IMPORT",
  "OPCODE", "OPADDR", "OPORABLE", "VALUESPEC", "LOCAL", "ADDR", "LCLADDR",
  "NAME", "LCLNAME", "COMMENT", "ENDLOC", "HEADER", "STRING", "TEXT",
  "FILENAME", "LIBFILE", "CHAR", "FLEXO", "INTEGER", "LITCHAR", "BAD",
  "ORIGIN", "EXPR", "BANK", "LOCATION", "LCLLOCATION", "FORCELOC", "DOT",
  "SLASH", "AND", "OR", "XOR", "CMPL", "MINUS", "PLUS", "DIV", "MOD",
  "UNOP", "BINOP", "PARENS", "BREF", "WILDREF", "ENDCONST", "SEPARATOR",
  "TERMINATOR", "SEMI", "CONSTANTS", "RELOC", "ENDRELOC", "LSHIFT",
  "RSHIFT", "MUL", "UMINUS", "'='", "'('", "')'", "$accept", "program",
  "optfilenames", "filenames", "start", "body", "stmt_list", "stmt",
  "one_stmt", "terminator", "terminators", "optExpr", "optINTEGER", "expr",
  "simple_expr", "directive_expr", "endConst", "optLocals", "symList",
  "symbol", "varnames", "bref", "wildref", "var", "varname", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-57)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-1)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
       5,   -57,    36,    15,    24,   -57,     8,   -57,   100,   150,
       6,   -57,   150,    41,    43,     9,   -57,   -57,   -57,   -57,
      43,   -28,    30,   -26,    35,   -57,    42,   -57,   -57,   -57,
     -57,    21,   -57,    44,   -57,   -57,   150,   150,   -57,   -57,
     -57,   150,    14,   100,   -17,   -57,   -57,    19,   -57,   179,
     -57,    28,   -57,    28,   -57,   156,   -57,   -57,    40,   -57,
      16,   202,   -57,   -57,   -57,    46,   -57,   -57,   -57,    49,
     -57,    39,   -19,   -57,   -57,    39,    39,   -57,   -57,    39,
     -57,   -57,    -9,   -57,   -57,   -57,   -57,   213,   150,    45,
     -57,   -17,   -57,   -57,   -57,   -57,   150,   150,   150,   150,
     150,   150,   150,   150,   150,   150,   150,   -57,   -57,   -57,
       6,   150,   150,    43,    43,   -57,   -57,   145,   -57,   -57,
     -57,   -57,   -57,   -57,   -57,   236,   -57,   -57,   271,   267,
     225,   -34,   -34,   -57,   -57,   259,     0,     0,   -57,   -57,
     145,   145,   -57,   -57,   -57
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       4,     5,     0,     0,     3,     1,     0,     6,     0,     0,
       0,    18,     0,     0,     0,     0,    59,    60,    61,    62,
      84,    65,    73,    71,    72,    34,    41,    26,    35,    75,
      74,    58,    76,     0,    77,    64,     0,     0,    36,    33,
      24,     0,     0,     9,     0,    14,    11,    32,    15,    42,
      43,    65,    73,    71,    72,     0,    97,    96,    17,    89,
      94,    27,    25,    88,    87,    29,    85,    30,    31,    78,
      82,    39,     0,    68,    70,    39,    39,    67,    69,    39,
      40,    79,     0,    66,    16,    57,    45,     0,     0,     0,
       2,     0,    12,    10,    37,    19,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    81,    80,    63,
       0,     0,     0,     0,     0,    21,    38,    42,    91,    92,
      93,    23,    20,    22,    56,     0,     8,    13,    51,    52,
      53,    47,    46,    49,    50,    44,    54,    55,    48,    90,
      95,    28,    86,    83,     7
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int8 yypgoto[] =
{
     -57,   -57,   -57,   -57,   -57,   -57,   -57,    58,   -57,   -41,
     -57,   -56,   -57,    -1,    -8,   -57,   -57,   -57,   -57,   -14,
     -57,   -15,   -10,   -23,   -57
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int8 yydefgoto[] =
{
       0,     2,     3,     4,    90,    42,    43,    44,    45,    46,
      47,   115,    81,   116,   117,    50,   109,    69,    65,    66,
      58,    73,    74,    59,    60
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_uint8 yytable[] =
{
      49,    55,    92,    93,    61,    25,    70,    48,    77,    71,
      28,    76,   118,    78,   101,   102,    83,    88,    89,   121,
     122,   119,   118,   123,    56,    72,    57,    72,    85,    86,
     106,   119,     1,    87,    67,    49,     5,    68,    77,     6,
      38,    39,    48,    78,     9,   120,    99,   100,   101,   102,
     127,     7,    16,    17,    18,    19,    20,    51,    52,    53,
      54,    63,    26,    64,   106,     8,    62,    75,    29,    30,
      31,    32,    79,    80,    82,    84,    94,   110,    34,    35,
     125,    72,   111,   113,    36,    37,   114,   139,   128,   129,
     130,   131,   132,   133,   134,   135,   136,   137,   138,   142,
     143,    91,   126,   140,   141,     9,    41,    10,    11,    12,
      13,    14,    15,    16,    17,    18,    19,    20,    21,    22,
      23,    24,    25,    26,     0,     0,    27,    28,     0,    29,
      30,    31,    32,     0,     0,     0,    33,     0,     0,    34,
      35,     0,     0,     0,     0,    36,    37,     0,     0,     0,
       0,     0,     0,     0,     0,     9,     0,    38,    39,    40,
       0,     0,     0,    16,    17,    18,    19,    41,    51,    52,
      53,    54,     0,     0,     0,     0,     0,     0,   107,    29,
      30,    31,    32,     0,     0,     0,     0,    96,    97,    98,
      35,    99,   100,   101,   102,    36,    37,     0,    96,    97,
      98,   103,    99,   100,   101,   102,     0,   104,   105,   106,
       0,   108,   103,    95,     0,     0,     0,    41,   104,   105,
     106,    96,    97,    98,     0,    99,   100,   101,   102,     0,
       0,     0,     0,     0,     0,   103,     0,     0,     0,   112,
       0,   104,   105,   106,    96,    97,    98,     0,    99,   100,
     101,   102,     0,     0,     0,    96,    97,    98,   103,    99,
     100,   101,   102,     0,   104,   105,   106,    96,     0,   103,
       0,    99,   100,   101,   102,   104,   105,   106,    96,    97,
      98,   124,    99,   100,   101,   102,     0,   104,   105,   106,
       0,     0,   103,   144,     0,     0,     0,     0,   104,   105,
     106,    96,    97,    98,     0,    99,   100,   101,   102,    96,
       0,    98,     0,    99,   100,   101,   102,    99,   100,   101,
     102,   104,   105,   106,     0,     0,     0,     0,     0,   104,
     105,   106,     0,   104,   105,   106
};

static const yytype_int8 yycheck[] =
{
       8,     9,    43,    44,    12,    22,    20,     8,    23,    37,
      27,    37,    31,    23,    48,    49,    31,     3,     4,    75,
      76,    40,    31,    79,    18,    53,    20,    53,    36,    37,
      64,    40,    27,    41,    25,    43,     0,    28,    53,    24,
      57,    58,    43,    53,     5,    64,    46,    47,    48,    49,
      91,    27,    13,    14,    15,    16,    17,    18,    19,    20,
      21,    18,    23,    20,    64,    57,    25,    37,    29,    30,
      31,    32,    37,    31,    53,    31,    57,    37,    39,    40,
      88,    53,    66,    37,    45,    46,    37,   110,    96,    97,
      98,    99,   100,   101,   102,   103,   104,   105,   106,   113,
     114,    43,    57,   111,   112,     5,    67,     7,     8,     9,
      10,    11,    12,    13,    14,    15,    16,    17,    18,    19,
      20,    21,    22,    23,    -1,    -1,    26,    27,    -1,    29,
      30,    31,    32,    -1,    -1,    -1,    36,    -1,    -1,    39,
      40,    -1,    -1,    -1,    -1,    45,    46,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,     5,    -1,    57,    58,    59,
      -1,    -1,    -1,    13,    14,    15,    16,    67,    18,    19,
      20,    21,    -1,    -1,    -1,    -1,    -1,    -1,    22,    29,
      30,    31,    32,    -1,    -1,    -1,    -1,    42,    43,    44,
      40,    46,    47,    48,    49,    45,    46,    -1,    42,    43,
      44,    56,    46,    47,    48,    49,    -1,    62,    63,    64,
      -1,    55,    56,    34,    -1,    -1,    -1,    67,    62,    63,
      64,    42,    43,    44,    -1,    46,    47,    48,    49,    -1,
      -1,    -1,    -1,    -1,    -1,    56,    -1,    -1,    -1,    37,
      -1,    62,    63,    64,    42,    43,    44,    -1,    46,    47,
      48,    49,    -1,    -1,    -1,    42,    43,    44,    56,    46,
      47,    48,    49,    -1,    62,    63,    64,    42,    -1,    56,
      -1,    46,    47,    48,    49,    62,    63,    64,    42,    43,
      44,    68,    46,    47,    48,    49,    -1,    62,    63,    64,
      -1,    -1,    56,    57,    -1,    -1,    -1,    -1,    62,    63,
      64,    42,    43,    44,    -1,    46,    47,    48,    49,    42,
      -1,    44,    -1,    46,    47,    48,    49,    46,    47,    48,
      49,    62,    63,    64,    -1,    -1,    -1,    -1,    -1,    62,
      63,    64,    -1,    62,    63,    64
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,    27,    70,    71,    72,     0,    24,    27,    57,     5,
       7,     8,     9,    10,    11,    12,    13,    14,    15,    16,
      17,    18,    19,    20,    21,    22,    23,    26,    27,    29,
      30,    31,    32,    36,    39,    40,    45,    46,    57,    58,
      59,    67,    74,    75,    76,    77,    78,    79,    82,    83,
      84,    18,    19,    20,    21,    83,    18,    20,    89,    92,
      93,    83,    25,    18,    20,    87,    88,    25,    28,    86,
      88,    37,    53,    90,    91,    37,    37,    90,    91,    37,
      31,    81,    53,    90,    31,    83,    83,    83,     3,     4,
      73,    76,    78,    78,    57,    34,    42,    43,    44,    46,
      47,    48,    49,    56,    62,    63,    64,    22,    55,    85,
      37,    66,    37,    37,    37,    80,    82,    83,    31,    40,
      64,    80,    80,    80,    68,    83,    57,    78,    83,    83,
      83,    83,    83,    83,    83,    83,    83,    83,    83,    92,
      83,    83,    88,    88,    57
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    69,    70,    71,    71,    72,    72,    73,    73,    74,
      75,    75,    75,    75,    76,    77,    77,    77,    77,    77,
      77,    77,    77,    77,    77,    77,    77,    77,    77,    77,
      77,    77,    78,    78,    78,    78,    79,    79,    80,    80,
      81,    81,    82,    82,    83,    83,    83,    83,    83,    83,
      83,    83,    83,    83,    83,    83,    83,    83,    83,    83,
      83,    83,    83,    83,    83,    83,    83,    83,    83,    83,
      83,    83,    83,    83,    83,    83,    83,    84,    84,    84,
      85,    85,    86,    86,    86,    87,    87,    88,    88,    89,
      89,    90,    90,    91,    92,    92,    93,    93
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     5,     1,     0,     1,     2,     3,     2,     1,
       2,     1,     2,     3,     1,     1,     2,     2,     1,     2,
       3,     3,     3,     3,     1,     2,     1,     2,     4,     2,
       2,     2,     1,     1,     1,     1,     1,     2,     1,     0,
       1,     0,     1,     1,     3,     2,     3,     3,     3,     3,
       3,     3,     3,     3,     3,     3,     3,     2,     1,     1,
       1,     1,     1,     3,     1,     1,     2,     2,     2,     2,
       2,     1,     1,     1,     1,     1,     1,     1,     2,     2,
       1,     1,     1,     3,     0,     1,     3,     1,     1,     1,
       3,     2,     2,     2,     1,     3,     1,     1
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
#line 197 "parser.y"
                {
                    rootP = newnode(lineno, cur_pc, HEADER, (yyvsp[-1].pnodeP), (yyvsp[0].pnodeP));
                    rootP->value.strP = (yyvsp[-3].strP);
		}
#line 1493 "y.tab.c"
    break;

  case 7: /* start: START simple_expr TERMINATOR  */
#line 211 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, START, NILP, NILP);
                    (yyval.pnodeP)->value.ival = evalExpr((yyvsp[-1].pnodeP));
                }
#line 1502 "y.tab.c"
    break;

  case 8: /* start: STOP TERMINATOR  */
#line 216 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, STOP, NILP, NILP);
                }
#line 1510 "y.tab.c"
    break;

  case 9: /* body: stmt_list  */
#line 221 "parser.y"
                {
                PNodeP nodeP;
                SymListP symlistP;
                BankContextP bankP;

		    if( (yyvsp[0].pnodeP) )
		    {
                        if( !sawBank )
                        {
                            // All in bank 0, but make a bank entry for it for consistency
                            bankP = addBank(0);
                            bankP->globalSymP = globalSymP;
                        }
                        else
                        {
                            bankP = findBank(curBank);
                        }

                        bankP->cur_pc = cur_pc;
                        bankP->globalSymP = globalSymP;
                        bankP->constSymP = constSymP;
                        bankP->varNodesP = varNodesP;

                        // now update all banks that need it for unemitted consts and vars
                        for(BankContextP bankP = banksP; bankP; bankP = bankP->nextP)
                        {
                            if( bankP->varNodesP )
                            {
                                setVarsPC(bankP->bank, bankP->varNodesP);
                            }

                            if( bankP->constSymP )
                            {
                                setConstPC(bankP->cur_pc, bankP->constSymP);
                                constsListP = addToSymlist(constsListP, bankP->constSymP,
                                    bankP->bank, bankP->cur_pc);
                            }
                        }

                        // Fix any wildcarded brefs
                        resolveWildcards(wildcardsP, banksP);

                        // Resolve all constant values
                        for( symlistP = constsListP; symlistP; symlistP = symlistP->nextP )
                        {
                            setConstVal(symlistP->symP);
                        }

			(yyval.pnodeP) = (yyvsp[0].pnodeP)->leftP;		/* recover head link */
			(yyvsp[0].pnodeP)->leftP = NILP;
		    }
		    else
                    {
			(yyval.pnodeP) = NILP;
                    }
		}
#line 1571 "y.tab.c"
    break;

  case 10: /* stmt_list: stmt terminator  */
#line 280 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);

                    if( (yyvsp[-1].pnodeP) )
                    {
                        (yyvsp[0].pnodeP)->leftP = (yyvsp[-1].pnodeP);		/* keep head */
                        (yyvsp[-1].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    }
                    else
                    {
                        (yyvsp[0].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    }
		}
#line 1589 "y.tab.c"
    break;

  case 11: /* stmt_list: terminator  */
#line 294 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    (yyvsp[0].pnodeP)->leftP = (yyvsp[0].pnodeP);
                }
#line 1598 "y.tab.c"
    break;

  case 12: /* stmt_list: stmt_list terminator  */
#line 299 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    if( (yyvsp[-1].pnodeP) )
                    {
                        (yyval.pnodeP)->leftP = (yyvsp[-1].pnodeP)->leftP;
                        (yyvsp[-1].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    }
                }
#line 1611 "y.tab.c"
    break;

  case 13: /* stmt_list: stmt_list stmt terminator  */
#line 308 "parser.y"
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
#line 1638 "y.tab.c"
    break;

  case 14: /* stmt: one_stmt  */
#line 337 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    atSol = false;
                }
#line 1647 "y.tab.c"
    break;

  case 15: /* one_stmt: expr  */
#line 343 "parser.y"
                {
		    (yyval.pnodeP) = newnode(lineno, cur_pc, EXPR, NILP, (yyvsp[0].pnodeP));
		    if( (yyvsp[0].pnodeP) && !((yyvsp[0].pnodeP)->flags & PN_NOINC) )
                    {
                        ++cur_pc;
                    }
		}
#line 1659 "y.tab.c"
    break;

  case 16: /* one_stmt: BANK INTEGER  */
#line 351 "parser.y"
                {
                    if( ((yyvsp[0].ival) < 0) || ((yyvsp[0].ival) > 31) )
                    {
                        verror("Bank number must be between 0-32 decimal, 037 octal, 1F hex");
                    }

                    if( localContextP )
                    {
                        verror("Bank cannot be used inside a local context");
                    }

                    (yyval.pnodeP) = newnode(lineno+1, cur_pc, BANK, NILP, NILP);
                    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
                    swapBanks((yyvsp[0].ival));
                    (yyval.pnodeP)->value2.ival = cur_pc;   // is the pc for the new bank
                }
#line 1680 "y.tab.c"
    break;

  case 17: /* one_stmt: VAR varnames  */
#line 368 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, VAR, NILP, (yyvsp[0].pnodeP));
                }
#line 1688 "y.tab.c"
    break;

  case 18: /* one_stmt: VARS  */
#line 372 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno+1, cur_pc, VARS, NILP, NILP);
                    (yyval.pnodeP)->value.ptr = varNodesP;
                    if( !varNodesP )
                    {
                        vwarn("no variables have been declareed, variables ignored");
                    }
                    else
                    {
                        setVarsPC(curBank, varNodesP);
                        varNodesP = 0;
                    }
                }
#line 1706 "y.tab.c"
    break;

  case 19: /* one_stmt: simple_expr ORIGIN  */
#line 386 "parser.y"
                {
		    (yyval.pnodeP) = newnode(lineno, cur_pc, ORIGIN, NILP, NILP);
                    (yyval.pnodeP)->value.ival = cur_pc = evalExpr((yyvsp[-1].pnodeP));
                }
#line 1715 "y.tab.c"
    break;

  case 20: /* one_stmt: NAME LOCATION optExpr  */
#line 391 "parser.y"
                {
                int locType;
                SymNodeP symP;

                    // Hack used by mactoam1 for symbols in defines.
                    // All new symbols are assumed local.
                    // We're defining this regular symbol in the local context.
                    if( localContextP && (localContextP->flags == CTX_FORCELOCAL) )
                    {
                        symP = addLocalSymbol((yyvsp[-2].strP));
                        symP->flags = SYMF_RESOLVED | SYMF_FORCED | SYM_LOC;
                        symP->value = cur_pc;
                        locType = LCLLOCATION;
                    }
                    else
                    {
                        symP = sym_make((yyvsp[-2].strP), 0);
                        symP->flags |= SYMF_RESOLVED | SYM_GLOB;
                        symP->value = cur_pc;
                        symP->bank = curBank;
                        sym_add(&globalSymP, symP);
                        locType = LOCATION;
                    }

                    (yyval.pnodeP) = newnode(lineno, ((yyvsp[0].pnodeP) && !((yyvsp[0].pnodeP)->flags & PN_NOINC))?cur_pc++:cur_pc, locType, NILP, (yyvsp[0].pnodeP));
                    (yyval.pnodeP)->value.symP = symP;
                }
#line 1747 "y.tab.c"
    break;

  case 21: /* one_stmt: ADDR LOCATION optExpr  */
#line 419 "parser.y"
                {
                    if( (yyvsp[-2].symP)->flags & SYMF_RESOLVED )
                    {
                        verror("Duplicate label %s", (yyvsp[-2].symP)->name);
                    }
                    else
                    {
                        if( ((yyvsp[-2].symP)->flags & SYM_MASK) == SYM_LOC )
                        {
                            // This was from a local context when forced was in effect,
                            // fix it up.
                            (yyvsp[-2].symP)->flags = SYM_GLOB;
                            (yyvsp[-2].symP)->symP->flags = SYM_GLOB | SYMF_RESOLVED;
                            (yyvsp[-2].symP)->symP->value = cur_pc;
                        }

                        (yyvsp[-2].symP)->flags |= SYMF_RESOLVED;
                        (yyvsp[-2].symP)->value = cur_pc;
                        (yyval.pnodeP) = newnode(lineno, ((yyvsp[0].pnodeP) && !((yyvsp[0].pnodeP)->flags & PN_NOINC))?cur_pc++:cur_pc, LOCATION, NILP, (yyvsp[0].pnodeP));
                        (yyval.pnodeP)->value.symP = (yyvsp[-2].symP);
                    }
                }
#line 1774 "y.tab.c"
    break;

  case 22: /* one_stmt: LCLNAME LOCATION optExpr  */
#line 442 "parser.y"
                {
                SymNodeP symP;

                    if( !(symP = addLocalSymbol((yyvsp[-2].strP))) )
                    {
                        verror("local symbol used, but not inside a local scope");
                    }

                    symP->flags = SYMF_RESOLVED | SYM_LOC;
                    symP->value = cur_pc;
                    (yyval.pnodeP) = newnode(lineno, ((yyvsp[0].pnodeP) && !((yyvsp[0].pnodeP)->flags & PN_NOINC))?cur_pc++:cur_pc, LCLLOCATION, NILP, (yyvsp[0].pnodeP));
                    (yyval.pnodeP)->value.symP = symP;
                }
#line 1792 "y.tab.c"
    break;

  case 23: /* one_stmt: LCLADDR LOCATION optExpr  */
#line 456 "parser.y"
                {
                    if( (yyvsp[-2].symP)->value2 < localDepth )
                    {
                        verror(
                            "local label %s is defined in outer scope %d, this is scope %d, can't be declared here",
                            (yyvsp[-2].symP)->name, (yyvsp[-2].symP)->value2, localDepth);
                    }
                    else if( (yyvsp[-2].symP)->flags & SYMF_RESOLVED )
                    {
                        verror("Duplicate local label %s", (yyvsp[-2].symP)->name);
                    }
                    else
                    {
                        (yyvsp[-2].symP)->flags |= SYMF_RESOLVED;
                        (yyvsp[-2].symP)->value = cur_pc;
                        (yyval.pnodeP) = newnode(lineno, ((yyvsp[0].pnodeP) && !((yyvsp[0].pnodeP)->flags & PN_NOINC))?cur_pc++:cur_pc, LCLLOCATION, NILP, (yyvsp[0].pnodeP));
                        (yyval.pnodeP)->value.symP = (yyvsp[-2].symP);
                    }
                }
#line 1816 "y.tab.c"
    break;

  case 24: /* one_stmt: CONSTANTS  */
#line 476 "parser.y"
                {
                SymListP symlistP;
                BankContextP ctxP;
                    // End this constant scope, if there is one, but include the node for listings
                    (yyval.pnodeP) = newnode(lineno+1, cur_pc, CONSTANTS, NILP, NILP);

                    if( constSymP )
                    {
                        constsListP = addToSymlist(constsListP, constSymP, curBank, cur_pc);
                        (yyval.pnodeP)->value.symP = constSymP;
                        cur_pc = setConstPC(cur_pc, constSymP);
                        sym_init(&constSymP);

                        // Be sure we clear from our bank context, if we have one
                        if( (ctxP = findBank(curBank)) )
                        {
                            ctxP->constSymP = NILP;
                        }
                    }
                }
#line 1841 "y.tab.c"
    break;

  case 25: /* one_stmt: ASCII STRING  */
#line 497 "parser.y"
                {
		    (yyval.pnodeP) = newnode(lineno+1, cur_pc, ASCII, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                    cur_pc += countAscii((yyvsp[0].strP));
                }
#line 1851 "y.tab.c"
    break;

  case 26: /* one_stmt: TEXT  */
#line 503 "parser.y"
                {
		    (yyval.pnodeP) = newnode(lineno+1, cur_pc, TEXT, NILP, NILP);
                    (yyval.pnodeP)->value.flexText = (yyvsp[0].flexText);
                    cur_pc += countText((yyvsp[0].flexText));
                }
#line 1861 "y.tab.c"
    break;

  case 27: /* one_stmt: TABLE simple_expr  */
#line 509 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, TABLE, NILP, NILP);
                    (yyval.pnodeP)->value.ival = evalExpr((yyvsp[0].pnodeP));
                    cur_pc += (yyval.pnodeP)->value.ival;
                }
#line 1871 "y.tab.c"
    break;

  case 28: /* one_stmt: TABLE simple_expr LOCATION simple_expr  */
#line 515 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, TABLE, NILP, (yyvsp[0].pnodeP));
                    (yyval.pnodeP)->value.ival = evalExpr((yyvsp[-2].pnodeP));
                    cur_pc += (yyval.pnodeP)->value.ival;
                }
#line 1881 "y.tab.c"
    break;

  case 29: /* one_stmt: EXPORT symList  */
#line 521 "parser.y"
                {
                SymNodeP symP;
                PNodeP nodeP;

                    nodeP = (yyvsp[0].pnodeP)->leftP;      // recover head link
                    (yyvsp[0].pnodeP)->leftP = NILP;

                    addExports(nodeP);
		    (yyval.pnodeP) = newnode(lineno, cur_pc, EXPORT, NILP, nodeP);
                    (yyval.pnodeP)->flags |= PN_NOINC;
                    doSymtab = true;        // and force output
                }
#line 1898 "y.tab.c"
    break;

  case 30: /* one_stmt: IMPORT STRING  */
#line 534 "parser.y"
                {
		    (yyval.pnodeP) = newnode(lineno, cur_pc, IMPORT, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                    importSymbols((yyvsp[0].strP));
                }
#line 1908 "y.tab.c"
    break;

  case 31: /* one_stmt: IMPORT LIBFILE  */
#line 540 "parser.y"
                {
		    (yyval.pnodeP) = newnode(lineno, cur_pc, IMPORT, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                    importSymbols((yyvsp[0].strP));
                }
#line 1918 "y.tab.c"
    break;

  case 32: /* terminator: terminators  */
#line 548 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    atSol = true;
                }
#line 1927 "y.tab.c"
    break;

  case 33: /* terminator: SEMI  */
#line 553 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, SEMI, NILP, NILP);
                }
#line 1935 "y.tab.c"
    break;

  case 34: /* terminator: COMMENT  */
#line 557 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, COMMENT, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                    if( atSol )
                    {
                        (yyval.pnodeP)->flags |= PN_SOL;
                    }
                    atSol = true;
                }
#line 1949 "y.tab.c"
    break;

  case 35: /* terminator: FILENAME  */
#line 567 "parser.y"
                {
		    (yyval.pnodeP) = newnode(lineno, cur_pc, FILENAME, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                    atSol = true;
                }
#line 1959 "y.tab.c"
    break;

  case 36: /* terminators: TERMINATOR  */
#line 575 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, TERMINATOR, NILP, NILP);
                }
#line 1967 "y.tab.c"
    break;

  case 37: /* terminators: terminators TERMINATOR  */
#line 579 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[-1].pnodeP);
                }
#line 1975 "y.tab.c"
    break;

  case 38: /* optExpr: expr  */
#line 585 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                }
#line 1983 "y.tab.c"
    break;

  case 39: /* optExpr: %empty  */
#line 589 "parser.y"
                {
                    // empty
		    (yyval.pnodeP) = NILP;
                }
#line 1992 "y.tab.c"
    break;

  case 40: /* optINTEGER: INTEGER  */
#line 596 "parser.y"
                {
                    (yyval.ival) = (yyvsp[0].ival);
                }
#line 2000 "y.tab.c"
    break;

  case 41: /* optINTEGER: %empty  */
#line 600 "parser.y"
                {
                    (yyval.ival) = -1;
                }
#line 2008 "y.tab.c"
    break;

  case 42: /* expr: simple_expr  */
#line 605 "parser.y"
                                            { (yyval.pnodeP) = (yyvsp[0].pnodeP); }
#line 2014 "y.tab.c"
    break;

  case 43: /* expr: directive_expr  */
#line 606 "parser.y"
                                            { (yyval.pnodeP) = (yyvsp[0].pnodeP); }
#line 2020 "y.tab.c"
    break;

  case 44: /* simple_expr: simple_expr SEPARATOR simple_expr  */
#line 609 "parser.y"
                                                          { (yyval.pnodeP) = binop(lineno, cur_pc, SEPARATOR, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2026 "y.tab.c"
    break;

  case 45: /* simple_expr: MINUS simple_expr  */
#line 610 "parser.y"
                                                   { (yyval.pnodeP) = unop(lineno, cur_pc, UMINUS, (yyvsp[0].pnodeP)); }
#line 2032 "y.tab.c"
    break;

  case 46: /* simple_expr: simple_expr PLUS simple_expr  */
#line 611 "parser.y"
                                                          { (yyval.pnodeP) = binop(lineno, cur_pc, PLUS, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2038 "y.tab.c"
    break;

  case 47: /* simple_expr: simple_expr MINUS simple_expr  */
#line 612 "parser.y"
                                                          { (yyval.pnodeP) = binop(lineno, cur_pc, MINUS, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2044 "y.tab.c"
    break;

  case 48: /* simple_expr: simple_expr MUL simple_expr  */
#line 613 "parser.y"
                                                          { (yyval.pnodeP) = binop(lineno, cur_pc, MUL, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2050 "y.tab.c"
    break;

  case 49: /* simple_expr: simple_expr DIV simple_expr  */
#line 614 "parser.y"
                                                          { (yyval.pnodeP) = binop(lineno, cur_pc, DIV, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2056 "y.tab.c"
    break;

  case 50: /* simple_expr: simple_expr MOD simple_expr  */
#line 615 "parser.y"
                                                          { (yyval.pnodeP) = binop(lineno, cur_pc, MOD, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2062 "y.tab.c"
    break;

  case 51: /* simple_expr: simple_expr AND simple_expr  */
#line 616 "parser.y"
                                                          { (yyval.pnodeP) = binop(lineno, cur_pc, AND, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2068 "y.tab.c"
    break;

  case 52: /* simple_expr: simple_expr OR simple_expr  */
#line 617 "parser.y"
                                                          { (yyval.pnodeP) = binop(lineno, cur_pc, OR, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2074 "y.tab.c"
    break;

  case 53: /* simple_expr: simple_expr XOR simple_expr  */
#line 618 "parser.y"
                                                          { (yyval.pnodeP) = binop(lineno, cur_pc, XOR, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2080 "y.tab.c"
    break;

  case 54: /* simple_expr: simple_expr LSHIFT simple_expr  */
#line 619 "parser.y"
                                                          { (yyval.pnodeP) = binop(lineno, cur_pc, LSHIFT, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2086 "y.tab.c"
    break;

  case 55: /* simple_expr: simple_expr RSHIFT simple_expr  */
#line 620 "parser.y"
                                                          { (yyval.pnodeP) = binop(lineno, cur_pc, RSHIFT, (yyvsp[-2].pnodeP), (yyvsp[0].pnodeP)); }
#line 2092 "y.tab.c"
    break;

  case 56: /* simple_expr: '(' simple_expr ')'  */
#line 621 "parser.y"
                                                   { (yyval.pnodeP) = unop(lineno, cur_pc, PARENS, (yyvsp[-1].pnodeP)); }
#line 2098 "y.tab.c"
    break;

  case 57: /* simple_expr: CMPL simple_expr  */
#line 622 "parser.y"
                                                   { (yyval.pnodeP) = unop(lineno, cur_pc, CMPL, (yyvsp[0].pnodeP)); }
#line 2104 "y.tab.c"
    break;

  case 58: /* simple_expr: INTEGER  */
#line 624 "parser.y"
                {
		    (yyval.pnodeP) = newnode(lineno, cur_pc, INTEGER, NILP, NILP);
		    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
		}
#line 2113 "y.tab.c"
    break;

  case 59: /* simple_expr: OPCODE  */
#line 629 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, OPCODE, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2122 "y.tab.c"
    break;

  case 60: /* simple_expr: OPADDR  */
#line 634 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, OPADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2131 "y.tab.c"
    break;

  case 61: /* simple_expr: OPORABLE  */
#line 639 "parser.y"
                {
		    (yyval.pnodeP) = newnode(lineno, cur_pc, OPORABLE, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2140 "y.tab.c"
    break;

  case 62: /* simple_expr: VALUESPEC  */
#line 644 "parser.y"
                {
		    (yyval.pnodeP) = newnode(lineno, cur_pc, VALUESPEC, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2149 "y.tab.c"
    break;

  case 63: /* simple_expr: CONSTANT simple_expr endConst  */
#line 649 "parser.y"
                {
                int hash;
                SymNodeP symP;
                char *nameP;

                    // Jump thru hoops for constant compression
                    sprintf(scratchStr,"%ld",hashExpr((yyvsp[-1].pnodeP)));
                    if( !(symP = sym_find(&constSymP, scratchStr)) )
                    {
                        nameP = malloc(strlen(scratchStr) + 1);
                        strcpy(nameP, scratchStr);
                        symP = sym_make(nameP, 0);
                        sym_add(&constSymP, symP);
                        symP->ptr = (yyvsp[-1].pnodeP);
                    }
		    (yyval.pnodeP) = newnode(lineno, cur_pc, CONSTANT, NILP, (yyvsp[0].pnodeP));
                    (yyval.pnodeP)->value.symP = symP;
		}
#line 2172 "y.tab.c"
    break;

  case 64: /* simple_expr: DOT  */
#line 668 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, DOT, NILP, NILP);
                    (yyval.pnodeP)->value.ival = cur_pc;
                }
#line 2181 "y.tab.c"
    break;

  case 65: /* simple_expr: ADDR  */
#line 673 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, ADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2190 "y.tab.c"
    break;

  case 66: /* simple_expr: INTEGER bref  */
#line 678 "parser.y"
                {
		    (yyval.pnodeP) = newnode(lineno, cur_pc, INTEGER, NILP, NILP);
		    (yyval.pnodeP)->value.ival = (yyvsp[-1].ival) + ((yyvsp[0].ival) << 12);
                }
#line 2199 "y.tab.c"
    break;

  case 67: /* simple_expr: NAME bref  */
#line 683 "parser.y"
                {
                BankContextP bankP;
                SymNodeP symP;

                    if( !(bankP = findBank((yyvsp[0].ival))) )
                    {
                        verror("bank %d has not been used, it cannot be referenced", (yyvsp[0].ival));
                    }

                    if( !(symP = sym_find(&(bankP->globalSymP), (yyvsp[-1].strP))) )
                    {
                        // Nothing in that bank, go ahead and create it.
                        // If it's never resolved there, an error will be reported later.
                        symP = sym_make((yyvsp[-1].strP), 0);
                        sym_add(&(bankP->globalSymP), symP);
                        symP->flags = SYM_GLOB;
                        vwarn("bank %d is creating symbol %s in bank %d", curBank, (yyvsp[-1].strP), (yyvsp[0].ival));
                    }

                    if( !didBrefWarn )
                    {
                        vwarn(
                        "remember that a cross-bank reference is the full 16-bit address of %s, not 12 bits",
                        symP->name);
                        didBrefWarn = true;
                    }

                    (yyval.pnodeP) = newnode(lineno, cur_pc, BREF, NILP, NILP);
                    (yyval.pnodeP)->value.symP = symP;
                    (yyval.pnodeP)->value2.ival = (yyvsp[0].ival);
                }
#line 2235 "y.tab.c"
    break;

  case 68: /* simple_expr: ADDR bref  */
#line 715 "parser.y"
                {
                    // This is a symbol in our own bank, but that's ok
                    (yyval.pnodeP) = newnode(lineno, cur_pc, BREF, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[-1].symP);
                    (yyval.pnodeP)->value2.ival = (yyvsp[0].ival);
                }
#line 2246 "y.tab.c"
    break;

  case 69: /* simple_expr: NAME wildref  */
#line 722 "parser.y"
                {
                PNodeListP wildP;

                    if( !didBrefWarn )
                    {
                        vwarn(
                        "remember that a cross-bank reference is the full 16-bit address of %s, not 12 bits",
                        (yyvsp[-1].strP));
                        didBrefWarn = true;
                    }

                    (yyval.pnodeP) = newnode(lineno, cur_pc, WILDREF, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[-1].strP);

                    // We need this for fixup later
                    wildP = (PNodeListP)malloc(sizeof(PNodeListItem));
                    wildP->nodeP = (yyval.pnodeP);
                    wildP->nextP = wildcardsP;
                    wildcardsP = wildP;
                }
#line 2271 "y.tab.c"
    break;

  case 70: /* simple_expr: ADDR wildref  */
#line 743 "parser.y"
                {
                    // This is a symbol in our own bank, but that's ok
                    (yyval.pnodeP) = newnode(lineno, cur_pc, BREF, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[-1].symP);
                    (yyval.pnodeP)->value2.ival = curBank;
                }
#line 2282 "y.tab.c"
    break;

  case 71: /* simple_expr: NAME  */
#line 750 "parser.y"
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
                        sym_add(&globalSymP, symP2);
                        symP->flags = symP2->flags = SYMF_FORCED | SYM_LOC;
                        (yyval.pnodeP) = newnode(lineno, cur_pc, LCLADDR, NILP, NILP);
                    }
                    else
                    {
                        symP = sym_make((yyvsp[0].strP), 0);
                        symP->bank = curBank;
                        sym_add(&globalSymP, symP);
                        symP->flags = SYM_GLOB;
                        (yyval.pnodeP) = newnode(lineno, cur_pc, ADDR, NILP, NILP);
                    }

                    (yyval.pnodeP)->value.symP = symP;
                }
#line 2315 "y.tab.c"
    break;

  case 72: /* simple_expr: LCLNAME  */
#line 779 "parser.y"
                {
                SymNodeP symP;

                    // a symbol we haven't seen yet, add to the local symtab
                    if( !localContextP )
                    {
                        verror("local %s used outside a local scope", (yyvsp[0].strP));
                    }

                    symP = addLocalSymbol((yyvsp[0].strP));
                    symP->flags = SYM_LOC;
                    (yyval.pnodeP) = newnode(lineno, cur_pc, LCLADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = symP;
                }
#line 2334 "y.tab.c"
    break;

  case 73: /* simple_expr: LCLADDR  */
#line 794 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, LCLADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2343 "y.tab.c"
    break;

  case 74: /* simple_expr: FLEXO  */
#line 799 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, FLEXO, NILP, NILP);
                    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
                }
#line 2352 "y.tab.c"
    break;

  case 75: /* simple_expr: CHAR  */
#line 804 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, CHAR, NILP, NILP);
                    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
                }
#line 2361 "y.tab.c"
    break;

  case 76: /* simple_expr: LITCHAR  */
#line 809 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, LITCHAR, NILP, NILP);
                    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
                }
#line 2370 "y.tab.c"
    break;

  case 77: /* directive_expr: FORCELOC  */
#line 816 "parser.y"
                {
                    if( localDepth == 0 )
                    {
                        verror("%%forcelocal without an opening local");
                    }

                    localContextP->flags = CTX_FORCELOCAL;
                    sawForceLocal = true;
		    (yyval.pnodeP) = newnode(lineno, cur_pc, FORCELOC, NILP, NILP);
                    (yyval.pnodeP)->flags |= PN_NOINC;
                }
#line 2386 "y.tab.c"
    break;

  case 78: /* directive_expr: LOCAL optLocals  */
#line 828 "parser.y"
                {
                SymNodeP symP;
                PNodeP nodeP;
                char *cP;

                    // We push any current local scope, establish a new one
                    // locaSymlPP can be null if there is no current scope
                    if( (yyvsp[0].pnodeP) )
                    {
                        nodeP = (yyvsp[0].pnodeP)->leftP;      // recover head link
                        (yyvsp[0].pnodeP)->leftP = NILP;
                    }
                    else
                    {
                        nodeP = NILP;
                    }

		    (yyval.pnodeP) = newnode(lineno, cur_pc, LOCAL, NILP, nodeP);
                    (yyval.pnodeP)->flags |= PN_NOINC;

                    localStack[localDepth++] = localContextP;
                    if( localDepth > maxLocalDepth )
                    {
                        maxLocalDepth = localDepth;
                    }

                    localContextP = newLocalContext();
                    localContextP->pc = cur_pc;          // will be the origin for the local relative refs
                    sym_init( &(localContextP->symRootP) );

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
                        nodeP = nodeP->leftP;
                    }
                }
#line 2437 "y.tab.c"
    break;

  case 79: /* directive_expr: ENDLOC optINTEGER  */
#line 875 "parser.y"
                {
                    if( (yyvsp[0].ival) > 0 )
                    {
                        if( (yyvsp[0].ival) != localDepth )
                        {
                            vwarn("endloc says ending level %d but the current level is %d", (yyvsp[0].ival), localDepth);
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

		    (yyval.pnodeP) = newnode(lineno, cur_pc, ENDLOC, NILP, NILP);
                    (yyval.pnodeP)->flags |= PN_NOINC;
                    (yyval.pnodeP)->value.ival = (yyvsp[0].ival);
                }
#line 2465 "y.tab.c"
    break;

  case 80: /* endConst: ENDCONST  */
#line 901 "parser.y"
                {
                    (yyval.pnodeP) = NILP;
                }
#line 2473 "y.tab.c"
    break;

  case 81: /* endConst: COMMENT  */
#line 905 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, COMMENT, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                }
#line 2482 "y.tab.c"
    break;

  case 82: /* optLocals: symbol  */
#line 912 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    (yyval.pnodeP)->leftP = (yyval.pnodeP); // keep head
                }
#line 2491 "y.tab.c"
    break;

  case 83: /* optLocals: optLocals LOCATION symbol  */
#line 917 "parser.y"
                {
                    (yyvsp[0].pnodeP)->leftP = (yyvsp[-2].pnodeP)->leftP;
                    (yyvsp[-2].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                }
#line 2501 "y.tab.c"
    break;

  case 84: /* optLocals: %empty  */
#line 923 "parser.y"
                {
                    (yyval.pnodeP) = NILP;
                }
#line 2509 "y.tab.c"
    break;

  case 85: /* symList: symbol  */
#line 929 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                    (yyval.pnodeP)->leftP = (yyval.pnodeP); // keep head
                }
#line 2518 "y.tab.c"
    break;

  case 86: /* symList: symList LOCATION symbol  */
#line 934 "parser.y"
                {
                    (yyvsp[0].pnodeP)->leftP = (yyvsp[-2].pnodeP)->leftP;
                    (yyvsp[-2].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                }
#line 2528 "y.tab.c"
    break;

  case 87: /* symbol: NAME  */
#line 942 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, NAME, NILP, NILP);
                    (yyval.pnodeP)->value.strP = (yyvsp[0].strP);
                }
#line 2537 "y.tab.c"
    break;

  case 88: /* symbol: ADDR  */
#line 947 "parser.y"
                {
                    (yyval.pnodeP) = newnode(lineno, cur_pc, ADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                }
#line 2546 "y.tab.c"
    break;

  case 89: /* varnames: var  */
#line 954 "parser.y"
                {
                PNodeListP varP;

                    (yyval.pnodeP) = (yyvsp[0].pnodeP);

                    // Chain it into the list of unemitted vars
                    varP = (PNodeListP)malloc(sizeof(PNodeListItem));
                    varP->nodeP = (yyval.pnodeP);
                    varP->nextP = varNodesP;
                    varNodesP = varP;
                }
#line 2562 "y.tab.c"
    break;

  case 90: /* varnames: varnames LOCATION var  */
#line 966 "parser.y"
                {
                PNodeListP varP;

                    (yyvsp[0].pnodeP)->rightP = (yyvsp[-2].pnodeP);
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);

                    // And chain in the new var
                    varP = (PNodeListP)malloc(sizeof(PNodeListItem));
                    varP->nodeP = (yyvsp[0].pnodeP);
                    varP->nextP = varNodesP;
                    varNodesP = varP;
                }
#line 2579 "y.tab.c"
    break;

  case 91: /* bref: BREF INTEGER  */
#line 982 "parser.y"
                {
                    if( ((yyvsp[0].ival) < 0) || ((yyvsp[0].ival) > 15) )
                    {
                        verror("bank number must be 0-15 decimal, 0-17 octal");
                    }

                    (yyval.ival) = (yyvsp[0].ival);
                }
#line 2592 "y.tab.c"
    break;

  case 92: /* bref: BREF DOT  */
#line 991 "parser.y"
                {
                    (yyval.ival) = curBank;        // dot is a marker to indicate 'this bank'
                }
#line 2600 "y.tab.c"
    break;

  case 94: /* var: varname  */
#line 1000 "parser.y"
                {
                    (yyval.pnodeP) = (yyvsp[0].pnodeP);
                }
#line 2608 "y.tab.c"
    break;

  case 95: /* var: varname '=' simple_expr  */
#line 1004 "parser.y"
                {
                    (yyvsp[-2].pnodeP)->leftP = (yyvsp[0].pnodeP);
                    (yyval.pnodeP) = (yyvsp[-2].pnodeP);
                }
#line 2617 "y.tab.c"
    break;

  case 96: /* varname: NAME  */
#line 1010 "parser.y"
                {
                SymNodeP symP;

                    symP = sym_make((yyvsp[0].strP), 0);
                    symP->bank = curBank;
                    sym_add(&globalSymP, symP);
                    symP->flags = SYM_GLOB | SYMF_VAR;
                    (yyval.pnodeP) = newnode(lineno, cur_pc, ADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = symP;
                }
#line 2632 "y.tab.c"
    break;

  case 97: /* varname: ADDR  */
#line 1021 "parser.y"
                {
                    if( (yyvsp[0].symP)->flags & SYMF_RESOLVED )
                    {
                        verror("variable %s is already declared", (yyvsp[0].symP)->name);
                    }

                    (yyval.pnodeP) = newnode(lineno, cur_pc, ADDR, NILP, NILP);
                    (yyval.pnodeP)->value.symP = (yyvsp[0].symP);
                    (yyvsp[0].symP)->flags = SYM_GLOB | SYMF_VAR;
                }
#line 2647 "y.tab.c"
    break;


#line 2651 "y.tab.c"

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

#line 1031 "parser.y"


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
            sym_add(&globalSymP, symP);
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
            symP->flags |= SYMF_RESOLVED;
            symP->value = cur_pc++;
            symP->bank = bank;
            nodeP->pc = symP->value;
            nodeP->value2.ival = bank;
        }

        listP = listP->nextP;
    }
}

// Process a symbol file, bring in all exported ones.
// If the filename starts with <, it will be terminated with the same and means system include.
// Exported symbols found are created as globals in the bank they were defined in, a bank context
// will be created if needed.
void
importSymbols(char *filenameP)
{
int bank;
int origBank;
int lastBank;
long address;
char *cP;
FILE *infP;
SymNodeP symP;
BankContextP bankP;
char str[256];

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

    fgets(str, sizeof(str), infP);          // discard first line, the file name
    while( fgets(str, sizeof(str), infP) )
    {
        address = strtol(str, &cP, 8);      // first the address, which is octal
        bank = address >> 12;
        address &= 07777;

        if( *(++cP) != 'X' )
        {
            continue;                       // not an exported symbol
        }

        cP += 2;                            // now the symbol name
        *(cP + strlen(cP) - 1) = 0;         // get rid of the newline at the end

        if( bank != lastBank )
        {
            bankP = swapBanks(bank);
            lastBank = bank;
        }

        if( sym_find(&globalSymP, cP) )
        {
            fclose(infP);
            verror("imported symbol '%s' has already been defined", cP);
        }

        symP = sym_make(cP, 0);
        symP->flags |= SYMF_IMPORTED | SYMF_RESOLVED | SYM_GLOB;
        symP->value = address;
        symP->bank = bank;
        sym_add(&globalSymP, symP);
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

// Used when bank is processed to switch between bank states
BankContextP
swapBanks(int newBank)
{
BankContextP newP;
BankContextP curP;

    if( !(curP = findBank(curBank)) )
    {
        // First time we've left this bank, add an entry.
        curP = addBank(curBank);
        sawBank = true;
    }

    curP->cur_pc = cur_pc;
    curP->globalSymP = globalSymP;
    curP->constSymP = constSymP;
    curP->varNodesP = varNodesP;

    newP = findBank(newBank);

    if( newP )
    {
        // we've been here before, restore the state
        cur_pc = newP->cur_pc;
        globalSymP = newP->globalSymP;
        constSymP = newP->constSymP;
        varNodesP = newP->varNodesP;
    }
    else
    {
        // fresh start
        sym_init(&globalSymP);
        sym_init(&constSymP);
        varNodesP = NILP;
        cur_pc = 0;

        // First time we've been to this bank, add an entry.
        newP = addBank(newBank);
        newP->cur_pc = cur_pc;
        sawBank = true;
    }

    curBank = newBank;
    return( newP );
}

BankContextP
addBank(int bank)
{
BankContextP newP;

    newP = (BankContextP)calloc(1, sizeof(BankContext));
    newP->bank = bank;
    newP->cur_pc = 0;       // new banks start at 0
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

int
yywrap()				/* tell lex to clean up */
{
    return(1);
}

void
vwarn(const char *msgP, ...)
{
va_list argP;
char format[1024];

    if( noWarn )
        return;
    va_start(argP, msgP);
    sprintf(format,"am1: WARNING: %s\nat line %d, file %s\n",
        msgP,lineno,filenameP);
    vfprintf(stderr,format,argP);
    va_end(argP);
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
