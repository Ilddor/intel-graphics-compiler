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
#pragma once

#if defined(_WIN32)
    #include <intrin.h>
#else
    #include <x86intrin.h>
#endif

#include "types.h"


namespace iSTD
{

/*****************************************************************************\
Inline Function:
    FastClamp

Description:
    Fast clamping implementation(s) for 4xfloats
\*****************************************************************************/
__forceinline void FastClampF(  const __m128 &inMins, 
                              const __m128 &inMaxs, 
                              float* oDest)
{
    // load data to be clamped into 128 register
    __m128 vals = _mm_loadu_ps(oDest);

    // clamp
    vals = _mm_min_ps(inMaxs, _mm_max_ps(vals, inMins));

    // load into output
    _mm_storeu_ps(oDest, vals);
}

__forceinline void FastClampF(  const __m128 &inMins, 
                                const __m128 &inMaxs, 
                                float* oDest, 
                                const float* inSrc )
{
    // load data to be clamped into 128 register
    __m128 vals = _mm_loadu_ps(inSrc);

    // clamp
    vals = _mm_min_ps(inMaxs, _mm_max_ps(vals, inMins));

    // load into output
    _mm_storeu_ps(oDest, vals);
}

__forceinline void FastClampF(  const __m128 &inMins, 
                              const __m128 &inMaxs, 
                              float* oDest, 
                              const __m128 &inSrc )
{
    // clamp
    __m128 vals = _mm_min_ps(inMaxs, _mm_max_ps(inSrc, inMins));

    // load into output
    _mm_storeu_ps(oDest, vals);
}

} // iSTD
