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

#include "../include/BiF_Definitions.cl"
#include "../../Headers/spirv.h"

INLINE float __builtin_spirv_OpenCL_modf_f32_p1f32( float           x,
                                      __global float* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f32(x);
    return __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x) ? (float)(0.0f) : x - *iptr), x);
}

INLINE float2 __builtin_spirv_OpenCL_modf_v2f32_p1v2f32( float2           x,
                                           __global float2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f32(x);
    float2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    return temp;
}

INLINE float3 __builtin_spirv_OpenCL_modf_v3f32_p1v3f32( float3           x,
                                           __global float3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f32(x);
    float3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    return temp;
}

INLINE float4 __builtin_spirv_OpenCL_modf_v4f32_p1v4f32( float4           x,
                                           __global float4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f32(x);
    float4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    return temp;
}

INLINE float8 __builtin_spirv_OpenCL_modf_v8f32_p1v8f32( float8           x,
                                           __global float8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f32(x);
    float8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s4) ? (float)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s5) ? (float)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s6) ? (float)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s7) ? (float)(0.0f) : temp.s7), x.s7);
    return temp;
}

INLINE float16 __builtin_spirv_OpenCL_modf_v16f32_p1v16f32( float16           x,
                                              __global float16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f32(x);
    float16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s4) ? (float)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s5) ? (float)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s6) ? (float)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s7) ? (float)(0.0f) : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s8) ? (float)(0.0f) : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s9) ? (float)(0.0f) : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sa) ? (float)(0.0f) : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sb) ? (float)(0.0f) : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sc) ? (float)(0.0f) : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sd) ? (float)(0.0f) : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.se) ? (float)(0.0f) : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sf) ? (float)(0.0f) : temp.sf), x.sf);
    return temp;
}

INLINE float __builtin_spirv_OpenCL_modf_f32_p0f32( float            x,
                                      __private float* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f32(x);
    return __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x) ? (float)(0.0f) : x - *iptr), x);
}

INLINE float2 __builtin_spirv_OpenCL_modf_v2f32_p0v2f32( float2            x,
                                           __private float2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f32(x);
    float2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    return temp;
}

INLINE float3 __builtin_spirv_OpenCL_modf_v3f32_p0v3f32( float3            x,
                                           __private float3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f32(x);
    float3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    return temp;
}

INLINE float4 __builtin_spirv_OpenCL_modf_v4f32_p0v4f32( float4            x,
                                           __private float4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f32(x);
    float4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    return temp;
}

INLINE float8 __builtin_spirv_OpenCL_modf_v8f32_p0v8f32( float8            x,
                                           __private float8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f32(x);
    float8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s4) ? (float)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s5) ? (float)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s6) ? (float)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s7) ? (float)(0.0f) : temp.s7), x.s7);
    return temp;
}

INLINE float16 __builtin_spirv_OpenCL_modf_v16f32_p0v16f32( float16            x,
                                              __private float16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f32(x);
    float16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s4) ? (float)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s5) ? (float)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s6) ? (float)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s7) ? (float)(0.0f) : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s8) ? (float)(0.0f) : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s9) ? (float)(0.0f) : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sa) ? (float)(0.0f) : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sb) ? (float)(0.0f) : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sc) ? (float)(0.0f) : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sd) ? (float)(0.0f) : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.se) ? (float)(0.0f) : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sf) ? (float)(0.0f) : temp.sf), x.sf);
    return temp;
}

INLINE float __builtin_spirv_OpenCL_modf_f32_p3f32( float          x,
                                      __local float* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f32(x);
    return __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x) ? (float)(0.0f) : x - *iptr), x);
}

INLINE float2 __builtin_spirv_OpenCL_modf_v2f32_p3v2f32( float2          x,
                                           __local float2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f32(x);
    float2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    return temp;
}

INLINE float3 __builtin_spirv_OpenCL_modf_v3f32_p3v3f32( float3          x,
                                           __local float3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f32(x);
    float3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    return temp;
}

INLINE float4 __builtin_spirv_OpenCL_modf_v4f32_p3v4f32( float4          x,
                                           __local float4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f32(x);
    float4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    return temp;
}

INLINE float8 __builtin_spirv_OpenCL_modf_v8f32_p3v8f32( float8          x,
                                           __local float8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f32(x);
    float8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s4) ? (float)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s5) ? (float)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s6) ? (float)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s7) ? (float)(0.0f) : temp.s7), x.s7);
    return temp;
}

INLINE float16 __builtin_spirv_OpenCL_modf_v16f32_p3v16f32( float16          x,
                                              __local float16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f32(x);
    float16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s4) ? (float)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s5) ? (float)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s6) ? (float)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s7) ? (float)(0.0f) : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s8) ? (float)(0.0f) : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s9) ? (float)(0.0f) : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sa) ? (float)(0.0f) : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sb) ? (float)(0.0f) : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sc) ? (float)(0.0f) : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sd) ? (float)(0.0f) : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.se) ? (float)(0.0f) : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sf) ? (float)(0.0f) : temp.sf), x.sf);
    return temp;
}

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

INLINE float __builtin_spirv_OpenCL_modf_f32_p4f32( float            x,
                                      __generic float* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f32(x);
    return __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x) ? (float)(0.0f) : x - *iptr), x);
}

INLINE float2 __builtin_spirv_OpenCL_modf_v2f32_p4v2f32( float2            x,
                                           __generic float2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f32(x);
    float2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    return temp;
}

INLINE float3 __builtin_spirv_OpenCL_modf_v3f32_p4v3f32( float3            x,
                                           __generic float3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f32(x);
    float3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    return temp;
}

INLINE float4 __builtin_spirv_OpenCL_modf_v4f32_p4v4f32( float4            x,
                                           __generic float4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f32(x);
    float4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    return temp;
}

INLINE float8 __builtin_spirv_OpenCL_modf_v8f32_p4v8f32( float8            x,
                                           __generic float8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f32(x);
    float8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s4) ? (float)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s5) ? (float)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s6) ? (float)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s7) ? (float)(0.0f) : temp.s7), x.s7);
    return temp;
}

INLINE float16 __builtin_spirv_OpenCL_modf_v16f32_p4v16f32( float16            x,
                                              __generic float16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f32(x);
    float16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s0) ? (float)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s1) ? (float)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s2) ? (float)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s3) ? (float)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s4) ? (float)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s5) ? (float)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s6) ? (float)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s7) ? (float)(0.0f) : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s8) ? (float)(0.0f) : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.s9) ? (float)(0.0f) : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sa) ? (float)(0.0f) : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sb) ? (float)(0.0f) : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sc) ? (float)(0.0f) : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sd) ? (float)(0.0f) : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.se) ? (float)(0.0f) : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f32_f32((__intel_relaxed_isinf(x.sf) ? (float)(0.0f) : temp.sf), x.sf);
    return temp;
}

#endif //#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

#ifdef cl_khr_fp16

INLINE half __builtin_spirv_OpenCL_modf_f16_p1f16( half           x,
                                     __global half* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f16(x);
    return __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x) ? (half)(0.0f) : x - *iptr), x);
}

INLINE half2 __builtin_spirv_OpenCL_modf_v2f16_p1v2f16( half2           x,
                                          __global half2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f16(x);
    half2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    return temp;
}

INLINE half3 __builtin_spirv_OpenCL_modf_v3f16_p1v3f16( half3           x,
                                          __global half3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f16(x);
    half3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    return temp;
}

INLINE half4 __builtin_spirv_OpenCL_modf_v4f16_p1v4f16( half4           x,
                                          __global half4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f16(x);
    half4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    return temp;
}

INLINE half8 __builtin_spirv_OpenCL_modf_v8f16_p1v8f16( half8           x,
                                          __global half8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f16(x);
    half8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s4) ? (half)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s5) ? (half)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s6) ? (half)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s7) ? (half)(0.0f) : temp.s7), x.s7);
    return temp;
}

INLINE half16 __builtin_spirv_OpenCL_modf_v16f16_p1v16f16( half16           x,
                                             __global half16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f16(x);
    half16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s4) ? (half)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s5) ? (half)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s6) ? (half)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s7) ? (half)(0.0f) : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s8) ? (half)(0.0f) : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s9) ? (half)(0.0f) : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sa) ? (half)(0.0f) : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sb) ? (half)(0.0f) : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sc) ? (half)(0.0f) : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sd) ? (half)(0.0f) : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.se) ? (half)(0.0f) : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sf) ? (half)(0.0f) : temp.sf), x.sf);
    return temp;
}

INLINE half __builtin_spirv_OpenCL_modf_f16_p0f16( half            x,
                                     __private half* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f16(x);
    return __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x) ? (half)(0.0f) : x - *iptr), x);
}

INLINE half2 __builtin_spirv_OpenCL_modf_v2f16_p0v2f16( half2            x,
                                          __private half2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f16(x);
    half2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    return temp;
}

INLINE half3 __builtin_spirv_OpenCL_modf_v3f16_p0v3f16( half3            x,
                                          __private half3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f16(x);
    half3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    return temp;
}

INLINE half4 __builtin_spirv_OpenCL_modf_v4f16_p0v4f16( half4            x,
                                          __private half4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f16(x);
    half4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    return temp;
}

INLINE half8 __builtin_spirv_OpenCL_modf_v8f16_p0v8f16( half8            x,
                                          __private half8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f16(x);
    half8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s4) ? (half)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s5) ? (half)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s6) ? (half)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s7) ? (half)(0.0f) : temp.s7), x.s7);
    return temp;
}

INLINE half16 __builtin_spirv_OpenCL_modf_v16f16_p0v16f16( half16            x,
                                             __private half16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f16(x);
    half16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s4) ? (half)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s5) ? (half)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s6) ? (half)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s7) ? (half)(0.0f) : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s8) ? (half)(0.0f) : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s9) ? (half)(0.0f) : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sa) ? (half)(0.0f) : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sb) ? (half)(0.0f) : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sc) ? (half)(0.0f) : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sd) ? (half)(0.0f) : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.se) ? (half)(0.0f) : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sf) ? (half)(0.0f) : temp.sf), x.sf);
    return temp;
}

INLINE half __builtin_spirv_OpenCL_modf_f16_p3f16( half          x,
                                     __local half* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f16(x);
    return __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x) ? (half)(0.0f) : x - *iptr), x);
}

INLINE half2 __builtin_spirv_OpenCL_modf_v2f16_p3v2f16( half2          x,
                                          __local half2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f16(x);
    half2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    return temp;
}

INLINE half3 __builtin_spirv_OpenCL_modf_v3f16_p3v3f16( half3          x,
                                          __local half3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f16(x);
    half3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    return temp;
}

INLINE half4 __builtin_spirv_OpenCL_modf_v4f16_p3v4f16( half4          x,
                                          __local half4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f16(x);
    half4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    return temp;
}

INLINE half8 __builtin_spirv_OpenCL_modf_v8f16_p3v8f16( half8          x,
                                          __local half8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f16(x);
    half8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s4) ? (half)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s5) ? (half)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s6) ? (half)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s7) ? (half)(0.0f) : temp.s7), x.s7);
    return temp;
}

INLINE half16 __builtin_spirv_OpenCL_modf_v16f16_p3v16f16( half16          x,
                                             __local half16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f16(x);
    half16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s4) ? (half)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s5) ? (half)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s6) ? (half)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s7) ? (half)(0.0f) : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s8) ? (half)(0.0f) : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s9) ? (half)(0.0f) : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sa) ? (half)(0.0f) : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sb) ? (half)(0.0f) : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sc) ? (half)(0.0f) : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sd) ? (half)(0.0f) : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.se) ? (half)(0.0f) : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sf) ? (half)(0.0f) : temp.sf), x.sf);
    return temp;
}
#endif


#if defined(cl_khr_fp16) && (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

INLINE half __builtin_spirv_OpenCL_modf_f16_p4f16( half            x,
                                     __generic half* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f16(x);
    return __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x) ? (half)(0.0f) : x - *iptr), x);
}

INLINE half2 __builtin_spirv_OpenCL_modf_v2f16_p4v2f16( half2            x,
                                          __generic half2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f16(x);
    half2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    return temp;
}

INLINE half3 __builtin_spirv_OpenCL_modf_v3f16_p4v3f16( half3            x,
                                          __generic half3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f16(x);
    half3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    return temp;
}

INLINE half4 __builtin_spirv_OpenCL_modf_v4f16_p4v4f16( half4            x,
                                          __generic half4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f16(x);
    half4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    return temp;
}

INLINE half8 __builtin_spirv_OpenCL_modf_v8f16_p4v8f16( half8            x,
                                          __generic half8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f16(x);
    half8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s4) ? (half)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s5) ? (half)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s6) ? (half)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s7) ? (half)(0.0f) : temp.s7), x.s7);
    return temp;
}

INLINE half16 __builtin_spirv_OpenCL_modf_v16f16_p4v16f16( half16            x,
                                             __generic half16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f16(x);
    half16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s0) ? (half)(0.0f) : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s1) ? (half)(0.0f) : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s2) ? (half)(0.0f) : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s3) ? (half)(0.0f) : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s4) ? (half)(0.0f) : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s5) ? (half)(0.0f) : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s6) ? (half)(0.0f) : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s7) ? (half)(0.0f) : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s8) ? (half)(0.0f) : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.s9) ? (half)(0.0f) : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sa) ? (half)(0.0f) : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sb) ? (half)(0.0f) : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sc) ? (half)(0.0f) : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sd) ? (half)(0.0f) : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.se) ? (half)(0.0f) : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f16_f16((__intel_relaxed_isinf(x.sf) ? (half)(0.0f) : temp.sf), x.sf);
    return temp;
}
#endif //#if defined(cl_khr_fp16) && (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

#if defined(cl_khr_fp64)

double __builtin_spirv_OpenCL_modf_f64_p1f64( double           x,
                                       __global double* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f64(x);
    return __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x) ? 0.0 : x - *iptr), x);
}

double2 __builtin_spirv_OpenCL_modf_v2f64_p1v2f64( double2           x,
                                            __global double2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f64(x);
    double2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    return temp;
}

double3 __builtin_spirv_OpenCL_modf_v3f64_p1v3f64( double3           x,
                                            __global double3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f64(x);
    double3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    return temp;
}

double4 __builtin_spirv_OpenCL_modf_v4f64_p1v4f64( double4           x,
                                            __global double4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f64(x);
    double4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    return temp;
}

double8 __builtin_spirv_OpenCL_modf_v8f64_p1v8f64( double8           x,
                                            __global double8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f64(x);
    double8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s4) ? 0.0 : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s5) ? 0.0 : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s6) ? 0.0 : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s7) ? 0.0 : temp.s7), x.s7);
    return temp;
}

double16 __builtin_spirv_OpenCL_modf_v16f64_p1v16f64( double16           x,
                                               __global double16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f64(x);
    double16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s4) ? 0.0 : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s5) ? 0.0 : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s6) ? 0.0 : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s7) ? 0.0 : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s8) ? 0.0 : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s9) ? 0.0 : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sa) ? 0.0 : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sb) ? 0.0 : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sc) ? 0.0 : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sd) ? 0.0 : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.se) ? 0.0 : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sf) ? 0.0 : temp.sf), x.sf);
    return temp;
}

double __builtin_spirv_OpenCL_modf_f64_p0f64( double            x,
                                       __private double* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f64(x);
    return __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x) ? 0.0 : x - *iptr), x);
}

double2 __builtin_spirv_OpenCL_modf_v2f64_p0v2f64( double2            x,
                                            __private double2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f64(x);
    double2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    return temp;
}

double3 __builtin_spirv_OpenCL_modf_v3f64_p0v3f64( double3            x,
                                            __private double3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f64(x);
    double3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    return temp;
}

double4 __builtin_spirv_OpenCL_modf_v4f64_p0v4f64( double4            x,
                                            __private double4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f64(x);
    double4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    return temp;
}

double8 __builtin_spirv_OpenCL_modf_v8f64_p0v8f64( double8            x,
                                            __private double8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f64(x);
    double8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s4) ? 0.0 : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s5) ? 0.0 : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s6) ? 0.0 : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s7) ? 0.0 : temp.s7), x.s7);
    return temp;
}

double16 __builtin_spirv_OpenCL_modf_v16f64_p0v16f64( double16            x,
                                               __private double16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f64(x);
    double16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s4) ? 0.0 : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s5) ? 0.0 : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s6) ? 0.0 : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s7) ? 0.0 : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s8) ? 0.0 : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s9) ? 0.0 : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sa) ? 0.0 : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sb) ? 0.0 : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sc) ? 0.0 : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sd) ? 0.0 : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.se) ? 0.0 : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sf) ? 0.0 : temp.sf), x.sf);
    return temp;
}

double __builtin_spirv_OpenCL_modf_f64_p3f64( double          x,
                                       __local double* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f64(x);
    return __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x) ? 0.0 : x - *iptr), x);
}

double2 __builtin_spirv_OpenCL_modf_v2f64_p3v2f64( double2        x,
                                          __local double2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f64(x);
    double2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    return temp;
}

double3 __builtin_spirv_OpenCL_modf_v3f64_p3v3f64( double3          x,
                                            __local double3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f64(x);
    double3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    return temp;
}

double4 __builtin_spirv_OpenCL_modf_v4f64_p3v4f64( double4          x,
                                            __local double4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f64(x);
    double4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    return temp;
}

double8 __builtin_spirv_OpenCL_modf_v8f64_p3v8f64( double8          x,
                                            __local double8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f64(x);
    double8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s4) ? 0.0 : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s5) ? 0.0 : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s6) ? 0.0 : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s7) ? 0.0 : temp.s7), x.s7);
    return temp;
}

double16 __builtin_spirv_OpenCL_modf_v16f64_p3v16f64( double16          x,
                                               __local double16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f64(x);
    double16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s4) ? 0.0 : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s5) ? 0.0 : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s6) ? 0.0 : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s7) ? 0.0 : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s8) ? 0.0 : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s9) ? 0.0 : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sa) ? 0.0 : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sb) ? 0.0 : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sc) ? 0.0 : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sd) ? 0.0 : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.se) ? 0.0 : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sf) ? 0.0 : temp.sf), x.sf);
    return temp;
}

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

double __builtin_spirv_OpenCL_modf_f64_p4f64( double            x,
                                       __generic double* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_f64(x);
    return __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x) ? 0.0 : x - *iptr), x);
}

double2 __builtin_spirv_OpenCL_modf_v2f64_p4v2f64( double2            x,
                                            __generic double2* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v2f64(x);
    double2 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    return temp;
}

double3 __builtin_spirv_OpenCL_modf_v3f64_p4v3f64( double3            x,
                                            __generic double3* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v3f64(x);
    double3 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    return temp;
}

double4 __builtin_spirv_OpenCL_modf_v4f64_p4v4f64( double4            x,
                                            __generic double4* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v4f64(x);
    double4 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    return temp;
}

double8 __builtin_spirv_OpenCL_modf_v8f64_p4v8f64( double8            x,
                                            __generic double8* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v8f64(x);
    double8 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s4) ? 0.0 : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s5) ? 0.0 : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s6) ? 0.0 : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s7) ? 0.0 : temp.s7), x.s7);
    return temp;
}

double16 __builtin_spirv_OpenCL_modf_v16f64_p4v16f64( double16            x,
                                               __generic double16* iptr )
{
    *iptr = __builtin_spirv_OpenCL_trunc_v16f64(x);
    double16 temp = x - *iptr;
    temp.s0 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s0) ? 0.0 : temp.s0), x.s0);
    temp.s1 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s1) ? 0.0 : temp.s1), x.s1);
    temp.s2 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s2) ? 0.0 : temp.s2), x.s2);
    temp.s3 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s3) ? 0.0 : temp.s3), x.s3);
    temp.s4 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s4) ? 0.0 : temp.s4), x.s4);
    temp.s5 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s5) ? 0.0 : temp.s5), x.s5);
    temp.s6 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s6) ? 0.0 : temp.s6), x.s6);
    temp.s7 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s7) ? 0.0 : temp.s7), x.s7);
    temp.s8 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s8) ? 0.0 : temp.s8), x.s8);
    temp.s9 = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.s9) ? 0.0 : temp.s9), x.s9);
    temp.sa = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sa) ? 0.0 : temp.sa), x.sa);
    temp.sb = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sb) ? 0.0 : temp.sb), x.sb);
    temp.sc = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sc) ? 0.0 : temp.sc), x.sc);
    temp.sd = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sd) ? 0.0 : temp.sd), x.sd);
    temp.se = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.se) ? 0.0 : temp.se), x.se);
    temp.sf = __builtin_spirv_OpenCL_copysign_f64_f64((__intel_relaxed_isinf(x.sf) ? 0.0 : temp.sf), x.sf);
    return temp;
}

#endif // (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
#endif // defined(cl_khr_fp64)
