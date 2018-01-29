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
#ifndef FLOATS_HPP
#define FLOATS_HPP

#include <cstdint>
#include <cmath> // needed for android build!
#include <iostream>

// Provides utilities for dealing with floating point numbers including some
// minimal fp16 support.

#if !defined(_WIN32) || (_MSC_VER >= 1800)
// GCC and VS2013 and higher support these
#define IS_NAN(X) std::isnan(X)
#define IS_INF(X) std::isinf(X)
#else
#define IS_NAN(X) ((X) != (X))
#define IS_INF(X) (!IS_NAN(X) && IS_NAN((X) - (X)))
#endif

namespace iga {
// formats a floating point value in decimal if possible
// otherwise it falls back to hex
void FormatFloat(std::ostream &os, double d);
void FormatFloat(std::ostream &os, float f);
void FormatFloat(std::ostream &os, uint16_t h);
void FormatFloat(std::ostream &os, uint8_t q); // GEN's 8-bit restricted float

// These functions exist since operations on NaN values might change the NaN
// payload.  E.g. An sNan might convert to a qNan during a cast
float     ConvertDoubleToFloat(double d);
uint16_t  ConvertFloatToHalf(float f);
static
uint16_t  ConvertDoubleToHalf(double d) {
    return ConvertFloatToHalf(ConvertDoubleToFloat(d));
}
float     ConvertHalfToFloat(uint16_t u16);
double    ConvertFloatToDouble(float f32);

// This expands Intel GEN's restricted 8-bit format
float     ConvertQuarterToFloatGEN(uint8_t u8);


// Various raw accessors to convert between bits and float
static uint64_t FloatToBits(double f) {
    union{double f; uint64_t i;} u;
    u.f = f;
    return u.i;
}
static uint32_t FloatToBits(float f) {
    union{float f; uint32_t i;} u;
    u.f = f;
    return u.i;
}
static uint16_t FloatToBits(uint16_t f) {return f;}
static double FloatFromBits(uint64_t f) {
    union{double f; uint64_t i;} u;
    u.i = f;
    return u.f;
}
static float FloatFromBits(uint32_t f) {
    union{float f; uint32_t i;} u;
    u.i = f;
    return u.f;
}
static uint16_t FloatFromBits(uint16_t f) {return f;}


static const uint64_t IGA_F64_SIGN_BIT  = 0x8000000000000000ull;
static const uint64_t IGA_F64_EXP_MASK  = 0x7FF0000000000000ull;
static const uint64_t IGA_F64_MANT_MASK = 0x000FFFFFFFFFFFFFull;
static const uint64_t IGA_F64_SNAN_BIT  = 0x0008000000000000ull;
static const uint32_t IGA_F32_SIGN_BIT  = 0x80000000;
static const uint32_t IGA_F32_EXP_MASK  = 0x7F800000;
static const uint32_t IGA_F32_MANT_MASK = 0x007FFFFF;
static const uint32_t IGA_F32_SNAN_BIT  = 0x00400000;
static const uint16_t IGA_F16_SIGN_BIT  = 0x8000;
static const uint16_t IGA_F16_EXP_MASK  = 0x7C00;
static const uint16_t IGA_F16_MANT_MASK = 0x03FF;
static const uint16_t IGA_F16_SNAN_BIT  = 0x0200;
} // namespace iga

#endif // FLOATS_HPP
