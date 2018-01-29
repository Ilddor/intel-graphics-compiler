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
#include "BufferedLexer.hpp"
#include "Floats.hpp"
#include "KernelParser.hpp"
#include "Lexemes.hpp"
#include "Parser.hpp"
#include "../IR/InstBuilder.hpp"
#include "../IR/Types.hpp"
#include "../strings.hpp"

#include <limits>
#include <map>
#include <string>
#include <vector>


using namespace iga;

static const IdentMap<FlagModifier> FLAGMODS {
    {"lt", FlagModifier::LT},
    {"le", FlagModifier::LE},
    {"gt", FlagModifier::GT},
    {"ge", FlagModifier::GE},
    {"eq", FlagModifier::EQ},
    {"ne", FlagModifier::NE},
    {"ov", FlagModifier::OV},
    {"un", FlagModifier::UN},
    {"eo", FlagModifier::EO},
    // aliased by different operations
    {"ze", FlagModifier::EQ},
    {"nz", FlagModifier::NE},
};

static const IdentMap<FlagModifier> FLAGMODS_LEGACY {
    {"l", FlagModifier::LT},
    {"g", FlagModifier::GT},
    {"e", FlagModifier::EQ},
    {"o", FlagModifier::OV},
    {"u", FlagModifier::UN},
    {"z", FlagModifier::EQ},
};


static const IdentMap<Type> SRC_TYPES = {
    {"b",  Type::B},
    {"ub", Type::UB},
    {"w",  Type::W},
    {"uw", Type::UW},
    {"d",  Type::D},
    {"ud", Type::UD},
    {"q",  Type::Q},
    {"uq", Type::UQ},
    {"hf", Type::HF},
    {"f",  Type::F},
    {"df", Type::DF},
    {"v",  Type::V},
    {"uv", Type::UV},
    {"vf", Type::VF},
    {"nf", Type::NF},
};
static const IdentMap<Type> DST_TYPES = {
    {"b",  Type::B},
    {"ub", Type::UB},
    {"w",  Type::W},
    {"uw", Type::UW},
    {"d",  Type::D},
    {"ud", Type::UD},
    {"q",  Type::Q},
    {"uq", Type::UQ},
    {"hf", Type::HF},
    {"f",  Type::F},
    {"df", Type::DF},
    {"nf", Type::NF},
};
static const IdentMap<ImplAcc> IMPLACCS = {
    {"acc2", ImplAcc::ACC2},
    {"acc3", ImplAcc::ACC3},
    {"acc4", ImplAcc::ACC4},
    {"acc5", ImplAcc::ACC5},
    {"acc6", ImplAcc::ACC6},
    {"acc7", ImplAcc::ACC7},
    {"acc8", ImplAcc::ACC8},
    {"acc9", ImplAcc::ACC9},
    {"noacc", ImplAcc::NOACC},
};



class KernelParser : Parser {
    const Model&                   m_model;
    InstBuilder&                   m_handler;

    const ParseOpts                m_parseOpts;

    // maps mnemonics and registers for faster lookup
    std::map<std::string,const OpSpec*>   opmap;
    std::map<std::string,const RegInfo*>  regmap;

    int                            m_defaultExecutionSize;
    Type                           m_defaultRegisterType;

public:
    KernelParser(
        const Model &model,
        InstBuilder &handler,
        const std::string &inp,
        ErrorHandler &eh,
        const ParseOpts &pots)
        : Parser(inp,eh)
        , m_model(model)
        , m_handler(handler)
        , m_parseOpts(pots)

        , m_defaultExecutionSize(1)
        , m_defaultRegisterType(Type::INVALID)
    {
        initSymbolMaps();
    }


    void ParseListing() {
        ParseProgram();
    }
private:
    // instruction state
    bool                  m_hasWrEn;
    Type                  m_unifType;
    const OpSpec         *m_opSpec;
    const Token          *m_unifTypeTk;
    RegRef                m_flagReg;
    int                   m_execSize;
    int                   m_chOff;

    // Message               m_ldStInst;

    Operand::Kind         m_srcKinds[3];
    Loc                   m_srcLocs[3];

    void initSymbolMaps() {
        // map mnemonics names to their ops
        // subops only get mapped by their fully qualified names in this pass
        std::vector<const OpSpec *> subOps;
        for (const OpSpec *os : m_model.ops()) {
            if (os->isValid()) {
                if (os->isSubop()) {
                    opmap[os->fullMnemonic] = os;
                    subOps.emplace_back(os);
                } else {
                    opmap[os->mnemonic] = os;
                }
            }
        }
        // subops get mapped by their short names only if that does not
        // conflict with some other op.
        // e.g. "sync.nop" will not parse as "nop", but as the real nop.
        for (auto os : subOps) {
            // frequency of a short name in the subops
            auto subOpMnemonicFreq = [&](const std::string &mne) {
                int k = 0;
                for (auto os : subOps) {
                    if (mne == os->mnemonic) {
                        k++;
                    }
                }
                return k;
            };
            // e.g. SYNC_NOP's "nop" conflicts with NOP
            bool conflictsWithRealOp = opmap.find(os->mnemonic) != opmap.end();
            // e.g. A_C's "c" short name would conflict with B_C's "c" short op
            bool conflictsWithOtherSubOp = subOpMnemonicFreq(os->mnemonic) != 1;
            if (!conflictsWithRealOp && !conflictsWithOtherSubOp) {
                opmap[os->mnemonic] = os;
            }
        }
        // map the register names
        // this maps just the non-number part.
        // e.g. with cr0, this maps "cr"; see LookupReg()
        for (size_t i = 0; i < sizeof(registers)/sizeof(registers[0]); i++) {
            const RegInfo *ri = &registers[i];
            if (ri->supportedOn(m_model.platform)) {
                regmap[ri->name] = ri;
            }
        }
    }

    template <typename S>
    void Error(const Loc &loc, const S &m1) {
        std::stringstream ss;
        ss << m1;
        Fail(loc, ss.str());
    }
    template <typename S, typename T>
    void Error(const Loc &loc, const S &m1, const T &m2) {
        std::stringstream ss;
        ss << m1;
        ss << m2;
        Fail(loc, ss.str());
    }
    template <typename S, typename T, typename U>
    void Error(const Loc &loc, const S &m1, const T &m2, const U &m3) {
        std::stringstream ss;
        ss << m1;
        ss << m2;
        ss << m3;
        Fail(loc, ss.str());
    }
    bool isMacroOp() const {
        return m_opSpec->isMacro();
    }


    // Program = (Label? Insts* (Label Insts))?
    void ParseProgram() {
        m_handler.ProgramStart();

        // parse default type directives
        if (m_parseOpts.supportLegacyDirectives) {
            // e.g. .default_execution_size...
            ParseLegacyDirectives();
        }

        // first block doesn't need a label
        if (!LookingAtLabelDef() && !EndOfFile()) {
            ParseBlock(NextLoc(),""); // unnamed
        }
        // successive blocks need a label
        std::string label;
        Loc lblLoc = NextLoc();
        while (ConsumeLabelDef(label)) {
            ParseBlock(lblLoc, label);
            lblLoc = NextLoc();
        }
        if (!EndOfFile()) {
            Fail("expected instruction, block, or EOF");
        }

        m_handler.ProgramEnd();
    }

    // fix to support .default_execution_size
    bool ParseLegacyDirectives() {
        int parsed = 0;
        try {
            while (LookingAtSeq(Lexeme::DOT,Lexeme::IDENT)) {
                Skip();
                parsed++;
                if (ConsumeIdentEq("default_execution_size")) {
                    Consume(LPAREN);
                    auto loc = NextLoc();
                    if (!ConsumeIntLit<int>(m_defaultExecutionSize)) {
                        Fail("expected SIMD width (integral value)");
                    }
                    if (m_defaultExecutionSize !=  1 &&
                        m_defaultExecutionSize !=  2 &&
                        m_defaultExecutionSize !=  4 &&
                        m_defaultExecutionSize !=  8 &&
                        m_defaultExecutionSize != 16 &&
                        m_defaultExecutionSize != 32)
                    {
                        Fail(loc, "invalid default execution size; "
                            "must be 1, 2, 4, 8, 16, 32");
                    }
                    Consume(RPAREN);
                    Consume(NEWLINE);
                } else if (ConsumeIdentEq("default_register_type")) {
                    m_defaultRegisterType = TryParseOpType(DST_TYPES);
                    if (m_defaultRegisterType == Type::INVALID) {
                        Fail("expected default register type");
                    }
                    Consume(NEWLINE);
                } else {
                    Fail("unexpected directive name");
                }
            } // while
        } catch (const SyntaxError &s) {
            RecoverFromSyntaxError(s);
            // returning true will make ParseBlock restart the loop
            // (so more directives get another shot)
            return true;
        }
        return parsed > 0;
    }

    void RecoverFromSyntaxError(const SyntaxError &s) {
        // record the error in this instruction
        m_errorHandler.reportError(s.loc, s.message);
        // bail if we've reached the max number of errors
        if (m_errorHandler.getErrors().size() >=
            m_parseOpts.maxSyntaxErrors)
        {
            throw s;
        }
        // attempt resync by skipping until we pass a newline
        // DumpLookaheads();
        while (!Consume(Lexeme::NEWLINE) &&
            // !Consume(Lexeme::SEMI) &&
            //   don't sync on ; since this is typically part of a
            //   region ratherthan an operand separator
            //  ...     r12.0<8;8,1>:d
            //  ^ error        ^ syncs here otherwise
            !LookingAt(Lexeme::END_OF_FILE))
        {
            // DumpLookaheads();
            Skip();
        }
    }

    void ParseBlock(const Loc &lblLoc, const std::string &label) {
        m_handler.BlockStart(lblLoc, label);
        auto lastInst = NextLoc();
        while (true) {
            if (Consume(Lexeme::NEWLINE) || Consume(Lexeme::SEMI)) {
                continue;
            } else if (m_parseOpts.supportLegacyDirectives &&
                ParseLegacyDirectives())
            {
                // .default_execution_size...
                continue;
            }
            // if we make it here, then we are not looking at a legacy
            // directive and we are not looking at a newline
            if (LookingAtLabelDef() || EndOfFile()) {
                break;
            }
            // not at EOF and not looking at the beginning of new block
            // so we better be looking at an instruction
            ParseInst();
            lastInst = NextLoc(-1); // peek at the end of the last instruction
        }

        m_handler.BlockEnd(ExtentTo(lblLoc,lastInst));
    }


    void ParseInst() {
        try {
            // parse one instruction
            // DumpLookaheads();
            ParseInstBody();
        } catch (SyntaxError &s) {
            // we resync here and return from ParseInst
            RecoverFromSyntaxError(s);
        }
    }


    // Instruction = Predication? Mnemonic UniformType? EMask
    //                 ConditionModifier? Operands InstOptions?
    //
    void ParseInstBody() {
        // ShowCurrentLexicalContext();
        const Loc startLoc = NextLoc();
        m_handler.InstStart(startLoc);

        // reset all internal instruction state
        // m_ldStInst.family = Message::Family::INVALID;

        m_flagReg = REGREF_INVALID;
        m_opSpec = nullptr;
        m_unifType = Type::INVALID;
        m_unifTypeTk = nullptr;
        for (size_t i = 0; i < sizeof(m_srcKinds)/sizeof(m_srcKinds[0]); i++)
            m_srcKinds[i] = Operand::Kind::INVALID;

        // (W&~f0) mov (8|M0) r1 r2
        // ^
        ParseWrEnPred();

        // (W&~f0) mov (8|M0) r1 r2
        //         ^
        ParseMnemonic();

        // if (Consume(COLON)) {
        //    m_unifTypeTk = &Next(0);
        //    ConsumeIdentOneOfOrFail(
        //        dstTypes,
        //        m_unifType,
        //        "expected uniform type",
        //        "invalid uniform type");
        // }

        // (W&~f0) mov (8|M0) (le)f0.0 r1 r2
        //             ^
        ParseExecInfo();

        ParseFlagModOpt();
        switch (m_opSpec->format) {
        case OpSpec::NULLARY:
            // nop
            // illegal
            break; // fallthrough to instruction options
        case OpSpec::BASIC_UNARY_REG:
        case OpSpec::BASIC_UNARY_REGIMM:
        case OpSpec::MATH_UNARY_REGIMM:
        case OpSpec::MATH_MACRO_UNARY_REG:
            ParseDstOp();
            ParseSrcOp(0);
            if (m_opSpec->format == OpSpec::BASIC_UNARY_REG &&
                m_srcKinds[0] != Operand::Kind::DIRECT &&
                m_srcKinds[0] != Operand::Kind::INDIRECT)
            {
                Fail(m_srcLocs[0], "src0 must be a register");
            }
            break;
        case OpSpec::SYNC_UNARY:
            // implicit destination
            ParseSrcOp(0);
            if (m_model.supportsWaitDirect() &&
                m_srcKinds[0] != Operand::Kind::DIRECT)
            {
                Fail(
                    m_srcLocs[0],
                    "src0 must be a direct notification register");
            }
            break;
        case OpSpec::SEND_UNARY:
            ParseSendDstOp();
            ParseSendSrcOp(0, false);
            ParseSendDescs();
            break;
        case OpSpec::SEND_BINARY:
            ParseSendDstOp();
            ParseSendSrcOp(0, false);
            ParseSendSrcOp(1,
                m_model.sendCheck3() &&
                m_parseOpts.supportLegacyDirectives);
            ParseSendDescs();
            break;
        case OpSpec::BASIC_BINARY_REG_IMM:
        case OpSpec::BASIC_BINARY_REG_REG:
        case OpSpec::BASIC_BINARY_REG_REGIMM:
        case OpSpec::MATH_BINARY_REG_REGIMM:
        case OpSpec::MATH_MACRO_BINARY_REG_REG:
            ParseDstOp();
            ParseSrcOp(0);
            ParseSrcOp(1);
            break;
        case OpSpec::TERNARY_REGIMM_REG_REGIMM:
        case OpSpec::TERNARY_MACRO_REG_REG_REG:
            ParseDstOp();
            ParseSrcOp(0);
            ParseSrcOp(1);
            ParseSrcOp(2);
            break;
        case OpSpec::JUMP_UNARY_IMM:
            ParseSrcOpLabel(0);
            break;
        case OpSpec::JUMP_UNARY_REGIMM:
            // e.g. jmpi or brd  (registers or labels)
            ParseSrcOp(0);
            break;
        case OpSpec::JUMP_UNARY_REG:
            // e.g. ret (..) r12
            ParseSrcOp(0);
            if (m_srcKinds[0] != Operand::Kind::DIRECT &&
                m_srcKinds[0] != Operand::Kind::INDIRECT)
            {
                Fail(m_srcLocs[0], "src0 must be a register");
            }
            break;
        case OpSpec::JUMP_BINARY_BRC: {
            //   brc (...) lbl lbl
            //   brc (...) reg null
            //
            // for legacy reasons we support a unary form
            //   brc (...) reg
            // we do add an explicit null parameter for src1
            // it's very possible there are some simulator tests floating
            // aroudn that require this old form.
            auto brcStart = NextLoc();
            ParseSrcOp(0);
            if (m_srcKinds[0] == Operand::Kind::IMMEDIATE ||
                m_srcKinds[0] == Operand::Kind::LABEL ||
                LookingAtIdentEq("null"))
            {
                ParseSrcOp(1);
            } else {
                // legacy format
                // add an explicit null operand
                const RegInfo *ri = nullptr;
                int reg; // discarded
                (void)LookupReg("null", ri, reg);
                IGA_ASSERT(ri != nullptr, "failed to lookup null register");
                // trb: this is no longer a warning, but we still set the
                // parameter in the IR for the time being
                //
                // if (m_parseOpts.deprecatedSyntaxWarnings) {
                //    Warning(brcStart,
                //        "deprecated syntax: register version "
                //        "of brc takes an explicit null parameter");
                // }
                m_handler.InstSrcOpRegDirect(
                    1,
                    brcStart,
                    SrcModifier::NONE,
                    *ri,
                    REGREF_ZERO_ZERO,
                    Region::SRC010,
                    Type::UD);
            }
            break;
        }
        case OpSpec::JUMP_UNARY_CALL_REGIMM:
            // call (..) dst src
            // calla (..) dst src
            ParseDstOp();
            ParseSrcOp(0);
            break;
        case OpSpec::JUMP_BINARY_IMM_IMM:
            // e.g. else, cont, break, goto, halt
            ParseSrcOpLabel(0);
            ParseSrcOpLabel(1);
            break;
        default:
            IGA_ASSERT_FALSE("unhandled syntax class in parser");
        }
        ParseInstOpts();

        if (!LookingAt(Lexeme::NEWLINE) &&
            !LookingAt(Lexeme::SEMI) &&
            !LookingAt(Lexeme::END_OF_FILE))
        {
            FailAtPrev("expected '\\n', ';', or EOF");
        }

        m_handler.InstEnd(ExtentToPrevEnd(startLoc));
    }


    // Predication = ('(' WrEnPred ')')?
    //   where WrEnPred = WrEn
    //                  | Pred
    //                  | WrEn '&' Pred
    //         WrEn = 'W'
    //         Pred = '~'? FlagReg ('.' PreCtrl?)
    void ParseWrEnPred() {
        m_hasWrEn = false;
        if (Consume(LPAREN)) {
            const Loc nmLoc = NextLoc(0);
            if (ConsumeIdentEq("W")) {
                m_hasWrEn = true;
                m_handler.InstNoMask(nmLoc);
                if (Consume(AMP)) {
                    ParsePred();
                }
            } else {
                ParsePred();
            }
            ConsumeOrFail(RPAREN, "expected )");
        }
    }


    // Pred = '~'? FlagReg ('.' PreCtrl?)
    void ParsePred() {
        static const IdentMap<PredCtrl> PREDCTRLS = {
            {"xyzw", PredCtrl::SEQ}, // lack of a specific pred control defaults to SEQ
            {"anyv", PredCtrl::ANYV},
            {"allv", PredCtrl::ALLV},
            {"any2h", PredCtrl::ANY2H},
            {"all2h", PredCtrl::ALL2H},
            {"any4h", PredCtrl::ANY4H},
            {"all4h", PredCtrl::ALL4H},
            {"any8h", PredCtrl::ANY8H},
            {"all8h", PredCtrl::ALL8H},
            {"any16h", PredCtrl::ANY16H},
            {"all16h", PredCtrl::ALL16H},
            {"any32h", PredCtrl::ANY32H},
            {"all32h", PredCtrl::ALL32H},
        };

        const Loc prLoc = NextLoc(0);
        bool predInv = Consume(TILDE);
        ParseFlagRegRef(m_flagReg);
        PredCtrl predCtrl = PredCtrl::NONE;
        if (Consume(DOT)) {
            ConsumeIdentOneOfOrFail(
                PREDCTRLS,
                predCtrl,
                "expected predication control",
                "invalid predication control");
        } else {
            predCtrl = PredCtrl::SEQ;
        }
        m_handler.InstPredication(prLoc, predInv, m_flagReg, predCtrl);
    }


    const OpSpec *TryConsumeMmenonic() {
        const Token &tk = Next();
        if (tk.lexeme != IDENT) {
            return nullptr;
        }
        const char *p = &m_lexer.GetSource()[tk.loc.offset];
        std::string s;
        s.reserve(tk.loc.extent + 1);
        for (size_t i = 0; i < tk.loc.extent; i++) {
            s += *p++;
        }
        auto itr = opmap.find(s);
        if (itr == opmap.end()) {
            return nullptr;
        } else {
            Skip();
            return itr->second;
        }
    }

    bool LookupReg(
        const std::string &str,
        const RegInfo*& ri,
        int& reg)
    {
        ri = nullptr;
        reg = 0;

        // given something like "r13", parse the "r"
        // "cr0" -> "cr"
        size_t len = 0;
        while (len < str.length() && !isdigit(str[len]))
            len++;
        if (len == 0)
            return false;
        auto itr = regmap.find(str.substr(0,len));
        if (itr == regmap.end()) {
            return false;
        }
        ri = itr->second;
        reg = 0;
        if (ri->num_regs > 0) {
            // if it's a numbered register like "r13" or "cr1", then
            // parse the number part
            size_t off = len;
            while (off < str.size() && isdigit(str[off])) {
                char c = str[off++];
                reg = 10*reg + c - '0';
            }
            if (off < str.size()) {
                // we have something like "r13xyz"; we don't treat this
                // as a register, but fallback so it can be treated as an
                // identifier (an immediate reference)
                reg = 0;
                ri = nullptr;
                return false;
            }
            if (reg >= ri->num_regs) {
                Warning("register is out of bounds");
            }
        } // else it was something like "null" or "ip"
          // either way, we are done
        return true;
    }

    bool ConsumeReg(const RegInfo*& ri, int& reg) {
        const Token &tk = Next();
        if (tk.lexeme != IDENT) {
            return false;
        }
        if (LookupReg(GetTokenAsString(tk), ri, reg)) {
            Skip();
            return true;
        } else {
            return false;
        }
    }

    bool PeekReg(const RegInfo*& ri, int& reg) {
        const Token &tk = Next();
        if (tk.lexeme != IDENT) {
            return false;
        }
        return LookupReg(GetTokenAsString(tk), ri, reg);
    }

#if 0
    // returns the number of characters off
    size_t similarityR(
        size_t mismatches, size_t shortestStrLen,
        const std::string &left, size_t li,
        const std::string &right, size_t ri)
    {
        if (mismatches >= shortestStrLen) { // everything mismatches
            return shortestStrLen;
        } else if (li == left.size() - 1) { // the rest of right mismatches
            return mismatches + right.size() - ri;
        } else if (ri == right.size() - 1) { // the rest of left mismatches
            return mismatches + left.size() - li;
        } else {
            if (left[li] != right[ri]) {
                mismatches++;
            }
            // 1. delete both
            auto db = similarityR(
                mismatches, shortestStrLen,
                left, li + 1,
                right, ri + 1);
            // 2. delete left
            auto dr = similarityR(
                mismatches, shortestStrLen,
                left, li,
                right, ri + 1);
            // 3. delete right
            auto dl = similarityR(
                mismatches, shortestStrLen,
                left, li + 1,
                right, ri);
            return std::min<size_t>(db, std::min<size_t>(dr, dl));
        }
    }
    // roughly the number of characters
    float similarity(const std::string &left, const std::string &right) {
        if (left.size() > 8 || right.size() > 8)
            return 0;
        auto shortestLen = std::min<size_t>(left.size(), right.size());
        size_t minEdits = similarityR(0, shortestLen, left, 0, right, 0);
        return 1.0f - (float)minEdits/(float)shortestLen;
    }
#endif
    void failWithUnexpectedSubfunction(
        const Loc &loc,
        const std::string &sfIdent)
    {
        std::stringstream ss;
        ss << "unexpected subfunction for op";
        std::vector<std::pair<float,const char *>> matches;
#if 0
        for (int i = (int)m_opSpec->op + 1;
            i < (int)m_opSpec->op + m_opSpec->subopsLength;
            i++)
        {
            const OpSpec &sf = m_model.lookupOpSpec((iga::Op)i);
            if (sf.isValid()) {
                auto sim = similarity(sfIdent,sf.mnemonic);
                std::cout << sf.mnemonic << ": " << sim << "\n";
                if (sim >= 0.66f) {
                    matches.emplace_back(sim,sf.mnemonic);
                }
            }
        }
        std::sort(matches.begin(), matches.end(),
            std::greater<std::pair<float,const char *>>());
        if (!matches.empty()) {
            ss << "; did you mean ";
            if (matches.size() > 2) {
                ss << "one of: ";
            }
            // only show the top five
            size_t len = std::min<size_t>(5, matches.size());
            for (size_t i = 0; i < len; i++) {
                if (i > 0 && i == len - 1) {
                    if (len > 2) {
                        ss << ", or ";
                    } else {
                        ss << " or ";
                    }
                } else {
                    ss << ", ";
                }
                ss << matches[i].second;
            }
        }

#endif
        Fail(loc, ss.str());
    }

    // Mnemoninc    = Ident SubMnemonic? | Ident BrCtl | LdOp
    //   SubMnemoninc = '.' [Ident]SubFunc
    //   BrCtl = '.b'
    // TBD: LdOp = ('ld'|'ldc'|'lds'|'ldsc') ('.' Ident)*
    //
    void ParseMnemonic() {
        const Loc mnemonicLoc = NextLoc();
        m_opSpec = TryConsumeMmenonic();
        if (!m_opSpec) {
            Fail(mnemonicLoc, "invalid mnemonic");
        }
        if (m_opSpec->format == OpSpec::GROUP) {
            // e.g. math.*, sync.*, or send.*
            ConsumeOrFail(DOT, "expected operation subfunction");
            if (!LookingAt(IDENT)) {
                Fail("expected subfunction");
            }
            auto sfLoc = NextLoc();
            auto sfIdent = GetTokenAsString(Next());
            // look up the function by the fully qualified name
            std::stringstream ss;
            ss << m_opSpec->mnemonic << "." << sfIdent;
            auto itr = opmap.find(ss.str());
            if (itr == opmap.end()) {
                failWithUnexpectedSubfunction(sfLoc, sfIdent);
            } else {
                // resolve to idiv etc...
                Skip();
                m_opSpec = itr->second;
            }
        }
        m_handler.InstOp(m_opSpec);
        // GED will reject this
        if (!m_hasWrEn && m_opSpec->op == Op::JMPI) {
            Warning(mnemonicLoc,
                "jmpi must have (W) specified (automatically adding)");
            m_handler.InstNoMask(mnemonicLoc);
        }

        if (m_opSpec->supportsBranchCtrl()) {
            if (Consume(DOT)) {
                if (!ConsumeIdentEq("b")) {
                    Fail("expected 'b' (branch control)");
                }
                m_handler.InstBrCtl(BranchCntrl::ON);
            } else {
                m_handler.InstBrCtl(BranchCntrl::OFF);
            }
        } else if (LookingAt(DOT)) {
            // maybe an old condition modifier or saturation
            FlagModifier fm;
            if (LookingAtIdentEq(1,"sat")) {
                Fail("saturation flag goes on destination operand: "
                    "e.g. op (..) (sat)dst ...");
            } else if (
                IdentLookup(1, FLAGMODS, fm) ||
                IdentLookup(1, FLAGMODS_LEGACY, fm))
            {
                Fail("conditional modifier follows execution mask "
                    "info: e.g. op (16|M0)  (le)f0.0 ...");
            } else {
                // didn't match flag modifier
                Fail("unexpected . (expected execution size)");
            }
        }
    }


    // ExecInfo = '(' ExecSize EmOffNm? ')'
    //   where EmOffNm = '|' EmOff  (',' 'NM')?
    //                 | '|' 'NM'
    //         EmOff = 'M0' | 'M4' | ...
    //         ExecSize = '1' | '2' | ... | '32'
    void ParseExecInfo() {
        Loc execSizeLoc = NextLoc(0);
        Loc execOffsetLoc = NextLoc(0);
        // we are careful here since we might have things like:
        //    jmpi        (1*16)
        //    jmpi (1|M0) ...
        //    jmpi (1)    ...
        // We resolve that by looking ahead two symbols
        if (LookingAt(LPAREN) && (LookingAt(2,RPAREN) || LookingAt(2,PIPE))) {
            Skip();
            execSizeLoc = NextLoc();
            ConsumeIntLitOrFail(m_execSize, "expected SIMD width");

            m_chOff = 0;
            if (Consume(PIPE)) {
                static const IdentMap<int> EM_OFFS {
                      {"M0", 0}
                    , {"M4", 4}
                    , {"M8", 8}
                    , {"M12", 12}
                    , {"M16", 16}
                    , {"M20", 20}
                    , {"M24", 24}
                    , {"M28", 28}
                };
                execOffsetLoc = NextLoc();
                ConsumeIdentOneOfOrFail(
                    EM_OFFS,
                    m_chOff,
                    "expected emask offset",
                    "invalid emask offset");
                //if (m_chOff % m_execSize != 0) {
                //    Fail(execOffsetLoc,
                //        "invalid execution mask offset for execution size");
                //} else if (m_chOff + m_execSize > 32) {
                //    Fail(execOffsetLoc,
                //        "invalid execution mask offset for execution size");
                //}
            }
            ConsumeOrFail(RPAREN,"expected )");
        } else {
            if (m_opSpec->hasImpicitEm()) {
                m_chOff = 0;
                m_execSize = 1;
            } else if (m_parseOpts.supportLegacyDirectives) {
                m_chOff = 0;
                m_execSize = m_defaultExecutionSize;
            } else {
                Fail("expected '(' (start of execution size info)");
            }
        }

        ExecSize execSize;
        switch (m_execSize) {
        case 1: execSize = ExecSize::SIMD1; break;
        case 2: execSize = ExecSize::SIMD2; break;
        case 4: execSize = ExecSize::SIMD4; break;
        case 8: execSize = ExecSize::SIMD8; break;
        case 16: execSize = ExecSize::SIMD16; break;
        case 32: execSize = ExecSize::SIMD32; break;
        default: Fail("invalid SIMD width");
        }

        ChannelOffset chOff;
        switch (m_chOff) {
        case 0: chOff = ChannelOffset::M0; break;
        case 4: chOff = ChannelOffset::M4; break;
        case 8: chOff = ChannelOffset::M8; break;
        case 12: chOff = ChannelOffset::M12; break;

        case 16: chOff = ChannelOffset::M16; break;
        case 20: chOff = ChannelOffset::M20; break;
        case 24: chOff = ChannelOffset::M24; break;
        case 28: chOff = ChannelOffset::M28; break;
        default: Fail(execOffsetLoc,"invalid emask");
        }

        m_handler.InstExecInfo(
            execSizeLoc, execSize, execOffsetLoc, chOff);
    }


    // FlagModifierOpt = '(' FlagModif ')' FlagReg
    void ParseFlagModOpt() {
        Loc loc = NextLoc();
        if (Consume(LBRACK)) {
            // try the full form [(le)f0.0]
            FlagModifier flagMod;
            if (!TryParseFlagModFlag(flagMod)) {
                Fail("expected flag modifier function");
            }
            ParseFlagModFlagReg();
            ConsumeOrFail(RBRACK, "expected ]");
            if (m_parseOpts.deprecatedSyntaxWarnings) {
                Warning(loc,
                    "deprecated flag modifier "
                    "syntax (omit the brackets)");
            }
            m_handler.InstFlagModifier(m_flagReg, flagMod);
        } else {
            // try the short form
            // (le)f0.0
            FlagModifier flagMod;
            if (!TryParseFlagModFlag(flagMod)) {
                // soft fail (we might be looking at "(sat)r0.0")
                return;
            }
            ParseFlagModFlagReg();
            m_handler.InstFlagModifier(m_flagReg, flagMod);
        }
    }
    // e.g. "(le)"
    bool TryParseFlagModFlag(FlagModifier &flagMod) {
        if (!LookingAt(LPAREN)) {
            return false;
        }
        if (!IdentLookup(1, FLAGMODS, flagMod)) {
            Loc loc = NextLoc(1);
            if (!IdentLookup(1, FLAGMODS_LEGACY, flagMod)) {
                return false;
            } else if (m_parseOpts.deprecatedSyntaxWarnings) {
                // deprecated syntax
                std::stringstream ss;
                ss << "deprecated flag modifier syntax: ";
                ss << "use " << ToSyntax(flagMod) << " for this function";
                Warning(loc, ss.str().c_str());
            }
        }
        Skip(2);
        ConsumeOrFail(RPAREN, "expected )");
        return true;
    }
    // e.g. "f0.1"
    void ParseFlagModFlagReg() {
        const Loc flregLoc = NextLoc();
        RegRef fr;
        ParseFlagRegRef(fr);
        if (m_flagReg.regNum != REGREF_INVALID.regNum &&
            (m_flagReg.regNum != fr.regNum ||
                m_flagReg.subRegNum != fr.subRegNum))
        {
            Fail(flregLoc,
                "flag register must be same for predication "
                "and flag modifier");
        }
        m_flagReg = fr;
    }


    // Examples:
    //   r13.4<2>:hf
    //   (sat)r13.0<1>:f
    //   r[a0.3,16]<1>:ud
    void ParseDstOp() {
        // We want to track the beginning of the "operand", not just the register.
        // That includes the saturate flag prefix(sat) on the dst.
        // That is we need:
        // (sat)r13.3:ud
        // ^    ^
        // | | not here
        // |
        // | here
        const Loc opStart = NextLoc(0);
        // ParseSatOpt increments m_offset.
        if (ParseSatOpt()) {
            m_handler.InstDstOpSaturate();
        }
        // Note that, if there is SAT token, opStart != regStart.
        // This is because m_offset changed.
        const Loc regStart = NextLoc(0);
        if (ConsumeIdentEq("r")) {
            ParseDstOpRegInd(opStart, 0);
        } else {
            const RegInfo *ri;
            int regNum;
            if (!ConsumeReg(ri, regNum)) {
                Fail("invalid destination register");
            }
            if (!ri->isRegNumberValid(regNum)) {
                FailF("invalid destination register number "
                    "(%s only has %d registers on this platform)",
                    ri->name, ri->num_regs);
            }
            if (LookingAt(LBRACK)) {
                ParseDstOpRegInd(opStart, regNum * 32);
            } else {
                FinishDstOpRegDirSubRegRgnTy(opStart, regStart, *ri, regNum);
            }
        }
    }

    // r13
    // null
    // r13:w (non-standard)
    void ParseSendDstOp() {
        const Loc regStart = NextLoc(0);
        if (ConsumeIdentEq("r")) {
            ParseDstOpRegInd(regStart, 0);
        } else {
            const RegInfo *ri = NULL;
            int regNum = 0;
            if (!ConsumeReg(ri, regNum)) {
                Fail("invalid send destination");
            }
            if (!ri->isRegNumberValid(regNum)) {
                FailF("invalid destination register number "
                    "(%s only has %d registers on this platform)",
                    ri->name, ri->num_regs);
            }
            if (LookingAt(LBRACK)) {
                Fail("this form of indirect (r3[a0.0,16]) is invalid for send dst opnd; use regular form: r[a0.0,16]");
            }
            else
            {
                FinishDstOpRegDirSubRegRgnTy(regStart, regStart, *ri, regNum);
            }
        }
    }

    // (sat)
    bool ParseSatOpt() {
        // TODO: expand to tokens so (sat) can be imm expr
        return Consume(SAT);
    }


    ImplAcc ParseImplAcc() {
        ImplAcc implAcc = ImplAcc::NOACC;
        const char *expected =
            "expected implicit accumulator operand "
            "(e.g. .noacc, .acc2, ..., .acc9)";
        ConsumeOrFail(DOT, expected);
        ConsumeIdentOneOfOrFail<ImplAcc>(
            IMPLACCS, implAcc, expected, expected);
        return implAcc;
    }


    // REG ('.' INT)? DstRgn (':' DstTy)?
    //   where DstRgn = '<' INT '>'
    //
    // E.g. r13.4<2>:t
    void FinishDstOpRegDirSubRegRgnTy(
        const Loc &opStart,
        const Loc &regnameLoc,
        const RegInfo &ri,
        int regNum)
    {
        // subregister or implicit accumulator operand
        // e.g. .4 or .acc3
        Loc subregLoc = NextLoc(0);
        int subregNum = 0;

        // <1>
        Region::Horz rgnHz = Region::Horz::HZ_1;

        ImplAcc implAcc = ImplAcc::INVALID;
        if (m_opSpec->isSendOrSendsFamily() && Consume(DOT)) {
            ConsumeIntLitOrFail(subregNum, "expected subregister");
            // whine about them using a subregister on a send operand
            if (m_parseOpts.deprecatedSyntaxWarnings) {
                Warning(subregLoc, "send operand subregisters have no effect"
                    " and are deprecated syntax");
            }
            if (m_opSpec->isSendOrSendsFamily() && LookingAt(LANGLE)) {
                if (m_parseOpts.deprecatedSyntaxWarnings) {
                    // whine about them using a region
                    Warning("send operand region has no effect and is"
                        " deprecated syntax");
                }
            }
            rgnHz = ParseDstOpRegion();
        } else if (isMacroOp()) {
            // implicit accumulator operand
            implAcc = ParseImplAcc();
        } else {
            // non-send with subregister.
            // regular subregister
            if (Consume(DOT)) {
                ConsumeIntLitOrFail(subregNum, "expected subregister");
            } else {
                // implicit subregister
                //  E.g. r12:d  ...
                subregNum = 0;
            }
            rgnHz = ParseDstOpRegion();
        }

        // :t
        Type dty = Type::INVALID;
        if (m_opSpec->isSendOrSendsFamily()) {
            // special handling for send types
            dty = ParseSendOperandTypeWithDefault(-1);
        } else {
            dty = ParseDstOpTypeWithDefault();
        }

        // ensure the subregister is not out of bounds
        if (dty != Type::INVALID) {
            int typeSize = TypeSize(dty);
            if (ri.isSubRegByteOffsetValid(regNum, subregNum * typeSize)) {
                Warning(subregLoc, "subregister too out of bounds for data type");
            } else if (typeSize < ri.acc_gran) {
                Warning(regnameLoc, "access granularity too small for data type");
            }
        }

        if (isMacroOp()) {
            m_handler.InstDstOpRegImplAcc(opStart, ri, regNum, implAcc, dty);
        } else {
            RegRef reg = {(uint8_t)regNum, (uint8_t)subregNum};
            m_handler.InstDstOpRegDirect(opStart, ri, reg, rgnHz, dty);
        }
    }


    // e.g. [a0.4,  16]
    //  or  [a0.4 + 16]
    //  or  [a0.4 - 16]
    void ParseIndOpArgs(RegRef &addrRegRef, int &addrOff) {
        ConsumeOrFail(LBRACK, "expected [");
        if (!ParseAddrRegRefOpt(addrRegRef)) {
            Fail("expected address subregister");
        }
        Loc addrOffLoc = NextLoc();
        if (Consume(COMMA)) {
            bool neg = Consume(SUB);
            ConsumeIntLitOrFail(addrOff, "expected indirect address offset");
            if (neg)
                addrOff *= -1;
        } else if (Consume(ADD)) {
            ConsumeIntLitOrFail(addrOff, "expected indirect address offset");
        } else if (Consume(SUB)) {
            ConsumeIntLitOrFail(addrOff, "expected indirect address offset");
            addrOff *= -1;
        } else {
            addrOff = 0;
        }
        const int ADDROFF_BITS = 10; // one is a sign bit
        if (addrOff < -(1 << (ADDROFF_BITS - 1)) ||
            addrOff > (1 << (ADDROFF_BITS - 1)) - 1)
        {
            Fail(addrOffLoc, "immediate offset is out of range; "
                "must be in [-512,512]");
        }
        ConsumeOrFail(RBRACK, "expected ]");
    }


    // '[' 'a0' '.' INT (',' INT)? ']' ('<' INT '>')?)? TY?
    // e.g [a0.3,16]<1>:t
    void ParseDstOpRegInd(const Loc &opStart, int baseAddr) {
        // [a0.4,16]
        int addrOff;
        RegRef addrRegRef;
        ParseIndOpArgs(addrRegRef, addrOff);
        addrOff += baseAddr;

        // <1>
        Region::Horz rgnHz = ParseDstOpRegion();

        // :t
        Type dty = ParseDstOpTypeWithDefault();
        m_handler.InstDstOpRegIndirect(
            opStart, addrRegRef, addrOff, rgnHz, dty);
    }


    // '<' INT '>'
    Region::Horz ParseDstOpRegion() {
        if (!LookingAt(LANGLE)) {
            if (m_opSpec->hasImplicitDstRegion()) {
                return m_opSpec->implicitDstRegion().getHz();
            } else {
                return Region::Horz::HZ_1;
            }
        }

        Region::Horz rgnHz = Region::Horz::HZ_1;
        if (Consume(LANGLE)) {
            const Loc loc = NextLoc();
            int rgnHzInt;
            ConsumeIntLitOrFail(rgnHzInt, "destination region argument");
            switch (rgnHzInt) {
            case 1: rgnHz = Region::Horz::HZ_1; break;
            case 2: rgnHz = Region::Horz::HZ_2; break;
            case 4: rgnHz = Region::Horz::HZ_4; break;
            default:
                Fail(loc, "invalid destination region");
            }
            ConsumeOrFail(RANGLE,"expected >");
        }

        return rgnHz;
    }


    // E.g. 3 in "a0.3"
    bool ParseAddrRegRefOpt(RegRef& addrReg) {
        Loc loc = NextLoc();
        const RegInfo *ri;
        int regNum;
        if (!ConsumeReg(ri, regNum)) {
            return false;
        }
        if (ri->reg != RegName::ARF_A && regNum != 0) {
            Fail("expected a0");
        }
        if (!Consume(DOT)) {
            Fail("expected .");
        }
        addrReg.regNum = addrReg.subRegNum = 0;
        ConsumeIntLitOrFail(
            addrReg.subRegNum, "expected address register subregister");
        // TODO: range-check against RegInfo for "a"
        return true;
    }


    // same as ParseSrcOp, but semantically chekcs that it's a label
    void ParseSrcOpLabel(int srcOpIx) {
        ParseSrcOp(srcOpIx);
        if (m_srcKinds[srcOpIx] != Operand::Kind::LABEL &&
            m_srcKinds[srcOpIx] != Operand::Kind::IMMEDIATE)
        {
            std::stringstream ss;
            ss << "src" << srcOpIx << " must be an immediate label";
            Fail(m_srcLocs[srcOpIx], ss.str());
        }
    }


    void ParseSrcOp(int srcOpIx) {
        m_srcLocs[srcOpIx] = NextLoc();

        // TODO: sink this down to the immediate literal case specially
        // For now, we leave it here since TryParseConstExpr() will otherwise
        // fail on "-r12:f" would fail: - as unary negation then r12 fails to
        // resolve
        const Token regnameTk = Next();
        const RegInfo *regInfo;
        int regNum;

        // first try and parse as register
        m_lexer.Mark();
        bool pipeAbs = false;
        SrcModifier srcMods = ParseSrcModifierOpt(pipeAbs);
        if (ConsumeIdentEq("r")) {
            // canonical register indirect
            // r[a0.4,-32]
            m_srcKinds[srcOpIx] = Operand::Kind::INDIRECT;
            ParseSrcOpInd(srcOpIx, m_srcLocs[srcOpIx], srcMods, 0);
            // register, symbolic immediate, label, ...
        } else if (ConsumeReg(regInfo, regNum)) {
            // register direct or new pre-scaled register indirect
            // r13
            //   or
            // r13[a0.0,4] => r[a0.0, 13*32 + 4]
            if (LookingAt(LBRACK)) {
                if (!regInfo->supportsRegioning()) {
                    Fail("this doesn't support regioning");
                }
                m_srcKinds[srcOpIx] = Operand::Kind::INDIRECT;
                ParseSrcOpInd(
                    srcOpIx,
                    m_srcLocs[srcOpIx],
                    srcMods,
                    32*regNum);
            } else {
                // normal register access
                //   r13.3<0;1,0>
                //   acc3
                m_srcKinds[srcOpIx] = Operand::Kind::DIRECT;
                FinishSrcOpRegDirSubRegRgnTy(
                    srcOpIx,
                    m_srcLocs[srcOpIx],
                    regnameTk.loc,
                    srcMods,
                    *regInfo,
                    regNum);
            }
            if (pipeAbs) {
                ConsumeOrFailAfterPrev(PIPE, "expected |");
            }
        } else {
            // backtrack to before any "source modifier"
            ImmVal immVal;
            m_lexer.Reset();
            // try as constant expression
            if (TryParseConstExpr(srcOpIx, immVal)) {
                // does not match labels
                m_srcKinds[srcOpIx] = Operand::Kind::IMMEDIATE;
                FinishSrcOpImmValue(
                    srcOpIx,
                    m_srcLocs[srcOpIx],
                    regnameTk,
                    immVal);
            } else {
                // failed constant expression without consuming any input
                if (LookingAt(IDENT)) {
                    // e.g. LABEL64
                    if (m_opSpec->isBranching()) {
                        m_srcKinds[srcOpIx] = Operand::Kind::LABEL;
                        std::string str = GetTokenAsString(Next(0));
                        Skip(1);
                        FinishSrcOpImmLabel(
                            srcOpIx,
                            m_srcLocs[srcOpIx],
                            srcMods,
                            regnameTk.loc,
                            str);
                    } else {
                        // okay, we're out of ideas now
                        Fail("unbound identifier");
                    }
                } else {
                    // the token is not in the FIRST(srcop)
                    Fail("expected source operand");
                }
            }
        }
    }


    // expression parsing
    // &,|
    // <<,>>
    // +,-
    // *,/,%
    // -(unary neg)
    bool TryParseConstExpr(int srcOpIx, ImmVal &v) {
        if (ParseBitwiseExpr(false, v)) {
            if (srcOpIx > 0) {
                m_srcKinds[srcOpIx] = Operand::Kind::IMMEDIATE;
            }
            return true;
        }
        return false;
    }
    bool TryParseConstExpr(ImmVal &v) {
        return TryParseConstExpr(-1,v);
    }
    bool TryParseIntConstExpr(ImmVal &v, const char *for_what) {
        Loc loc = NextLoc();
        bool z = TryParseConstExpr(-1,v);
        if (!z) {
            return false;
        } else if (!isIntegral(v)) {
            std::stringstream ss;
            if (for_what) {
                ss << for_what << " must be a constant integer expression";
            } else {
                ss << "expected constant integer expression";
            }
            Fail(loc,ss.str());
        }
        return true;
    }
    void EnsureIntegral(const Token &t, const ImmVal &v) {
        if (!isIntegral(v)) {
            Fail(t.loc, "argument to operator must be integral");
        }
    }
    void CheckNumTypes(const ImmVal &v1, const Token &op, const ImmVal &v2) {
        if (isFloating(v1) && !isFloating(v2)) {
            Fail(op.lexeme, "right operand to operator must be floating point"
                " (append a .0 to force floating point)");
        } else if (isFloating(v2) && !isFloating(v1)) {
            Fail(op.lexeme, "left operand to operator must be floating point"
                " (append a .0 to force floating point)");
        }
    }
    // target must be float
    void CheckIntTypes(const ImmVal &v1, const Token &op, const ImmVal &v2) {
        if (isFloating(v1)) {
            Fail(op.lexeme, "left operand to operator must be integral");
        } else if (isFloating(v2)) {
            Fail(op.lexeme, "right operand to operator must be integral");
        }
    }
    ImmVal EvalBinExpr(const ImmVal &v1, const Token &op, const ImmVal &v2) {
        bool isF =isFloating(v1) || isFloating(v2);
        bool isU = isUnsignedInt(v1) || isUnsignedInt(v2);
        ImmVal result = v1;
        switch (op.lexeme) {
        case AMP:
        case PIPE:
        case LSH:
        case RSH:
        case MOD:
            // integral only operations
            CheckIntTypes(v1, op, v2);
            switch (op.lexeme) {
            case AMP:
                if (isU) {
                    result.u64 &= v2.u64;
                } else {
                    result.s64 &= v2.s64;
                }
                break;
            case PIPE:
                if (isU) {
                    result.u64 |= v2.u64;
                } else {
                    result.s64 |= v2.s64;
                }
                break;
            case LSH:
                if (isU) {
                    result.u64 <<= v2.u64;
                } else {
                    result.s64 <<= v2.s64;
                }
                break;
            case RSH:
                if (isU) {
                    result.u64 >>= v2.u64;
                } else {
                    result.s64 >>= v2.s64;
                }
                break;
            case MOD:
                if (isU) {
                    result.u64 %= v2.u64;
                } else {
                    result.s64 %= v2.s64;
                }
                break;
            default:
                break;
            }
            break;
        case ADD:
        case SUB:
        case MUL:
        case DIV:
            CheckNumTypes(v1, op, v2);
            switch (op.lexeme) {
            case ADD:
                if (isF) {
                    result.f64 += v2.f64;
                } else if (isU) {
                    result.u64 += v2.u64;
                } else {
                    result.s64 += v2.s64;
                }
                break;
            case SUB:
                if (isF) {
                    result.f64 -= v2.f64;
                } else if (isU) {
                    result.u64 -= v2.u64;
                } else {
                    result.s64 -= v2.s64;
                }
                break;
            case MUL:
                if (isF) {
                    result.f64 *= v2.f64;
                } else if (isU) {
                    result.u64 *= v2.u64;
                } else {
                    result.s64 *= v2.s64;
                }
                break;
            case DIV:
                if (isF) {
                    result.f64 /= v2.f64;
                } else {
                    if (v2.u64 == 0) {
                        Fail(op.loc, "(integral) division by zero");
                    }
                    if (isU) {
                        result.u64 /= v2.u64;
                    } else {
                        result.s64 /= v2.s64;
                    }
                }
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
        return result;
    }
    bool isFloating(const ImmVal &v) {
        switch (v.kind) {
        case ImmVal::Kind::F16:
        case ImmVal::Kind::F32:
        case ImmVal::Kind::F64:
            return true;
        default:
            return false;
        }
    }
    bool isIntegral(const ImmVal &v) {
        return isSignedInt(v) || isUnsignedInt(v);
    }
    bool isSignedInt(const ImmVal &v) {
        switch (v.kind) {
        case ImmVal::Kind::S8:
        case ImmVal::Kind::S16:
        case ImmVal::Kind::S32:
        case ImmVal::Kind::S64:
            return true;
        default:
            return false;
        }
    }
    bool isUnsignedInt(const ImmVal &v) {
        switch (v.kind) {
        case ImmVal::Kind::U8:
        case ImmVal::Kind::U16:
        case ImmVal::Kind::U32:
        case ImmVal::Kind::U64:
            return true;
        default:
            return false;
        }
    }


    // E -> E (('&'|'|') E)*
    bool ParseBitwiseExpr(bool consumed, ImmVal &v) {
        if (!ParseShiftExpr(consumed,v)) {
            return false;
        }
        while (LookingAtAnyOf(AMP, PIPE)) {
            Token t = Next(); Skip();
            ImmVal r;
            ParseBitwiseExpr(true, r);
            v = EvalBinExpr(v, t, r);
        }
        return true;
    }
    // E -> E (('<<'|'>>') E)*
    bool ParseShiftExpr(bool consumed, ImmVal &v) {
        if (!ParseAddExpr(consumed, v)) {
            return false;
        }
        while (LookingAtAnyOf(LSH, RSH)) {
            Token t = Next(); Skip();
            ImmVal r;
            ParseAddExpr(true, r);
            v = EvalBinExpr(v, t, r);
        }
        return true;
    }
    // E -> E (('+'|'-') E)*
    bool ParseAddExpr(bool consumed, ImmVal &v) {
        if (!ParseMulExpr(consumed, v)) {
            return false;
        }
        while (LookingAtAnyOf(ADD, SUB)) {
            Token t = Next(); Skip();
            ImmVal r;
            ParseMulExpr(true, r);
            v = EvalBinExpr(v, t, r);
        }
        return true;
    }

    // E -> E (('*'|'/'|'%') E)*
    bool ParseMulExpr(bool consumed, ImmVal &v) {
        if (!ParseUnExpr(consumed, v)) {
            return false;
        }
        while (LookingAtAnyOf({MUL, DIV, MOD})) {
            Token t = Next(); Skip();
            ImmVal r;
            ParseUnExpr(true, r);
            v = EvalBinExpr(v, t, r);
        }
        return true;
    }
    // E -> ('-'|'~') E
    bool ParseUnExpr(bool consumed, ImmVal &v) {
        if (!LookingAtAnyOf(SUB, TILDE)) {
            if (!ParsePrimary(consumed, v)) {
                return false;
            }
        } else {
            Token t = Next(); Skip();
            ParsePrimary(true, v);
            switch (t.lexeme) {
            case SUB:
                v.Negate();
                break;
            case TILDE:
                EnsureIntegral(t, v);
                v.u64 = ~v.u64;
                break;
            default:
                break;
            }
        }
        return true;
    }
    // special symbol (e.g. nan, inf, ...)
    // grouped expression (E)
    // literal
    bool ParsePrimary(bool consumed, ImmVal &v) {
        Token t = Next();
        bool isQuietNaN = false;
        if (LookingAtIdentEq(t, "nan")) {
            Warning("nan is deprecated, us snan(...) or qnan(...)");
            v.kind = ImmVal::F64;
            v.f64 = std::numeric_limits<double>::signaling_NaN();
            Skip();
        } else if ((isQuietNaN = LookingAtIdentEq(t, "qnan")) ||
            LookingAtIdentEq(t, "snan"))
        {
            auto nanSymLoc = NextLoc();
            Skip();
            if (Consume(LPAREN)) {
                auto payloadLoc = NextLoc();
                ImmVal payload;
                payload.u64 = 0;
                ParseBitwiseExpr(true, payload);
                if (payload.u64 >= IGA_F64_SNAN_BIT) {
                    Fail(payloadLoc, "NaN payload overflows");
                } else if (payload.u64 == 0 && isQuietNaN) {
                    Fail(payloadLoc, "NaN payload must be nonzero for qnan");
                }
                ConsumeOrFail(RPAREN, "expected )");
                v.u64 = payload.u64 | IGA_F64_EXP_MASK;
                if (!isQuietNaN) {
                    v.u64 |= IGA_F64_SNAN_BIT;
                }
            } else {
                Warning(nanSymLoc,
                    "bare qnan and snan tokens deprecated"
                    " (pass in a valid payload)");
                v.u64 = IGA_F64_EXP_MASK;
                if (isQuietNaN) {
                    v.u64 |= 1; // set something non-zero in the payload
                } else {
                    v.u64 |= IGA_F64_SNAN_BIT;
                }
            }
            v.kind = ImmVal::F64;
        } else if (LookingAtIdentEq(t, "inf")) {
            v.f64 = std::numeric_limits<double>::infinity();
            v.kind = ImmVal::F64;
            Skip();
        } else if (LookingAt(FLTLIT)) {
            ParseFltFrom(t.loc, v.f64);
            v.kind = ImmVal::Kind::F64;
            Skip();
        } else if (LookingAtAnyOf({INTLIT02,INTLIT10,INTLIT16})) {
            // we parse as unsigned, but tag as signed for negation etc...
            ParseIntFrom<uint64_t>(t.loc, v.u64);
            v.kind = ImmVal::Kind::S64;
            Skip();
        } else if (Consume(LPAREN)) {
            // (E)
            ParseBitwiseExpr(true, v);
            Consume(RPAREN);
        } else if (LookingAt(IDENT)) {
            // TEST CASES
            // LABEL:
            // // jmpi  LABEL                    // passes
            // // goto (16) (2 + LABEL) LABEL    // fails
            // // goto (16) LABEL LABEL          // passes
            // // join (16) LABEL                // passes
            // // mov (1) r65:ud LABEL:ud        // fails
            // // mov (1) r65:ud (2 + LABEL):ud  // fails (poor diagnostic)
            if (m_opSpec->isBranching()) {
                if (consumed) {
                    //   jmpi (LABEL + 2)
                    //         ^^^^^ already consumed LPAREN
                    Fail("branching operands may not perform arithmetic on labels");
                } else {
                    // e.g. jmpi  LABEL64
                    //            ^^^^^^
                    // This backs out so caller can cleanly treat this as
                    // a branch label cleanly
                    return false;
                }
            } else {
                // non branching op
                if (!consumed) {
                    // e.g. mov (1) r13:ud   SYMBOL
                    //                       ^^^^^^
                    // we fail here since we don't know if we should treat SYMBOL
                    // as relative or absolute
                    Fail("non-branching operations may not reference symbols");
                } else {
                    // end of a term where FOLLOW contains IDENT
                    //   X + Y*Z  IDENT
                    //            ^
                    // this allows caller to back off and accept
                    //  X + Y*Z as the total expression with lookahead IDENT
                    return false;
                    // FIXME: mov (1) r65:ud (LABEL + 2):ud
                    //   Bad diagnostic "expected source type"
                    // Either we cannot allow IDENT in any const expr's
                    // follow set, or we must track more state through the
                    // expression parse...
                    // Maybe pass a bool, canFail around.
                    //
                    // NOTE: we could also keep a list of backpatches and
                    // apply it after the parse.  But this would require
                    // building a full expression tree and walking it after
                    // all labels have been seen.
                }
            }
        } else {
            // something else: error unless we haven't consumed anything
            if (consumed) {
                Fail("syntax error in constant expression");
            }
            return false;
        }
        return true;
    }


    void ParseSrcOpInd(
        int srcOpIx,
        const Loc &opStart,
        const SrcModifier &srcMods,
        int baseOff)
    {
        m_srcKinds[srcOpIx] = Operand::Kind::INDIRECT;

        // [a0.4,16]
        int addrOff;
        RegRef addrRegRef;
        ParseIndOpArgs(addrRegRef, addrOff);
        addrOff += baseOff;

        // regioning... <V;W,H> or <V,H>
        Region rgn = ParseSrcOpRegionInd(srcOpIx);

        // :t
        Type sty = ParseSrcOpTypeWithDefault(srcOpIx, true);

        m_handler.InstSrcOpRegIndirect(
            srcOpIx, opStart, srcMods, addrRegRef, addrOff, rgn, sty);
    }


    // REG ('.' INT)? SrcRgn? (':' SrcTy)?
    //    ^ HERE
    //
    // E.g. r13.4<2>:t
    // E.g. r13.4<0;1,0>:t
    // E.g. r13.4<8;8,1>:t
    // E.g. ce:ud
    // E.g. ip:ud
    // E.g. a0.1
    void FinishSrcOpRegDirSubRegRgnTy(
        int srcOpIx,
        const Loc &opStart,
        const Loc &regnameLoc,
        const SrcModifier &srcMod,
        const RegInfo &ri,
        int regNum)
    {
        // subregister or implicit accumulator operand
        // e.g. .4 or .acc3
        int subregNum;
        Region rgn;
        Loc subregLoc = NextLoc(1);
        ImplAcc implAcc = ImplAcc::INVALID;
        if (isMacroOp()) {
            // implicit accumulator operand
            subregNum = 0;
            if (!Consume(DOT)) {
                ConsumeIntLitOrFail(subregNum,
                    "expected implicit accumulator operand "
                    "(e.g. .noacc, .acc2, ..., .acc9)");
            }
            const char *expected =
                "expected implicit accumulator operand "
                "(e.g. .noacc, .acc2, ..., .acc9)";
            ConsumeIdentOneOfOrFail<ImplAcc>(
                IMPLACCS, implAcc, expected, expected);
        } else {
            // regular subregister
            bool hasExplicitSubreg = false;
            if (Consume(DOT)) {
                ConsumeIntLitOrFail(subregNum, "expected subregister");
                hasExplicitSubreg = true;
            } else { // default to 0
                subregLoc = NextLoc(0);
                subregNum = 0;
            }
            // for ternary ops <V;H> or <H>
            // for other regions <V;W,H>
            if (m_opSpec->isTernary()) {
                if (srcOpIx < 2) {
                    rgn = ParseSrcOpRegionVH(srcOpIx, hasExplicitSubreg);
                } else {
                    rgn = ParseSrcOpRegionH(srcOpIx, hasExplicitSubreg);
                }
            } else {
                rgn = ParseSrcOpRegionVWH(ri, srcOpIx, hasExplicitSubreg);
            }
        }

        // :t
        Type sty = Type::INVALID;
        if (m_opSpec->isSendOrSendsFamily())
        {
            sty = ParseSendOperandTypeWithDefault(srcOpIx);
        }
        else
        {
            sty = ParseSrcOpTypeWithDefault(srcOpIx, false);
        }

        if (sty != Type::INVALID) {
            // ensure the subregister is not out of bounds
            int typeSize = TypeSize(sty);
            if (ri.isSubRegByteOffsetValid(regNum, subregNum * typeSize)) {
                Warning(subregLoc, "sub-register out of bounds for data type");
            } else if (typeSize < ri.acc_gran) {
                Warning(regnameLoc,
                    "access granularity too small for register");
            }
        }

        if (isMacroOp()) {
            m_handler.InstSrcOpRegImplAcc(
                srcOpIx,
                opStart,
                srcMod,
                ri,
                regNum,
                implAcc,
                sty);
        } else {
            RegRef reg = {
                static_cast<uint8_t>(regNum),
                static_cast<uint8_t>(subregNum)
            };
            m_handler.InstSrcOpRegDirect(
                srcOpIx,
                opStart,
                srcMod,
                ri,
                reg,
                rgn,
                sty);
        }
    }


    // '<' INT ';' INT ',' INT '>'     (VxWxH)
    Region ParseSrcOpRegionVWH(
        const RegInfo &ri, int srcOpIx, bool hasExplicitSubreg)
    {
        if (m_opSpec->hasImplicitSrcRegion(srcOpIx, m_model.platform)) {
            if (!LookingAt(LANGLE)) {
                return m_opSpec->implicitSrcRegion(srcOpIx, m_model.platform);
            } else {
                WarningF("%s.Src%d region should be implicit",
                    m_opSpec->mnemonic,
                    srcOpIx);
            }
        }

        Region rgn = Region::SRC010;
        if (Consume(LANGLE)) {
            rgn.set(ParseRegionVert());
            ConsumeOrFailAfterPrev(SEMI, "expected ;");
            rgn.set(ParseRegionWidth());
            ConsumeOrFailAfterPrev(COMMA, "expected ,");
            rgn.set(ParseRegionHorz());
            ConsumeOrFailAfterPrev(RANGLE, "expected >");
        } else if (ri.supportsRegioning()) {
            rgn = hasExplicitSubreg || m_execSize == 1 ?
                Region::SRC010 :
                Region::SRC110; 
        } else {
            rgn = Region::SRC010;
        }
        return rgn;
    }

    // '<' INT ';' INT '>'   (CNL Align1 ternary)
    Region ParseSrcOpRegionVH(int srcOpIx, bool hasExplicitSubreg)
    {
        if (m_opSpec->hasImplicitSrcRegion(srcOpIx, m_model.platform)) {
            if (!LookingAt(LANGLE)) {
                return m_opSpec->implicitSrcRegion(srcOpIx, m_model.platform);
            } else if (m_parseOpts.deprecatedSyntaxWarnings) {
                WarningF("%s.Src%d region should be implicit",
                    m_opSpec->mnemonic,
                    srcOpIx);
            }
        }
        Region rgn = Region::SRC010;
        if (Consume(LANGLE)) {
            rgn.set(ParseRegionVert());
            ConsumeOrFailAfterPrev(SEMI, "expected ;");
            rgn.set(Region::Width::WI_INVALID);
            rgn.set(ParseRegionHorz());
            ConsumeOrFailAfterPrev(RANGLE, "expected >");
        } else {
            bool scalarAccess = hasExplicitSubreg || m_execSize == 1;
            if (scalarAccess) {
                rgn = Region::SRC0X0;
            } else {
                // packed access
                rgn = Region::SRC2X1; // most conservative mux;
            }
        }
        return rgn;
    }


    // '<' INT '>'   (CNL Align1 ternary src2)
    Region ParseSrcOpRegionH(int srcOpIx, bool hasExplicitSubreg)
    {
        if (m_opSpec->hasImplicitSrcRegion(srcOpIx, m_model.platform)) {
            if (!LookingAt(LANGLE)) {
                return m_opSpec->implicitSrcRegion(srcOpIx, m_model.platform);
            } else if (m_parseOpts.deprecatedSyntaxWarnings) {
                WarningF("%s.Src%d region should be implicit",
                    m_opSpec->mnemonic,
                    srcOpIx);
            }
        }

        Region rgn;
        rgn.bits = 0; // needed to clear _padding
        if (Consume(LANGLE)) {
            rgn.set(
                Region::Vert::VT_INVALID,
                Region::Width::WI_INVALID,
                ParseRegionHorz());
            ConsumeOrFailAfterPrev(RANGLE, "expected >");
        } else {
            rgn = hasExplicitSubreg || m_execSize == 1 ?
                Region::SRCXX0 :
                Region::SRCXX1;
        }
        return rgn;
    }


    // '<' INT ',' INT '>'             (VxH mode)
    // '<' INT ';' INT ',' INT '>'
    Region ParseSrcOpRegionInd(int srcOpIx) {
        if (m_opSpec->hasImplicitSrcRegion(srcOpIx, m_model.platform)) {
            if (!LookingAt(LANGLE)) {
                return m_opSpec->implicitSrcRegion(srcOpIx, m_model.platform);
            } else if (m_parseOpts.deprecatedSyntaxWarnings) {
                WarningF("%s.Src%d region should be implicit",
                    m_opSpec->mnemonic,
                    srcOpIx);
            }
        }

        Region rgn;
        rgn.bits = 0;
        if (Consume(LANGLE)) {
            Loc arg1Loc = NextLoc();
            int arg1;
            ConsumeIntLitOrFail(arg1, "syntax error in source region");
            if (Consume(COMMA)) {
                // <W,H>
                rgn.set(Region::Vert::VT_VxH);
                switch(arg1) {
                case  1:
                case  2:
                case  4:
                case  8:
                case 16:
                    rgn.w = arg1;
                    break;
                default:
                    Fail(arg1Loc, "invalid region width");
                    rgn.set(Region::Width::WI_INVALID);
                }
                rgn.set(ParseRegionHorz());
            } else {
                // <V;W,H>
                ConsumeOrFailAfterPrev(SEMI, "expected ;");
                switch(arg1) {
                case  0:
                case  1:
                case  2:
                case  4:
                case  8:
                case 16:
                case 32:
                    rgn.v = arg1;
                    break;
                default:
                    Fail(arg1Loc, "invalid region vertical stride");
                    rgn.set(Region::Vert::VT_INVALID);
                }
                rgn.set(ParseRegionWidth());
                ConsumeOrFailAfterPrev(COMMA, "expected ,");
                rgn.set(ParseRegionHorz());
            }
            ConsumeOrFailAfterPrev(RANGLE, "expected >");
        } else {
            rgn = Region::SRC110;
        }
        return rgn;
    }


    Region::Vert ParseRegionVert() {
        Loc loc = NextLoc();
        int x;
        ConsumeIntLitOrFail(x, "syntax error in region (vertical stride)");
        Region::Vert vs;
        switch(x) {
        case  0: vs = Region::Vert::VT_0; break;
        case  1: vs = Region::Vert::VT_1; break;
        case  2: vs = Region::Vert::VT_2; break;
        case  4: vs = Region::Vert::VT_4; break;
        case  8: vs = Region::Vert::VT_8; break;
        case 16: vs = Region::Vert::VT_16; break;
        case 32: vs = Region::Vert::VT_32; break;
        default:
            Fail(loc, "invalid region vertical stride");
            vs = Region::Vert::VT_INVALID;
        }
        return vs;
    }


    Region::Width ParseRegionWidth() {
        Loc loc = NextLoc();
        int x;
        ConsumeIntLitOrFail(x, "syntax error in region (width)");
        Region::Width wi;
        switch(x) {
        case  1: wi = Region::Width::WI_1; break;
        case  2: wi = Region::Width::WI_2; break;
        case  4: wi = Region::Width::WI_4; break;
        case  8: wi = Region::Width::WI_8; break;
        case 16: wi = Region::Width::WI_16; break;
        default:
            Fail(loc, "invalid region width");
            wi = Region::Width::WI_INVALID;
        }
        return wi;
    }


    Region::Horz ParseRegionHorz() {
        Loc loc = NextLoc();
        int x;
        ConsumeIntLitOrFail(x, "syntax error in region (horizontal stride)");
        Region::Horz hz;
        switch(x) {
        case  0: hz = Region::Horz::HZ_0; break;
        case  1: hz = Region::Horz::HZ_1; break;
        case  2: hz = Region::Horz::HZ_2; break;
        case  4: hz = Region::Horz::HZ_4; break;
        default:
            Fail(loc,"invalid region horizontal stride");
            hz = Region::Horz::HZ_INVALID;
        }
        return hz;
    }


    void CheckLiteralBounds(
        const Loc &opStart,
        Type type,
        const ImmVal &val,
        int64_t mn,
        int64_t mx)
    {
        if (val.s64 < mn || val.s64 > mx) {
            WarningF(opStart,
                "literal is out of bounds for type %s",
                ToSyntax(type).c_str());
        }
    }


    void FinishSrcOpImmValue(
        int srcOpIx,
        const Loc &opStart,
        const Token &valToken,
        ImmVal &val)
    {
        // convert to the underlying data type
        Type sty = ParseSrcOpTypeWithoutDefault(srcOpIx, true);
        switch (sty) {
        case Type::B:
        case Type::UB:
        case Type::W:
        case Type::UW:
        case Type::D:
        case Type::UD:
        case Type::Q:
        case Type::UQ:
        case Type::V:
        case Type::UV:
        case Type::VF:
            if (val.kind != ImmVal::S64) {
                Error(opStart,
                    "literal must be integral for type ", ToSyntax(sty));
            }
            break;
        case Type::HF:
            if (val.kind == ImmVal::S64) { // The value was parsed as integral
                if (valToken.lexeme == INTLIT10 && val.u64 != 0) {
                    // base10
                    // examples: 2:f or (1+2):f
                    Fail(opStart,
                        "immediate integer floating point literals must be "
                        "in hex or binary (e.g. 0x7F800000:f)");
                }
                // base2 or base16
                // e.g. 0x1234:hf  (preserve it)
                if (~0xFFFFull & val.u64) {
                    Error(opStart, "hex literal too big for type");
                }
                // no copy needed, the half float is already encoded as such
            } else { // ==ImmVal::F64
                     // it's an fp64 literal, we need to narrow to fp16
                     //   e.g. "(1.0/10.0):hf"
                uint64_t DROPPED_PAYLOAD = ~((uint64_t)IGA_F16_MANT_MASK) &
                    (IGA_F64_MANT_MASK >> 1);
                if (IS_NAN(val.f64) && (val.u64 & DROPPED_PAYLOAD)) {
                    Fail(opStart, "NaN payload value overflows");
                }
                // uint64_t orginalValue = val.u64;
                val.u64 = ConvertDoubleToHalf(val.f64); // copy over u64 to clobber all
                val.kind = ImmVal::F16;
                // uint64_t newValue = FloatToBits(
                //    ConvertFloatToDouble(ConvertHalfToFloat(val.u16)));
                // if (orginalValue != newValue) {
                //    Warning(opStart,
                //        "precision lost in literal conversion to fp16");
                // }
            }
            break;
        case Type::F:
        case Type::DF:
            if (val.kind == ImmVal::S64) {
                if (valToken.lexeme == INTLIT10 && val.u64 != 0) {
                    // base10
                    // examples: 2:f or (1+2):f
                    Fail(opStart,
                        "immediate integer floating point literals must be "
                        "in hex or binary (e.g. 0x7F800000:f)");
                }
                // base2 or base16 float bits
                // examples:
                //   0x3F000000:f (0.5)
                //   0x7F801010:f (qnan(0x01010))
                //   0xFFC00000:f (-qnan(...))
                //   0x7FC00000:f (qnan())
                //   0x7F800000:f (infinity)
                if (sty == Type::F) {
                    // preserve the bits
                    val.u32 = (uint32_t)val.s64;
                    val.kind = ImmVal::F32;
                } else {
                    // leave :df alone, bits are already set
                    val.kind = ImmVal::F64;
                }
            } else { // ==ImmVal::F64
                if (sty == Type::F) {
                    // parsed as double e.g. "3.1414:f"
                    // need to narrow to fp32
                    // any NaN payload must fit in the smaller mantissa
                    // The bits we will remove
                    //   ~((uint64_t)IGA_F32_MANT_MASK) &
                    //      (IGA_F64_MANT_MASK >> 1)
                    // the mantissa bits that will get truncated
                    uint64_t DROPPED_PAYLOAD = ~((uint64_t)IGA_F32_MANT_MASK) &
                        (IGA_F64_MANT_MASK >> 1);
                    if (IS_NAN(val.f64) && (val.u64 & DROPPED_PAYLOAD)) {
                        Fail(opStart, "NaN payload value overflows");
                    }
                    // uint64_t originalW64 = val.u64;
                    float newF32 = ConvertDoubleToFloat(val.f64);
                    // uint64_t newW64 = FloatToBits(ConvertFloatToDouble(newF32));
                    // if (newW64 != originalW64) {
                    //    std::stringstream ss;
                    //    ss << "precision lost in literal conversion to fp32";
                    //    ss << ": value narrows to " << newF32;
                    //    Warning(opStart, ss.str().c_str());
                    // }
                    val = newF32; // ImmVal::(==) clears high bits of val
                    val.kind = ImmVal::F32;
                } // else: sty == Type::DF (nothing needed)
            }
            break;
        default:
            break;
        }

        // check literal bounds of integral values
        // literals may be signed floating in general
        switch (sty) {
        case Type::B:
            // if (val.kind == ImmVal::S64 &&
            //     ((val.u64 & 0xFFFFFFFFFFFFFF00ull) == 0xFFFFFFFFFFFFFF00ull))
            // {
            //     // negative value, but not too negative for 16 bits
            //     val.u64 &= 0xFFull;
            // }
            CheckLiteralBounds(opStart, sty, val, -128, 127);
            val.kind = ImmVal::S8;
            val.s64 = val.s8; // sign extend to a 64-bit value
            break;
        case Type::UB:
            // CheckLiteralBounds(opStart, sty, val, 0, 0xFF);
            val.kind = ImmVal::U8;
            val.u64 = val.u8; // could &= by 0xFF
            break;
        case Type::W:
            // if (val.kind == ImmVal::S64 &&
            //     ((val.u64 & 0xFFFFFFFFFFFF0000ull) == 0xFFFFFFFFFFFF0000ull))
            // {
            //     // negative value, but not too negative for 16 bits
            //    val.u64 &= 0xFFFFull;
            // }
            val.s64 = val.s16; // sign extend to a 64-bit value
            CheckLiteralBounds(opStart, sty, val, -32768, 32767);
            val.kind = ImmVal::S16;
            break;
        case Type::UW:
            // fails ~1:ub
            // CheckLiteralBounds(opStart, sty, val, 0, 0xFFFF);
            val.kind = ImmVal::U16;
            val.u64 = val.u16; // truncate to 16-bit: // could &= by 0xFFFF
            break;
        case Type::D:
            val.s64 = val.s32; // sign extend to a 64-bit value
            CheckLiteralBounds(opStart, sty, val, -2147483648ll, 2147483647ll);
            val.kind = ImmVal::S32;
            break;
        case Type::UD:
            // CheckLiteralBounds(opStart, sty, val, 0, 0xFFFFFFFF);
            // only check if input is signed??? Or reject signed input
            // if (val.kind == ImmVal::S64 &&
            //    ((val.u64 & 0xFFFFFFFF00000000ull) == 0xFFFFFFFF00000000ull))
            // {
            //     // negative value, but we can reprsent it as u32
            // }
            // val.u64 &= 0xFFFFFFFF;
            val.u64 = val.u32; // truncate top bits
            val.kind = ImmVal::U32;
            break;
        case Type::Q:
            // no conversion needed
            val.kind = ImmVal::S64;
            CheckLiteralBounds(opStart, sty, val,
                0x8000000000000000ll, 0x7FFFFFFFFFFFFFFFll);
            break;
        case Type::UQ:
            // no conversion needed
            val.kind = ImmVal::U64;
            break;
        case Type::HF:
            val.kind = ImmVal::F16;
            break;
        case Type::F:
            val.kind = ImmVal::F32;
            break;
        case Type::DF:
            val.kind = ImmVal::F64;
            break;
        case Type::UV:
        case Type::V:
        case Type::VF:
            val.kind = ImmVal::U32;
            break;
        default:
            break;
        }

        if (m_opSpec->isBranching()) {
            if (m_opSpec->op == Op::CALLA) {
                m_handler.InstSrcOpImmLabelAbsolute(
                    srcOpIx,
                    opStart,
                    val.s64,
                    sty);
            } else {
                m_handler.InstSrcOpImmLabelRelative(
                    srcOpIx,
                    opStart,
                    val.s64,
                    sty);
            }
        } else {
            m_handler.InstSrcOpImmValue(srcOpIx, opStart, val, sty);
        }
    }


    void FinishSrcOpImmLabel(
        int srcOpIx,
        const Loc &opStart,
        const SrcModifier &srcMod,
        const Loc valLoc,
        const std::string &lbl)
    {
        Type type = ParseSrcOpTypeWithDefault(srcOpIx, true);
        m_handler.InstSrcOpImmLabel(srcOpIx, opStart, lbl, type);
    }


    // = '-' | '~' | '(abs)' | '-(abs)' | '~(abs)'
    // = or start of "|r3|" like
    SrcModifier ParseSrcModifierOpt(bool &pipeAbs) {
        if (!m_opSpec->supportsSourceModifiers()) {
            if (LookingAt(SUB) &&
                LookingAtAnyOf(1, {INTLIT02, INTLIT10, INTLIT16}))
            {
                // e.g. jmpi (...) -16:d
                //                 ^ we will convert the literal
                Skip();
                return SrcModifier::NEG;
            } else if (LookingAtAnyOf(TILDE, ABS)) {
                Fail("source modifier unsupported on this op");
            }
            return SrcModifier::NONE;
        }
        SrcModifier srcMod = SrcModifier::NONE;
        if (Consume(SUB) || Consume(TILDE)) { // same lexeme as -
            srcMod = SrcModifier::NEG;
        }
        pipeAbs = LookingAt(PIPE);
        if (Consume(ABS) || Consume(PIPE)) {
            srcMod = srcMod == SrcModifier::NEG ?
                SrcModifier::NEG_ABS : SrcModifier::ABS;
        }
        return srcMod;
    }


    // e.g. "r13" or "r13:f"
    void ParseSendSrcOp(int srcOpIx, bool enableImplicitOperand) {
        m_srcLocs[srcOpIx] = NextLoc();

        const RegInfo *regInfo;
        int regNum;
        if (enableImplicitOperand) {
            bool isSuccess = PeekReg(regInfo, regNum);
            if (!isSuccess || regInfo->reg == RegName::ARF_A)
            {
                RegInfo regInfoT;
                regInfoT.name = "null";
                regInfoT.reg = RegName::ARF_NULL;
                RegRef reg = { 0, 0 };
                Region rgn = Region::SRC010;
                Type sty = Type::INVALID;
                m_handler.InstSrcOpRegDirect(
                    srcOpIx,
                    m_srcLocs[srcOpIx],
                    SrcModifier::NONE,
                    regInfoT,
                    reg,
                    rgn,
                    sty);
                return;
            }
        }

#ifndef DISABLE_SENDx_IND_SRC_OPND
        ParseSrcOp(srcOpIx);
#endif

#ifdef DISABLE_SENDx_IND_SRC_OPND
        if (!ConsumeReg(regInfo, regNum)) {
            Fail("expected send operand");
        }
        auto dotLoc = NextLoc();
        if (Consume(DOT)) {
            int i;
            ConsumeIntLitOrFail(i, "expected subregister");
            if (m_parseOpts.deprecatedSyntaxWarnings) {
                Warning(dotLoc, "send instructions may not have subregisters");
            }
        }
        RegRef reg = {
            static_cast<uint8_t>(regNum),
            0
        };
        // gets the implicit region and warns against using explicit regioning
        Region rgn = ParseSrcOpRegionVWH(*regInfo, srcOpIx, false);
        // because we are trying to phase out source operands on send
        // instructions we handle them explicitly here
        Type sty = ParseSendOperandTypeWithDefault(srcOpIx);
        m_handler.InstSrcOpRegDirect(
            srcOpIx,
            m_srcLocs[srcOpIx],
            SrcModifier::NONE,
            *regInfo,
            reg,
            rgn,
            sty);
#endif
    }


    Type ParseDstOpTypeWithDefault() {
        Loc loc = NextLoc();
        bool hasImplicitType = m_opSpec->hasImplicitDstType();
        if (hasImplicitType) {
            if (!LookingAt(COLON)) {
                return m_opSpec->implicitDstType();
            }
        }
        Type t = ParseOpTypeWithDefault(DST_TYPES, "expected destination type");
        if (hasImplicitType && m_parseOpts.deprecatedSyntaxWarnings) {
            Warning(loc, "implicit type on dst should be omitted");
        }
        return t;
    }
    Type ParseSrcOpTypeWithDefault(int srcOpIx, bool lbl) {
        if (m_opSpec->hasImplicitSrcType(srcOpIx, lbl, m_model.platform)) {
            if (!LookingAt(COLON)) {
                return m_opSpec->implicitSrcType(srcOpIx, lbl, m_model.platform);
            } else if (m_parseOpts.deprecatedSyntaxWarnings) {
                WarningF("implicit type on src should be omitted", srcOpIx);
            }
        }
        return ParseOpTypeWithDefault(SRC_TYPES, "expected source type");
    }
    Type ParseSrcOpTypeWithoutDefault(int srcOpIx, bool lbl) {
        if (m_opSpec->hasImplicitSrcType(srcOpIx, lbl, m_model.platform)) {
            if (!LookingAt(COLON)) {
                return m_opSpec->implicitSrcType(srcOpIx, lbl, m_model.platform);
            } else if (m_parseOpts.deprecatedSyntaxWarnings) {
                WarningF("implicit type on src should be omitted", srcOpIx);
            }
        }
        Type t = TryParseOpType(SRC_TYPES);
        if (t == Type::INVALID &&
            !(m_opSpec->isBranching() && m_model.platformCheck6())) {
            Fail("expected source type");
        }
        return t;
    }
    Type ParseOpTypeWithDefault(
        const IdentMap<Type> types, const char *expected_err)
    {
        auto t = TryParseOpType(types);
        if (t == Type::INVALID) {
            if (m_defaultRegisterType != Type::INVALID) {
                t = m_defaultRegisterType;
            } else if (m_opSpec->isSendOrSendsFamily()) {
                t = Type::UD;
            } else if (m_opSpec->isBranching() && m_model.platformCheck6()) {
                // no more types for branching
                t = Type::UD;
            }
            else {
                Fail(expected_err);
            }
        }
        return t;
    }
    Type TryParseOpType(const IdentMap<Type> types) {
        Loc loc = NextLoc();
        if (!LookingAt(COLON)) {
            return Type::INVALID;
        }
        Type type = Type::INVALID;
        if (IdentLookup(1, types, type)) {
            Skip(2);
        }
        return type;
    }
    Type ParseSendOperandTypeWithDefault(int srcIx) {
        // sends's second parameter doesn't have a valid type
        auto t = srcIx == 1 ? Type::INVALID : Type::UD;
        if (Consume(COLON)) {
            if (!LookingAt(IDENT)) {
                Fail("expected a send operand type");
            }
            if (!IdentLookup(0, DST_TYPES, t)) {
                Fail("unexpected operand type for send");
            }
            Skip();
        }
        return t;
    }


    // (INTEXPR|AddrRegRef) (INTEXPR|AddrRegRef)
    void ParseSendDescs() {
        const Loc exDescLoc = NextLoc();
        SendDescArg exDesc;
        if (ParseAddrRegRefOpt(exDesc.reg)) {
            exDesc.type = SendDescArg::REG32A;
        } else {
            ImmVal v;
            // constant integral expression
            // start recursion one level under TryParseConstExpr so
            // that we can skip the operand "src-index" initialization
            if (!ParseBitwiseExpr(false, v)) {
                Fail("expected extended send descriptor");
            }
            if (v.kind != ImmVal::S64 && v.kind != ImmVal::U64) {
                Fail(exDescLoc,
                    "immediate descriptor expression must be integral");
            }
            exDesc.imm = (uint32_t)v.s64;
            exDesc.type = SendDescArg::IMM;
        }

        if (LookingAt(COLON))
        {
            Fail(NextLoc(),
                "Extended Message Descriptor is typeless");
        }

        const Loc descLoc = NextLoc();
        SendDescArg desc;
        if (ParseAddrRegRefOpt(desc.reg)) {
            desc.type = SendDescArg::REG32A;
        } else {
            // constant integral expression
            ImmVal v;
            if (!ParseBitwiseExpr(false, v)) {
                Fail("expected extended send descriptor");
            }
            if (v.kind != ImmVal::S64 && v.kind != ImmVal::U64) {
                Fail(descLoc,
                    "immediate descriptor expression must be integral");
            }
            desc.imm = (uint32_t)v.s64;
            desc.type = SendDescArg::IMM;
        }

        m_handler.InstSendDescs(exDescLoc, exDesc, descLoc, desc);

        if (LookingAt(COLON))
        {
            Fail(NextLoc(),
                "Message Descriptor is typeless");
        }
    }

    // ('{' IDENT (',' IDENT)* '}')?
    void ParseInstOpts() {
        InstOptSet instOpts;
        instOpts.clear();

        if (Consume(LBRACE)) {
            if (LookingAtAnyOf({IDENT, DOLLAR, AT}))
                ParseInstOpt(instOpts);
            while (Consume(COMMA)) {
                ParseInstOpt(instOpts);
            }
            ConsumeOrFail(RBRACE,"expected }");
        }

        m_handler.InstOpts(instOpts);
    }

    void ParseInstOpt(InstOptSet &instOpts) {
        auto loc = NextLoc();
            if (ConsumeIdentEq("AccWrEn")) {
                if (!instOpts.add(InstOpt::ACCWREN)) {
                    Fail(loc, "duplicate instruction option");
                }
            } else if (ConsumeIdentEq("Atomic")) {
                if (m_model.platform < Platform::GEN7) {
                    Fail(loc, "Atomic mot supported on given platform");
                }
                if (!instOpts.add(InstOpt::ATOMIC)) {
                    Fail(loc, "duplicate instruction option");
                } else if (instOpts.contains(InstOpt::SWITCH)) {
                    Fail(loc, "Atomic mutually exclusive with Switch");
                } else if (instOpts.contains(InstOpt::NOPREEMPT)) {
                    Fail(loc, "Atomic mutually exclusive with NoPreempt");
                }
            } else if (ConsumeIdentEq("Breakpoint")) {
                if (!instOpts.add(InstOpt::BREAKPOINT)) {
                    Fail(loc, "duplicate Breakpoint");
                }
            } else if (ConsumeIdentEq("Compacted")) {
                if (instOpts.contains(InstOpt::NOCOMPACT)) {
                    Fail(loc, "Compacted mutually exclusive with "
                        "Uncompacted/NoCompact");
                }
                if (!instOpts.add(InstOpt::COMPACTED)) {
                    Fail(loc, "duplicate Compacted");
                }
            } else if (ConsumeIdentEq("EOT")) {
                if (!instOpts.add(InstOpt::EOT)) {
                    Fail(loc, "duplicate instruction option");
                } else if (!m_opSpec->isSendOrSendsFamily()) {
                    Fail(loc, "EOT is only allowed on send instructions");
                }
            } else if (ConsumeIdentEq("NoCompact") ||
                ConsumeIdentEq("Uncompacted"))
            {
                if (instOpts.contains(InstOpt::COMPACTED)) {
                    Fail(loc, "Uncomapcted/NoCompact mutually exclusive "
                        "with Compacted");
                }
                if (!instOpts.add(InstOpt::NOCOMPACT)) {
                    Fail(loc, "duplicate Uncomapcted/NoCompact");
                }
            } else if (ConsumeIdentEq("NoDDChk")) {
                if (!instOpts.add(InstOpt::NODDCHK)) {
                    Fail(loc, "duplicate instruction option");
                }
                if (m_model.supportsNoDD()) {
                    Fail(loc, "NoDDChk not supported on given platform");
                }
            } else if (ConsumeIdentEq("NoDDClr")) {
                if (!instOpts.add(InstOpt::NODDCLR)) {
                    Fail(loc, "duplicate instruction option");
                }
                if (m_model.supportsNoDD()) {
                    Fail(loc, "NoDDClr not supported on given platform");
                }
            } else if (ConsumeIdentEq("NoPreempt")) {
                if (m_model.supportsNoPreempt()) {
                    if (!instOpts.add(InstOpt::NOPREEMPT)) {
                        Fail(loc, "duplicate instruction option");
                    }
                } else {
                    Fail(loc, "NoPreempt not supported on given platform");
                }
            } else if (ConsumeIdentEq("NoSrcDepSet")) {
                if (m_model.supportNoSrcDepSet()) {
                    if (!instOpts.add(InstOpt::NOSRCDEPSET)) {
                        Fail(loc, "duplicate instruction option");
                    }
                } else {
                    Fail(loc, "NoSrcDep not supported on given platform");
                }
            } else if (ConsumeIdentEq("Switch")) {
                if (!instOpts.add(InstOpt::SWITCH)) {
                    Fail(loc, "duplicate instruction option");
                }
            } else if (ConsumeIdentEq("NoMask")) {
                Fail(loc, "NoMask goes near predication as (W) (for WrEn): "
                    "e.g. (W) op (..) ... OR (W&f0.0) op (..) ..");
            } else if (ConsumeIdentEq("H1")) {
                Fail(loc, "H1 is obsolete; use M0 in execution offset: "
                    "e.g. op (16|M0) ...");
            } else if (ConsumeIdentEq("H2")) {
                Fail(loc, "H2 is obsolete; use M16 in execution offset: "
                    "e.g. op (16|M16) ...");
            } else if (ConsumeIdentEq("Q1")) {
                Fail(loc, "Q1 is obsolete; use M0 in execution offset: "
                    "e.g. op (8|M0) ...");
            } else if (ConsumeIdentEq("Q2")) {
                Fail(loc, "Q2 is obsolete; use M8 in execution offset: "
                    "e.g. op (8|M8) ...");
            } else if (ConsumeIdentEq("Q3")) {
                Fail(loc, "Q3 is obsolete; use M16 in execution offset: "
                    "e.g. op (8|M16) ...");
            } else if (ConsumeIdentEq("Q4")) {
                Fail(loc, "Q4 is obsolete; use M24 in execution offset: "
                    "e.g. op (8|M24) ...");
            } else if (ConsumeIdentEq("N1")) {
                Fail(loc, "N1 is obsolete; use M0 in execution offset: "
                    "e.g. op (4|M0) ...");
            } else if (ConsumeIdentEq("N2")) {
                Fail(loc, "N2 is obsolete; use M4 in execution offset: "
                    "e.g. op (4|M4) ...");
            } else if (ConsumeIdentEq("N3")) {
                Fail(loc, "N3 is obsolete; use M8 in execution offset: "
                    "e.g. op (4|M8) ...");
            } else if (ConsumeIdentEq("N4")) {
                Fail(loc, "N4 is obsolete; use M12 in execution offset: "
                    "e.g. op (4|M12) ...");
            } else if (ConsumeIdentEq("N5")) {
                Fail(loc, "N5 is obsolete; use M16 in execution offset: "
                    "e.g. op (4|M16) ...");
            } else if (ConsumeIdentEq("N6")) {
                Fail(loc, "N6 is obsolete; use M20 in execution offset: "
                    "e.g. op (4|M20) ...");
            } else if (ConsumeIdentEq("N7")) {
                Fail(loc, "N7 is obsolete; use M24 in execution offset: "
                    "e.g. op (4|M24) ...");
            } else if (ConsumeIdentEq("N8")) {
                Fail(loc, "N8 is obsolete; use M28 in execution offset: "
                    "e.g. op (4|M28) ...");
            } else {
                Fail(loc, "invalid instruction option");
            }
    }


    // FlagRegRef = ('f0'|'f1') ('.' ('0'|'1'))?
    void ParseFlagRegRef(RegRef &freg) {
        // TODO: use RegInfo for "f"
        if (!LookingAt(IDENT)) {
            Fail("expected flag register");
        }
        if (ConsumeIdentEq("f0")) {
            freg.regNum = 0;
        } else if (ConsumeIdentEq("f1")) {
            freg.regNum = 1;
        } else {
            Fail("expected flag register");
        }
        if(LookingAtSeq(DOT,INTLIT10)) { // e.g. .1
                                         // protects predication's use in short predication code
                                         // (f0.any2h)
            Skip();
            ConsumeIntLitOrFail(freg.subRegNum, "expected flag subregister");
        } else {
            freg.subRegNum = 0;
        }
    }

    bool ConsumeDstType(Type &dty) {
        return ConsumeIdentOneOf(DST_TYPES, dty);
    }


    bool ConsumeSrcType(Type &sty) {
        return ConsumeIdentOneOf(SRC_TYPES, sty);
    }


    // See LookingAtLabelDef for definition of a label
    bool ConsumeLabelDef(std::string &label) {
        if (LookingAtLabelDef()) {
            label = GetTokenAsString(Next(0));
            (void)Skip(2);
            return true;
        }
        return false;
    }


    // Label = IDENT ':' (not followed by ident
    //   mov:ud (16) -- not a label
    //
    //   mov:       -- label
    //      ud (16) ...
    //   mov:       -- label
    //     (f0) ud (16) ...
    //
    //   lbl1:      -- label
    //   lbl2:
    //
    // For now if we get IDENT COLON IDENT, we resolve by:
    //  * if on same line, then it's an instruction
    //  * otherwise it's a label
    bool LookingAtLabelDef() {
        if (LookingAtSeq(IDENT,COLON)) {
            // ensure not a uniform type
            //   mov:ud (..)
            const Token &t2 = Next(2);
            if (t2.lexeme != IDENT || Next(0).loc.line != t2.loc.line) {
                return true;
            }
        }
        return false;
    }

}; // class KernelParser


Kernel *iga::ParseGenKernel(
    const Model &m,
    const char *inp,
    iga::ErrorHandler &e,
    const ParseOpts &popts)
{
    Kernel *k = new Kernel(m);
    InstBuilder h(k, e);
    KernelParser p(m, h, inp, e, popts);
    try {
        p.ParseListing();
    } catch (SyntaxError) {
        // no need to handle it (error handler has already recorded the errors)
        delete k;
        return nullptr;
    }

    return k;
}
