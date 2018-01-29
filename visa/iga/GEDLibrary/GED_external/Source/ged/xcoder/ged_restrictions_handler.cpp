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

#include "xcoder/ged_restrictions_handler.h"

/*************************************************************************************************
 * class GEDRestrictionsHandler static data members
 *************************************************************************************************/

const int64_t GEDRestrictionsHandler::checkNegativeTable[] =
{
    (int64_t)0x1,                // 0
    (int64_t)0x2,                // 1
    (int64_t)0x4,                // 2
    (int64_t)0x8,                // 3
    (int64_t)0x10,               // 4
    (int64_t)0x20,               // 5
    (int64_t)0x40,               // 6
    (int64_t)0x80,               // 7
    (int64_t)0x100,              // 8
    (int64_t)0x200,              // 9
    (int64_t)0x400,              // 10
    (int64_t)0x800,              // 11
    (int64_t)0x1000,             // 12
    (int64_t)0x2000,             // 13
    (int64_t)0x4000,             // 14
    (int64_t)0x8000,             // 15
    (int64_t)0x10000,            // 16
    (int64_t)0x20000,            // 17
    (int64_t)0x40000,            // 18
    (int64_t)0x80000,            // 19
    (int64_t)0x100000,           // 20
    (int64_t)0x200000,           // 21
    (int64_t)0x400000,           // 22
    (int64_t)0x800000,           // 23
    (int64_t)0x1000000,          // 24
    (int64_t)0x2000000,          // 25
    (int64_t)0x4000000,          // 26
    (int64_t)0x8000000,          // 27
    (int64_t)0x10000000,         // 28
    (int64_t)0x20000000,         // 29
    (int64_t)0x40000000,         // 30
    (int64_t)0x80000000,         // 31
    (int64_t)0x100000000,        // 32
    (int64_t)0x200000000,        // 33
    (int64_t)0x400000000,        // 34
    (int64_t)0x800000000,        // 35
    (int64_t)0x1000000000,       // 36
    (int64_t)0x2000000000,       // 37
    (int64_t)0x4000000000,       // 38
    (int64_t)0x8000000000,       // 39
    (int64_t)0x10000000000,      // 40
    (int64_t)0x20000000000,      // 41
    (int64_t)0x40000000000,      // 42
    (int64_t)0x80000000000,      // 43
    (int64_t)0x100000000000,     // 44
    (int64_t)0x200000000000,     // 45
    (int64_t)0x400000000000,     // 46
    (int64_t)0x800000000000,     // 47
    (int64_t)0x1000000000000,    // 48
    (int64_t)0x2000000000000,    // 49
    (int64_t)0x4000000000000,    // 50
    (int64_t)0x8000000000000,    // 51
    (int64_t)0x10000000000000,   // 52
    (int64_t)0x20000000000000,   // 53
    (int64_t)0x40000000000000,   // 54
    (int64_t)0x80000000000000,   // 55
    (int64_t)0x100000000000000,  // 56
    (int64_t)0x200000000000000,  // 57
    (int64_t)0x400000000000000,  // 58
    (int64_t)0x800000000000000,  // 59
    (int64_t)0x1000000000000000, // 60
    (int64_t)0x2000000000000000, // 61
    (int64_t)0x4000000000000000, // 62
    (int64_t)0x8000000000000000  // 63
};
