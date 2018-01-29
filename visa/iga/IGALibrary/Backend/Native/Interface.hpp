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
////////////////////////////////////////////
// NATIVE ENCODER USERS USE THIS INTERFACE
#include "../EncoderOpts.hpp"
#include "../../ErrorHandler.hpp"
#include "../../IR/Kernel.hpp"
#include "InstEncoder.hpp"
#include "Field.hpp"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace iga {
    typedef std::pair<Field,std::string>         FieldListElem;
    typedef std::vector<FieldListElem>           FieldList;
    struct FieldPtrCmp
    {
        bool operator() (const Field *f1, const Field *f2) const {
            return *f1 < *f2;
        }
    };
    typedef std::set<const Field *,FieldPtrCmp>   FieldSet;
}

namespace iga {namespace native
{
    void Encode(
        const Model &m,
        const EncoderOpts &opts,
        ErrorHandler &eh,
        Kernel &k,
        void *&bits,
        int &bitsLen);

    void DecodeFields(
        Loc loc,
        const Model &model,
        const void *bits,
        FieldList &fields,
        ErrorHandler &errHandler);

    CompactionResult DebugCompaction(
        const Model &m,
        const void *uncompactedBits, // naative bits
        void *compactedOutput,       // optional param
        CompactionDebugInfo &cbdi);

// void Decode(
//    const Model &m,
//    ErrorHandler &eh,
//    const void *bits,
//    int bitsLen,
//    MemManager &mm,
//    Kernel *&k,
// );

}} // namespace