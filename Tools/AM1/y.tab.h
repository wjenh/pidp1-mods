/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

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

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

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
    

#line 146 "y.tab.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void);


#endif /* !YY_YY_Y_TAB_H_INCLUDED  */
