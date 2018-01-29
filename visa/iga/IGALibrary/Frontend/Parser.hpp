/*===================== begin_copyright_notice ==================================

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


======================= end_copyright_notice ==================================*/
#ifndef IGA_FRONTEND_PARSER_HPP
#define IGA_FRONTEND_PARSER_HPP

#include "BufferedLexer.hpp"
#include "../ErrorHandler.hpp"
// FIXME: needed for for ident_value<T> (want to make this agnostic IGA GEN IR types)
#include "../Models/Models.hpp"
#include "../IR/Loc.hpp"


#include <cstdarg>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iga
{
    template<typename T> using IdentMap =
        std::initializer_list<std::pair<const char *,T>>;

    // this type is used to bail out of the parsing algorithm upon syntax error
    class SyntaxError : std::runtime_error {
    public:
        const struct Loc loc;
        std::string message;

        SyntaxError(const struct Loc &l, const std::string &m) throw ()
            : std::runtime_error(m)
            , loc(l)
            , message(m)
        {
        }
        ~SyntaxError() throw () { }
    };

    ///////////////////////////////////////////////////////////////////////////
    // Recursive descent parser.
    // The nomaclaure for method names is roughly:
    //   Looking****  peeks at the token, doesn't consume
    //   Consume****  consume next token if some criteria is true
    //   Parse******  generally corresponds to a non-terminal or some
    //                complicated lexemes
    //
    class Parser {
    public:
        Parser(const std::string &inp, ErrorHandler &errHandler)
            : m_lexer(inp)
            , m_errorHandler(errHandler)
        {
        }

    protected:
        BufferedLexer                  m_lexer;
        ErrorHandler                  &m_errorHandler;

        //////////////////////////////////////////////////////////////////////
        // DEBUGGING
        void DumpLookaheads(int n = 1) const {m_lexer.DumpLookaheads(n); }
        void ShowCurrentLexicalContext() {ShowCurrentLexicalContext(NextLoc());}
        void ShowCurrentLexicalContext(const Loc &loc) const;

        //////////////////////////////////////////////////////////////////////
        // ERRORS and WARNINGS
        void Fail(const char *msg) {Fail(0, msg);}
        void Fail(int i, const char *msg) {Fail(NextLoc(i), msg);}
        void Fail(const Loc &loc, const std::string &msg);
        void Fail(const Loc &loc, const char *msg);
        void FailF(const char *pat, ...);
        void FailF(const Loc &loc, const char *pat, ...);
        void FailAtPrev(const char *msg);

        void Warning(const Loc &loc, const char *msg);
        void Warning(const char *msg);
        void WarningF(const char *pat, ...);
        void WarningF(const Loc &loc, const char *pat, ...);

        //////////////////////////////////////////////////////////////////////
        // BASIC and GENERAL FUNCTIONS
        const Token &Next(int i = 0) const {return m_lexer.Next(i);}

        Loc NextLoc(int i = 0) const {return Next().loc;}

        uint32_t ExtentToPrevEnd(const Loc &start) const;

        uint32_t ExtentTo(const Loc &start, const Loc &end) const;

        bool EndOfFile() const {return m_lexer.EndOfFile();}

        bool Skip(int k = 1) {return m_lexer.Skip(k);}

        // be sure nothing else fits before you use this
        std::string GetTokenAsString(const Token &token) const;

        //////////////////////////////////////////////////////////////////////
        // QUERYING (non-destructive lookahead)
        bool LookingAt(Lexeme lxm) const {return LookingAt(0,lxm);}
        bool LookingAt(int k, Lexeme lxm) const;

        bool LookingAtSeq(Lexeme lxm0, Lexeme lxm1) const {return LookingAtSeq({lxm0,lxm1});}
        bool LookingAtSeq(std::initializer_list<Lexeme> lxms) const;

        bool LookingAtAnyOf(Lexeme lxm0, Lexeme lxm1) const {return LookingAtAnyOf({lxm0,lxm1}); }
        bool LookingAtAnyOf(std::initializer_list<Lexeme> lxms) const;
        bool LookingAtAnyOf(int i, std::initializer_list<Lexeme> lxms) const;

        bool LookingAtPrefix(const char *pfx) const;

        //////////////////////////////////////////////////////////////////////
        // CONSUMPTION (destructive lookahead)
        bool Consume(Lexeme lxm) {return Consume(0, lxm);}
        bool Consume(int k, Lexeme lxm) {return m_lexer.Consume(lxm,k);}
        void ConsumeOrFail(Lexeme lxm, const char *msg);
        // same as above, but the error location chosen is the end of the
        // previous token; i.e. the suffix is screwed up
        void ConsumeOrFailAfterPrev(Lexeme lxm, const char *msg);
        bool Consume(Lexeme lxm0, Lexeme lxm1) {
            // first block doesn't require a label
            if (LookingAtSeq(lxm0,lxm1)) {
                return Skip(2);
            }
            return false;
        }

        //////////////////////////////////////////////////////////////////////
        // IDENTIFIER and RAW STRING MANIPULATION
        bool PrefixAtEq(size_t off, const char *pfx) const;

        bool LookingAtIdentEq(const char *eq) const;
        bool LookingAtIdentEq(int k, const char *eq) const;
        bool LookingAtIdentEq(const Token &tk, const char *eq) const;
        bool ConsumeIdentEq(const char *eq);
        std::string ConsumeIdentOrFail();

        bool TokenEq(const Token &tk, const char *eq) const;

        template <typename T>
        bool IdentLookup(int k, const IdentMap<T> map, T &value) const {
            if (!LookingAt(k,IDENT)) {
                return false;
            }
            for (const std::pair<const char *,T> &p : map) {
                if (TokenEq(Next(k), p.first)) {
                    value = p.second;
                    return true;
                }
            }
            return false;
        }

        template <typename T>
        void ConsumeIdentOneOfOrFail(
            const IdentMap<T> map,
            T &value,
            const char *err_expecting,
            const char *err_invalid)
        {
            if (!LookingAt(IDENT)) {
                Fail(err_expecting);
            }
            if (!IdentLookup(0, map, value)) {
                Fail(err_invalid);
            }
            Skip();
        }

        template <typename T>
        bool ConsumeIdentOneOf(const IdentMap<T> map, T &value) {
            if (LookingAt(IDENT) && IdentLookup(0, map, value)) {
                Skip();
                return true;
            }
            return false;
        }

/*
        //////////////////////////////////////////////////////////////////////
        // TODO: remove
        template <typename T>
        bool IdentLookup(int k,
            const struct ident_value<T> *map,
            T &value) const
        {
            if (!LookingAt(k,IDENT)) {
                return false;
            }
            int i = 0;
            while (map[i].key) {
                if (TokenEq(Next(k), map[i].key)) {
                    value = map[i].value;
                    return true;
                }
                i++;
            }
            return false;
        }

        template <typename T>
        void ConsumeIdentOneOfOrFail(
            const struct ident_value<T> *map,
            T &value,
            const char *err_expecting,
            const char *err_invalid)
        {
            if (!LookingAt(IDENT)) {
                Fail(err_expecting);
            }
            if (!IdentLookup(0, map, value)) {
                Fail(err_invalid);
            }
            Skip();
        }

        template <typename T>
        bool ConsumeIdentOneOf(
            const struct ident_value<T> *map,
            T &value)
        {
            if (LookingAt(IDENT) && IdentLookup(0,map,value)) {
                Skip();
                return true;
            }
            return false;
        }

*/

        ///////////////////////////////////////////////////////////////////////////
        // NUMBERS
        //
        template <typename T>
        bool ConsumeIntLit(T &value) {
            if (LookingAtAnyOf({INTLIT02, INTLIT10, INTLIT16})) {
                ParseIntFrom(NextLoc(), value);
                Skip();
                return true;
            }
            return false;
        }

        template <typename T>
        void ConsumeIntLitOrFail(T &value, const char *err) {
            if (!ConsumeIntLit(value)) {
                Fail(err);
            }
        }

        // Examples:
        //   3.141
        //    .451
        //   3.1e7
        //   3e9
        //   3e9.5
        void ParseFltFrom(const Loc loc, double &value);

        template <typename T>
        void ParseIntFrom(const Loc &loc, T &value) {
            ParseIntFrom(loc.offset, loc.extent, value);
        }

        template <typename T>
        void ParseIntFrom(size_t off, size_t len, T &value) {
            const std::string &src = m_lexer.GetSource();
            value = 0;
            if (len > 2 &&
                src[off] == '0' &&
                (src[off + 1] == 'b' || src[off + 1] == 'B'))
            {
                for (size_t i = 2; i < len; i++) {
                    char chr = src[off + i];
                    T next_value = 2 * value + chr - '0';
                    if (next_value < value) {
                        Fail(-1, "integer literal too large");
                    }
                    value = next_value;
                }
            } else if (len > 2 &&
                src[off] == '0' &&
                (src[off + 1] == 'x' || src[off + 1] == 'X'))
            {
                for (size_t i = 2; i < len; i++) {
                    char chr = src[off + i];
                    T dig = 0;
                    if (chr >= '0' && chr <= '9')
                        dig = chr - '0';
                    else if (chr >= 'A' && chr <= 'F')
                        dig = chr - 'A' + 10;
                    else if (chr >= 'a' && chr <= 'f')
                        dig = chr - 'a' + 10;
                    T next_value = 16 * value + dig;
                    if (next_value < value) {
                        Fail(-1, "integer literal too large");
                    }
                    value = next_value;
                }
            } else {
                for (size_t i = 0; i < len; i++) {
                    char chr = src[off + i];
                    T next_value = 10 * value + chr - '0';
                    if (next_value < value) {
                        Fail(-1, "integer literal too large");
                    }
                    value = next_value;
                }
            }
        }
    }; // Parser
} // namespace IGA


#endif // IGA_FRONTEND_PARSER_HPP
