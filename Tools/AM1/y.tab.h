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
    

#line 138 "y.tab.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void);


#endif /* !YY_YY_Y_TAB_H_INCLUDED  */
