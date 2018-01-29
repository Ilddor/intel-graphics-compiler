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

#include "Backend/Native/InstEncoder.hpp"
#include "Backend/Native/Interface.hpp"
#include "ColoredIO.hpp"
#include "Frontend/Formatter.hpp"
#include "InstDiff.hpp"
#include "strings.hpp"
#include "bits.hpp"

#include <algorithm>
#include <bitset>
#include <iostream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>


using namespace iga;

static std::string disassembleInst(
    Platform platform,
    bool useNativeDencoder,
    size_t fromPc,
    const void *bits)
{
    ErrorHandler eh;
    std::stringstream ss;
    FormatOpts fopts(platform);
    fopts.numericLabels = true;
    // fopts.hexFloats = opts.printHexFloats;
    fopts.hexFloats = false;
    FormatInstruction(eh, ss, fopts, fromPc, bits);

    return ss.str();
}
static std::string fmtPc(const MInst *mi, size_t pc)
{
    std::stringstream ssPc;
    ssPc << "[";
    if (mi) {
        ssPc << "0x" << std::setfill('0') << std::setw(3) <<
            std::uppercase << std::hex << pc;
    } else {
        ssPc << std::setw(3) << "EOF";
    }
    ssPc << "] ";
    return ssPc.str();
}
static std::string fmtHexValue(uint64_t val)
{
    std::stringstream ss;
    ss << "0x" << std::hex << val;
    return ss.str();
}
static std::string fmtBitRange(size_t off, size_t len)
{
    std::stringstream ssOff;  // [...:..]
    if (len != 1) {
        ssOff << "[" << (off + len - 1) << ":" << off << "]";
    } else {
        ssOff << "[" << off << "]";
    }
    return ssOff.str();
}
static std::string fmtBitRange(const Field &f)
{
    return fmtBitRange(f.offset, f.length);
}
static std::string fmtSize(const Field &f)
{
    std::stringstream ssSize;
    ssSize << "(" << f.length << ")";
    return ssSize.str();
}

static const char *UNMAPPED_FIELD_NAME = "<UNMAPPED>";
static const size_t FIELD_PC_WIDTH = 7; // [0x200]
static const size_t FIELD_NAME_WIDTH = 20; // Src.Type
static const size_t FIELD_OFFSET_WIDTH = 10; // [34:32]
static const size_t FIELD_SIZE_WIDTH = 5; // (32)
static const size_t FIELD_VALUE_INT_WIDTH = 14; // 0x1232
static const size_t FIELD_VALUE_STR_WIDTH = 20; // :d

static FieldList decodeFieldsWithWarnings(
    const Model &model,
    std::ostream &os,
    Loc loc,
    const MInst *mi,
    bool &success,
    const char *leftOrRight = nullptr)
{
    FieldList fields;
    ErrorHandler errHandler;
    native::DecodeFields(loc, model, (const void *)mi, fields, errHandler);
    for (auto &e : errHandler.getErrors()) {
        std::stringstream ss;
        ss << "PC" << e.at.offset << ". " << e.message << "\n";
        emitYellowText(os, ss.str());
    }
    auto findFieldsContaining =
        [&] (size_t bit_ix)
    {
        std::vector<Field*> matching;
        for (auto &fi : fields) {
            if (bit_ix >= (size_t)fi.first.offset &&
                bit_ix < (size_t)fi.first.offset + (size_t)fi.first.length)
            {
                matching.push_back(&fi.first);
            }
        }
        return matching;
    };

    std::stringstream warningStream;
    size_t iLen = mi->isCompact() ? 8 : 16;
    // check for field overlap
    for (size_t bitIx = 0; bitIx < 8*iLen;) {
        // find all the fields that overlap at this bit index
        auto matching = findFieldsContaining(bitIx);
        // no fields map here or multiple matching, issue missing field warning
        if (matching.size() == 1) {
            // exactly one field maps this index, step forward
            bitIx++;
        } else {
            // find the end of this missing/overlapped segment
            size_t endIx = bitIx + 1;
            for (; endIx < 8*iLen; endIx++) {
                auto m = findFieldsContaining(endIx);
                if (m != matching) {
                    break;
                }
            }
            if (!matching.empty()) {
                // TODO: remove this: do it at a higher level when we
                // are emitting the columns
                if (leftOrRight) {
                    warningStream << "in " << leftOrRight << " ";
                }
                warningStream << "BITS";
                warningStream << fmtBitRange(bitIx, endIx - bitIx) << ": ";
                warningStream << "PC:" << loc.offset <<
                    ": multiple fields map this range\n";
            } else {
                // insert an ERROR field here since nothing maps there
                Field errorField{UNMAPPED_FIELD_NAME, (int)bitIx, (int)(endIx - bitIx)};
                FieldList::iterator itr = fields.begin();
                for (; itr != fields.end() && itr->first.offset < errorField.offset; itr++)
                    ;
                fields.insert(itr,std::pair<Field,std::string>(errorField,""));
            }
            bitIx = endIx; // sanity restarts (or some new error)
        }
    }
    if (!warningStream.str().empty()) {
        success = false;
        emitRedText(os, warningStream.str());
    }

    return fields;
}



static void decodeFieldHeaders(std::ostream &os)
{
    os << std::setw(FIELD_PC_WIDTH) << std::left << "PC";
    os << " ";
    os << std::setw(FIELD_NAME_WIDTH) << std::left << "FIELD";
    os << " ";
    os << std::setw(FIELD_OFFSET_WIDTH) << " "; // [...:..]
    os << " ";
    os << std::setw(FIELD_SIZE_WIDTH) << " "; // (..)
    os << " ";
    os << std::setw(FIELD_VALUE_INT_WIDTH + 1 + FIELD_VALUE_STR_WIDTH) <<
        std::right << "VALUE";
    os << "\n";
}

static bool decodeFieldsForInst(
    bool useNativeDencoder,
    std::ostream &os,
    size_t pc,
    const Model &model,
    const MInst *mi)
{
    auto syntax = disassembleInst(model.platform, useNativeDencoder, pc, (const void *)mi);
    os << fmtPc(mi, pc) << " " <<  syntax << "\n";
    os.flush();
    bool success = true;
    FieldList fields = decodeFieldsWithWarnings(
        model, os, Loc((uint32_t)pc), mi, success);
    for (const auto &fv : fields) {
        const auto &f = fv.first;
        uint64_t val = mi->getField(f);
        std::stringstream ss;
        ss << std::setw(FIELD_PC_WIDTH) << std::left << " ";
        ss << " ";

        ss << std::setw(FIELD_NAME_WIDTH) << std::left << f.name;
        ss << " ";

        ss << std::setw(FIELD_OFFSET_WIDTH) << std::right << fmtBitRange(f);
        ss << " ";

        // e.g. (32)
        ss << std::right << std::setw(FIELD_SIZE_WIDTH) << fmtSize(f);
        ss << " ";

        // 0x1234
        ss << std::right << std::setw(FIELD_VALUE_INT_WIDTH) << fmtHexValue(val);
        ss << " ";

        // :q
        ss << std::right << std::setw(FIELD_VALUE_STR_WIDTH) << fv.second;
        ss << "\n";

        // TODO: need to also emit warnings on overlapped fields
        if (std::string(UNMAPPED_FIELD_NAME) == f.name) {
            emitRedText(os, ss.str());
        } else {
            os << ss.str();
        }
    }
    return success;
}

bool iga::DecodeFields(
    Platform p,
    bool useNativeDencoder, // TODO: use this once decoder is implemented
    std::ostream &os,
    const uint8_t *bits,
    size_t bitsLen)
{
    const Model *model = Model::LookupModel(p);
    if (model == nullptr) {
        emitRedText(os, "ERROR: unknown model\n");
        return false;
    }

    decodeFieldHeaders(os);
    bool success = true;

    size_t pc = 0;
    while (pc < bitsLen) {
        if (bitsLen - pc < 4) {
            emitYellowText(os, "WARNING: extra padding at end of kernel\n");
            break;
        }
        const MInst *mi = (const MInst *)&bits[pc];
        size_t iLen = mi->isCompact() ? 8 : 16;
        if (bitsLen - pc < iLen) {
            emitYellowText(os, "WARNING: extra padding at end of kernel\n");
            break;
        }

        success &= decodeFieldsForInst(useNativeDencoder, os, pc, *model, mi);

        pc += iLen;
    }

    return success;
}


bool iga::DiffFields(
    Platform p,
    bool useNativeDencoder, // TODO: use this
    std::ostream &os,
    const char *source1,
    const uint8_t *bits1,
    size_t bitsLen1,
    const char *source2,
    const uint8_t *bits2,
    size_t bitsLen2)
{
    const Model *model = Model::LookupModel(p);

    if (model == nullptr) {
        emitRedText(std::cerr, "ERROR: unknown model\n");
        return false;
    }
    if (source1 == nullptr) {
        source1 = "VALUE1";
    }
    if (source2 == nullptr) {
        source2 = "VALUE2";
    }

    os << std::setw(FIELD_PC_WIDTH) << std::left << "PC";
    os << " ";
    os << std::setw(FIELD_NAME_WIDTH) << std::left << "FIELD";
    os << " ";
    os << std::setw(FIELD_OFFSET_WIDTH) << " "; // [...:..]
    os << " ";
    os << std::setw(FIELD_SIZE_WIDTH) << " "; // (..)
    os << " ";
    os << std::setw(FIELD_VALUE_INT_WIDTH + 1 + FIELD_VALUE_STR_WIDTH) << std::right << source1;
    os << " ";
    os << std::setw(FIELD_VALUE_INT_WIDTH + 1 + FIELD_VALUE_STR_WIDTH) << std::right << source2;
    os << "\n";

    bool success = true;
    size_t pc1 = 0, pc2 = 0;
    while (pc1 < bitsLen1 || pc2 < bitsLen2) {
        auto getPc = [&] (const char *which, const uint8_t *bits, size_t bitsLen, size_t pc) {
            if (bitsLen - pc < 4) {
                std::stringstream ss;
                ss << which << ": extra padding at end of kernel";
            }
            const iga::MInst *mi = (const iga::MInst *)&bits[pc];
            if (mi->isCompact() && bitsLen - pc < 8 ||
                bitsLen - pc < 16)
            {
                std::stringstream ss;
                ss << which << ": extra padding at end of kernel";
            }
            return mi;
        };
        const iga::MInst *mi1 = getPc("kernel 1", bits1, bitsLen1, pc1),
                         *mi2 = getPc("kernel 2", bits2, bitsLen2, pc2);

        // TODO: do a colored diff here (words with given separators)
        os << fmtPc(mi1, pc1) << " " <<
            disassembleInst(p, useNativeDencoder, pc1, (const void *)mi1) << "\n";
        os << fmtPc(mi2, pc2) << " " <<
            disassembleInst(p, useNativeDencoder, pc2, (const void *)mi2) << "\n";

        FieldList fields1;
        bool successLeft = true;
        if (mi1) {
            fields1 = decodeFieldsWithWarnings(*model, os, Loc((uint32_t)pc1), mi1, successLeft, "left");
        }
        bool successRight = true;
        FieldList fields2;
        if (mi2) {
            fields2 = decodeFieldsWithWarnings(*model, os, Loc((uint32_t)pc2), mi2, successRight, "right");
        }
        success &= successLeft & successRight;

        // union the fields
        FieldSet allFields;
        for (const auto &f : fields1) {allFields.insert(&f.first);}
        for (const auto &f : fields2) {allFields.insert(&f.first);}

        for (const Field *f : allFields) {
            auto inList = [&] (const FieldList &flist) {
                auto fieldEq = [&](const FieldListElem &p) {
                    return p.first == *f;
                };
                return std::find_if(flist.begin(), flist.end(), fieldEq) != flist.end();
            };
            auto findStr = [&](const FieldList &flist) {
                for (const auto &fp : flist) {
                    if (fp.first == *f) {
                        return std::string(fp.second);
                    }
                }
                return std::string("");
            };

            bool inList1 = mi1 != nullptr && inList(fields1);
            uint64_t val1 = mi1 ? mi1->getField(*f) : 0;
            std::string valStr1 = findStr(fields1);

            bool inList2 = mi2 != nullptr && inList(fields2);
            uint64_t val2 = mi2 ? mi2->getField(*f) : 0;
            std::string valStr2 = findStr(fields2);

            auto fmtHexValue = [](uint64_t val) {
                std::stringstream ss;
                ss << "0x" << std::hex << val;
                return ss.str();
            };

            std::string diffToken = "  ";
            if (inList1 && inList2) {
                if (val1 != val2) {
                    diffToken = "~~"; // changed
                } else {
                    diffToken = "  ";
                }
            } else if (inList1) {
                diffToken = "--"; // removed
            } else if (inList2) {
                diffToken = "++"; // added
            } else {
                diffToken = "  ";
            }
            os << std::setw(FIELD_PC_WIDTH) << std::left << diffToken;

            // e.g. Src1.Imm32
            os << std::setw(FIELD_NAME_WIDTH) << std::left << f->name;
            os << " ";

            // e.g. [127:96]
            os << std::setw(FIELD_OFFSET_WIDTH) << std::left << fmtBitRange(*f);
            os << " ";

            // e.g. (32)
            os << std::setw(FIELD_SIZE_WIDTH) << std::left << fmtSize(*f);
            os << " ";

            // bit value
            auto fmtElem = [&](bool inList, uint64_t val, const std::string &str) {
                if (inList) {
                    os << std::right << std::setw(FIELD_VALUE_INT_WIDTH) << fmtHexValue(val);
                } else {
                    os << std::right << std::setw(FIELD_VALUE_INT_WIDTH) << " ";
                }
                os << " " << std::setw(FIELD_VALUE_STR_WIDTH) << str;
            };
            if (inList1 && val1 != val2) {
                os << Intensity::BRIGHT << Color::GREEN;
            }
            fmtElem(inList1, val1, valStr1);
            os << " ";

            if (inList2 && val1 != val2) {
                os << Intensity::BRIGHT << Color::RED;
            }
            fmtElem(inList2, val2, valStr2);

            if (val1 != val2) {
                os << Reset::RESET;
            }
            os << "\n";
        } // for

        // on to the next instruction
        if (mi1) {
            pc1 += mi1->isCompact() ? 8 : 16;
        }
        if (mi2) {
            pc2 += mi2->isCompact() ? 8 : 16;
        }
    } // for all instructions in both streams (in parallel)

    return success;
}


static std::string fmtHex(uint64_t val, int w)
{
    std::stringstream ss;
    if (w > 0) {
        ss << "0x" << std::setw(w) << std::setfill('0') <<
            std::hex << std::uppercase << val;
    } else {
        ss << "0x" << std::hex << std::uppercase << val;
    }
    return ss.str();
};

static void emitBits(std::ostream &os, int len, uint64_t val)
{
    for (int i = len - 1; i >= 0; i--) {
        if (val & (1ull<<(uint64_t)i)) {
            os << '1';
        } else {
            os << '0';
        }
    }
}

// emits output such as  "0`001`1`0`001"
// for SrcImm compacted fields it just emits the value
static void formatCompactionFieldValue(
    std::ostream &os,
    const CompactedField &cf,
    uint64_t val)
{
    if (cf.mappings == nullptr) {
        // srcimm field
        os << std::setw(16) << "(" << fmtHexValue(val) << ")";
    } else {
        int bitOff = (int)cf.countNumBitsMapped();
        for (int mIx = 0; mIx < (int)cf.numMappings; ++mIx) {
            if (mIx != 0) {
                os << "`";
            }
            const Field *mf = cf.mappings[mIx];
            bitOff -= mf->length;
            auto bs = iga::getBits(val, bitOff, mf->length);
            emitBits(os, mf->length, bs);
        }
    }
}


typedef size_t PC;
typedef int64_t Mapping;
typedef std::pair<PC,int> PCStats;
struct MappingStats
{
//    int64_t uniqueMisses = 0;

    // the specific misses (PC,num-fields-missed)
    std::vector<PCStats> misses;

    void orderMisses() {
        std::sort(misses.begin(), misses.end(),
            [] (const PCStats &m1, const PCStats &m2) {
                return m1.second < m2.second;
            });
    }
};
struct CompactionStats
{
    int64_t hits = 0;
    int64_t misses = 0;
    int64_t noCompactSet = 0;
    int64_t noCompactForm = 0; //e.g. send
    // maps field that missed to
    //   all the values that missed
    //   each value that missed maps to:
    //       - # of times it missed
    //       - # of times this miss was the unique
    std::map<const CompactedField *,std::map<Mapping,MappingStats>> fieldMisses;
};

static bool listInstructionCompaction(
    bool useNativeDecoder,
    std::ostream &os,
    CompactionStats &cmpStats,
    const Model &m,
    size_t pc,
    const MInst *bits)
{
    const int MAX_HAMMING_DIST   = 6; // what we consider a "close"  mapping
    const int MAX_CLOSE_MAPPINGS = 6;

    MInst compactedInst;
    CompactionDebugInfo cdi;
    CompactionResult cr = iga::native::DebugCompaction(m, bits, &compactedInst, cdi);

    bool success = true;
    switch (cr) {
    case CompactionResult::CR_SUCCESS:
        cmpStats.hits++;
        os << Color::GREEN << Intensity::BRIGHT <<
            "=> compaction hit " << Reset::RESET << "\n";
        success &= decodeFieldsForInst(useNativeDecoder, os, pc, m, &compactedInst);
        break;
    case CompactionResult::CR_MISS: {
        cmpStats.misses++;
        os << Color::YELLOW << Intensity::BRIGHT <<
            "=> compaction miss " << Reset::RESET << "\n";

        bool missesImm = false;
        for (const CompactedField *cf : cdi.fieldMisses) {
            missesImm |= cf->isSrcImmField();
        }

        for (size_t i = 0, len = cdi.fieldMisses.size(); i < len; i++) {
            const CompactedField &cf = *cdi.fieldMisses[i];
            uint64_t missedMapping = cdi.fieldMapping[i];

            auto &mms = cmpStats.fieldMisses[&cf];
            MappingStats &ms = mms[missedMapping];
            ms.misses.push_back(PCStats(pc,(int)cdi.fieldMisses.size()));

            // emit the field that missed
            os << Color::WHITE << Intensity::BRIGHT <<
                cf.index.name << Reset::RESET;
            os << ": ";
            for (int mIx = 0; mIx < (int)cf.numMappings; ++mIx) {
                if (mIx != 0) {
                    os << "`";
                }
                const Field *mf = cf.mappings[mIx];
                os << mf->name;
            }
            os << " misses compaction table\n";

            // compute the total number of bits in mappings
            // and the number of hex digits needed to for output alignment
            int hexDigits = ((int)cf.countNumBitsMapped() + 4 - 1)/4; // align up

            // emit the actual mapping we were looking for
            os << "       " << fmtHex(missedMapping, hexDigits);
            if (cf.mappings == nullptr) { // e.g. src1imm12, skip it
                os << "   [no explicit mappings] \n";
                continue;
            }
            os << ": ";
            formatCompactionFieldValue(os, cf, missedMapping);
            os << ": (";
            if (cf.format) {
                os << cf.format(missedMapping);
            }
            os << ") is the necessary table entry\n";

            // find the closest hits
            std::vector<size_t> closestIndices;
            for (size_t dist = 1;
                dist < MAX_HAMMING_DIST && closestIndices.size() < MAX_CLOSE_MAPPINGS;
                dist++)
            {
                // closest hits of distance `dist`
                for (size_t mIx = 0; mIx < cf.numMappings; mIx++) {
                    uint64_t val = cf.values[mIx];
                    std::bitset<64> bs(val ^ missedMapping);
                    if (bs.count() == dist) {
                        closestIndices.push_back(mIx);
                    }
                }
            } // for dist=1...8 or until we find nearest K

              // emit information on the closest mappings
            if (closestIndices.empty()) {
                os << "  no close mappings found\n";
            } else {
                os << "  closest mappings are:\n";
                for (auto cIx : closestIndices) {
                    uint64_t closeMapping = cf.values[cIx];
                    std::bitset<64> bs(closeMapping ^ missedMapping);
                    os << "  #" << std::setw(2) << std::left << cIx;
                    os << "  " << fmtHex(cf.values[cIx], hexDigits) << ": ";
                    int bitOff = (int)cf.countNumBitsMapped();
                    // walk through the fields listing who misses (from high to low bits)
                    std::vector<const Field *> missingFields;
                    for (int mIx = 0; mIx < (int)cf.numMappings; ++mIx) {
                        if (mIx != 0) {
                            os << "`";
                        }
                        const Field *mf = cf.mappings[mIx];
                        bitOff -= mf->length;
                        auto missedVal = iga::getBits(missedMapping, bitOff, mf->length);
                        auto closeVal = iga::getBits(closeMapping, bitOff, mf->length);
                        if (missedVal != closeVal) {
                            os << Color::RED << Intensity::BRIGHT;
                            emitBits(os, mf->length, closeVal);
                            os << Reset::RESET;
                            missingFields.push_back(mf);
                        } else {
                            emitBits(os, mf->length, closeVal);
                        }
                    }
                    os << ": ";

                    commafyList(os, missingFields,
                        [] (std::ostream &os, const Field *f)
                    {
                        os << Color::RED << Intensity::BRIGHT;
                        os << f->name;
                        os << Reset::RESET;
                    });
                    if (missingFields.size() == 1) {
                        os << " misses";
                    } else {
                        os << " miss";
                    }
                    os << "\n";
                }
            }
        }
        break;
    }
    case CompactionResult::CR_NO_COMPACT:
        cmpStats.noCompactSet++;
        os << Color::WHITE << "{NoCompact/Uncompacted} set" << Reset::RESET << "\n";
        break;
    case CompactionResult::CR_NO_FORMAT:
        cmpStats.noCompactForm++;
        emitRedText(os, "=> no compacted form\n");
        break;
    default:
        os << "INTERNAL ERROR: " << (int)cr << "\n";
    }
    return success;
}

bool iga::DebugCompaction(
    Platform p,
    bool useNativeDencoder,
    std::ostream &os,
    const uint8_t *bits,
    size_t bitsLen)
{
    const Model *model = Model::LookupModel(p);
    if (model == nullptr) {
        emitRedText(os, "ERROR: unknown model\n");
        return false;
    }

    CompactionStats cs;

    bool success = true;
    size_t pc = 0;
    while (pc < bitsLen) {
        if (bitsLen - pc < 4) {
            emitYellowText(os, "WARNING: extra padding at end of kernel\n");
            break;
        }
        const MInst *mi = (const MInst *)&bits[pc];
        size_t iLen = mi->isCompact() ? 8 : 16;
        if (bitsLen - pc < iLen) {
            emitYellowText(os, "WARNING: extra padding at end of kernel\n");
            break;
        }

        os << "============================================================\n";
        auto syntax = disassembleInst(p, useNativeDencoder, pc, (const void *)mi);
        os << fmtPc(mi, pc) << " " <<  syntax << "\n";
        os.flush();
        success &= listInstructionCompaction(
            useNativeDencoder, os, cs, *model, pc, mi);
        pc += iLen;
    }
    os << "\n";
    os << "\n";
    os << "*************************** SUMMARY ***************************\n";
    uint64_t totalInsts =
        cs.hits + cs.misses + cs.noCompactSet + cs.noCompactForm;
    auto emitSubset = [&](const char *name, int64_t val) {
        os << "  ";
        std::string title = name;
        title += ':';
        os << std::setw(24) << std::setfill(' ') << std::left << title;
        os << std::setw(12) << std::right << val;
        os << "  ";
        double pct = 100.0*val/(double)totalInsts;
        os << std::setprecision(3) << std::setw(8) << std::right << pct << "%";
        os << "\n";
    };
    emitSubset("hits", cs.hits);
    emitSubset("misses", cs.misses);
    emitSubset("{Uncompacted} set", cs.noCompactSet);
    emitSubset("no compact form", cs.noCompactForm);
    os << std::setw(24) << "TOTAL:" <<
        std::setw(10) << std::right << totalInsts << "\n";
    os << "***************************************************************\n";
    // show the various indices that missed
    int64_t totalMisses = 0;
    for (const auto &missEntry : cs.fieldMisses) {
        const CompactedField *cf = missEntry.first;
        os << std::setw(24) << cf->index.name;
        int64_t misses = 0;
        for (const auto ms : missEntry.second) {
            misses += ms.second.misses.size();
        }
        totalMisses += misses;

        os << "  ";
        os << std::setw(10) << std::right << misses;
        os << "\n";
    }

    // now list the actual misses
    os << "***************************************************************\n";
    for (const auto &missEntry : cs.fieldMisses) {
        const CompactedField *cf = missEntry.first;
        os << cf->index.name << "\n";

        // order the misses by the total number
        std::vector<std::pair<Mapping,MappingStats>> orderedEntries;
        for (const auto ms : missEntry.second) {
            orderedEntries.push_back(ms);
        }
        std::sort(
            orderedEntries.begin(),
            orderedEntries.end(),
            [] (const std::pair<Mapping,MappingStats> &p1,
                const std::pair<Mapping,MappingStats> &p2)
            {
                return p1.second.misses.size() > p2.second.misses.size();
            }
            );

        for (auto &ms : orderedEntries) {
            Mapping mVal = ms.first;
            MappingStats &mStats = ms.second;
            os << "    ";
            formatCompactionFieldValue(os, *cf, mVal); // e.g. 000`0010`0`00
            os << "  total misses:" << mStats.misses.size();
            size_t misses1 = 0, misses2 = 0, misses3plus = 0;
            for (const auto &missExample : mStats.misses) {
                if (missExample.second == 1) {
                    misses1++;
                } else if (missExample.second == 1) {
                    misses2++;
                } else if (missExample.second == 2) {
                    misses2++;
                } else {
                    misses3plus++;
                }
            }
            os << "  1m:" << misses1 << "  2m:" << misses2 << "  3+m:" << misses3plus;

            if (cf->format) {
                os << "  ";
                os << cf->format(mVal);
            }
            os << "\n";

            // emit those that miss
            // mStats.orderMisses();
            for (const auto &missExample : mStats.misses) {
                PC pc = missExample.first;
                int totalMissesForThisPc = missExample.second;
                os << "        misses " << totalMissesForThisPc << ": "
                    << disassembleInst(p, useNativeDencoder, pc, bits+pc)
                    << "\n";
            }
        }
    }

    return success;
}