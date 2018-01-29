%{
/*
Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
 * Contains the lexical specification for Intel Gen Assembly.
 * Build with:
 *   % flex ThisFile.flex
 * First constructed with flex 2.5.39
 * It should build without warnings.
 */

#include "Lexemes.hpp"

#define YY_DECL iga::Lexeme yylex (yyscan_t yyscanner, unsigned int &inp_off)

/*
 * It seems many versions of flex don't support column info in re-entrant
 * scanners.  This works around the issue.
 */
#define YY_USER_ACTION \
    yyset_column(yyget_column(yyscanner) + (int)yyget_leng(yyscanner), yyscanner);

%}

%option outfile="lex.yy.cpp" header-file="lex.yy.hpp"
%option nounistd
%option reentrant
%option noyywrap
%option yylineno
/* omits isatty */
%option never-interactive

%x SLASH_STAR
%x STRING_DBL
%x STRING_SNG
%%

<SLASH_STAR>"*/"      { inp_off += 2; BEGIN(INITIAL); }
<SLASH_STAR>[^*\n]+   { inp_off += (unsigned int)yyget_leng(yyscanner); } // eat comment in line chunks
<SLASH_STAR>"*"       { inp_off++; } // eat the lone star
<SLASH_STAR>\n        { inp_off++; }

<STRING_DBL>\"        { inp_off++;
                        BEGIN(INITIAL);
                        return iga::Lexeme::STRLIT; }
<STRING_DBL>\\.       { inp_off += 2; }
<STRING_DBL>.         { inp_off++; }

<STRING_SNG>\'        { inp_off++;
                        BEGIN(INITIAL);
                        return iga::Lexeme::CHRLIT; }
<STRING_SNG>\\.       { inp_off += 2; }
<STRING_SNG>.         { inp_off++; }

"/*"                  {inp_off += 2; BEGIN(SLASH_STAR);}
\<                    return iga::Lexeme::LANGLE;
\>                    return iga::Lexeme::RANGLE;
\[                    return iga::Lexeme::LBRACK;
\]                    return iga::Lexeme::RBRACK;
\{                    return iga::Lexeme::LBRACE;
\}                    return iga::Lexeme::RBRACE;
\(                    return iga::Lexeme::LPAREN;
\)                    return iga::Lexeme::RPAREN;

\|                    return iga::Lexeme::PIPE;
\&                    return iga::Lexeme::AMP;
\$                    return iga::Lexeme::DOLLAR;
\.                    return iga::Lexeme::DOT;
\,                    return iga::Lexeme::COMMA;
\;                    return iga::Lexeme::SEMI;
\:                    return iga::Lexeme::COLON;

\~                    return iga::Lexeme::TILDE;
\(abs\)               return iga::Lexeme::ABS;
\(sat\)               return iga::Lexeme::SAT;

\!                    return iga::Lexeme::BANG;
\@                    return iga::Lexeme::AT;
\#                    return iga::Lexeme::HASH;
\=                    return iga::Lexeme::EQ;

\%                    return iga::Lexeme::MOD;
\*                    return iga::Lexeme::MUL;
\/                    return iga::Lexeme::DIV;
\+                    return iga::Lexeme::ADD;
\-                    return iga::Lexeme::SUB;
\<<                   return iga::Lexeme::LSH;
\>>                   return iga::Lexeme::RSH;

[_a-zA-Z][_a-zA-Z0-9]*  return iga::Lexeme::IDENT;

0[bB][01]+             return iga::Lexeme::INTLIT02;
[0-9]+                 return iga::Lexeme::INTLIT10;
0[xX][0-9A-Fa-f]+      return iga::Lexeme::INTLIT16;

[0-9]+\.[0-9]+([eE][-+]?[0-9]+)?  return iga::Lexeme::FLTLIT;
[0-9]+[eE][-+]?[0-9]+  return iga::Lexeme::FLTLIT;

\n                     return iga::Lexeme::NEWLINE; /* newlines are explicitly represented */
[ \t\r]+               { inp_off += (unsigned int)yyget_leng(yyscanner); } /* whitespace */;
"//"[^\n]*             { inp_off += (unsigned int)yyget_leng(yyscanner); } /* EOL comment ?*/

.                    return iga::Lexeme::LEXICAL_ERROR;
<<EOF>>              return iga::Lexeme::END_OF_FILE;

%%
