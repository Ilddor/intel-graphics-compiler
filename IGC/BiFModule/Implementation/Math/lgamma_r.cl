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

INLINE float __builtin_spirv_OpenCL_lgamma_r_f32_p1i32( float         x,
                                          __global int* signp )
{
    int     s;
    float   r = __builtin_spirv_OpenCL_lgamma_r_f32_p0i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float2 __builtin_spirv_OpenCL_lgamma_r_v2f32_p1v2i32( float2         x,
                                               __global int2* signp )
{
    int2    s;
    float2  r = __builtin_spirv_OpenCL_lgamma_r_v2f32_p0v2i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float3 __builtin_spirv_OpenCL_lgamma_r_v3f32_p1v3i32( float3         x,
                                               __global int3* signp )
{
    int3    s;
    float3  r = __builtin_spirv_OpenCL_lgamma_r_v3f32_p0v3i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float4 __builtin_spirv_OpenCL_lgamma_r_v4f32_p1v4i32( float4         x,
                                               __global int4* signp )
{
    int4    s;
    float4  r = __builtin_spirv_OpenCL_lgamma_r_v4f32_p0v4i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float8 __builtin_spirv_OpenCL_lgamma_r_v8f32_p1v8i32( float8         x,
                                               __global int8* signp )
{
    int8    s;
    float8  r = __builtin_spirv_OpenCL_lgamma_r_v8f32_p0v8i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float16 __builtin_spirv_OpenCL_lgamma_r_v16f32_p1v16i32( float16         x,
                                                  __global int16* signp )
{
    int16   s;
    float16 r = __builtin_spirv_OpenCL_lgamma_r_v16f32_p0v16i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float __builtin_spirv_OpenCL_lgamma_r_f32_p3i32( float        x,
                                          __local int* signp )
{
    int     s;
    float   r = __builtin_spirv_OpenCL_lgamma_r_f32_p0i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float2 __builtin_spirv_OpenCL_lgamma_r_v2f32_p3v2i32( float2        x,
                                               __local int2* signp )
{
    int2    s;
    float2  r = __builtin_spirv_OpenCL_lgamma_r_v2f32_p0v2i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float3 __builtin_spirv_OpenCL_lgamma_r_v3f32_p3v3i32( float3        x,
                                               __local int3* signp )
{
    int3    s;
    float3  r = __builtin_spirv_OpenCL_lgamma_r_v3f32_p0v3i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float4 __builtin_spirv_OpenCL_lgamma_r_v4f32_p3v4i32( float4        x,
                                               __local int4* signp )
{
    int4    s;
    float4  r = __builtin_spirv_OpenCL_lgamma_r_v4f32_p0v4i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float8 __builtin_spirv_OpenCL_lgamma_r_v8f32_p3v8i32( float8        x,
                                               __local int8* signp )
{
    int8    s;
    float8  r = __builtin_spirv_OpenCL_lgamma_r_v8f32_p0v8i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float16 __builtin_spirv_OpenCL_lgamma_r_v16f32_p3v16i32( float16        x,
                                                  __local int16* signp )
{
    int16   s;
    float16 r = __builtin_spirv_OpenCL_lgamma_r_v16f32_p0v16i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float __builtin_spirv_OpenCL_lgamma_r_f32_p0i32( float          x,
                                          __private int* signp )
{
    int     s;
    float   r;
    if( __intel_relaxed_isnan(x) )
    {
        r = __builtin_spirv_OpenCL_nan_i32(0U);
        s = 0;
    }
    else
    {
        float g = __builtin_spirv_OpenCL_tgamma_f32(x);
        r = __intel_relaxed_isnan(g) ? INFINITY : __builtin_spirv_OpenCL_native_log_f32(__builtin_spirv_OpenCL_fabs_f32(g));
        s = __builtin_spirv_OpenCL_sign_f32(g);
    }
    signp[0] = s;
    return r;
}

INLINE float2 __builtin_spirv_OpenCL_lgamma_r_v2f32_p0v2i32( float2          x,
                                               __private int2* signp )
{
    float2  r;
    const __private float* px = (const __private float*)&x;
    __private int*      sign_scalar = (__private int*)signp;
    __private float*    r_scalar = (__private float*)&r;
    for(uint i = 0; i < 2; i++)
    {
        r_scalar[i] = __builtin_spirv_OpenCL_lgamma_r_f32_p0i32( px[i], sign_scalar + i );
    }
    return r;
}

INLINE float3 __builtin_spirv_OpenCL_lgamma_r_v3f32_p0v3i32( float3          x,
                                               __private int3* signp )
{
    float3  r;
    const __private float* px = (const __private float*)&x;
    __private int*      sign_scalar = (__private int*)signp;
    __private float*    r_scalar = (__private float*)&r;
    for(uint i = 0; i < 3; i++)
    {
        r_scalar[i] = __builtin_spirv_OpenCL_lgamma_r_f32_p0i32( px[i], sign_scalar + i );
    }
    return r;
}

INLINE float4 __builtin_spirv_OpenCL_lgamma_r_v4f32_p0v4i32( float4          x,
                                               __private int4* signp )
{
    float4  r;
    const __private float* px = (const __private float*)&x;
    __private int*      sign_scalar = (__private int*)signp;
    __private float*    r_scalar = (__private float*)&r;
    for(uint i = 0; i < 4; i++)
    {
        r_scalar[i] = __builtin_spirv_OpenCL_lgamma_r_f32_p0i32( px[i], sign_scalar + i );
    }
    return r;
}

INLINE float8 __builtin_spirv_OpenCL_lgamma_r_v8f32_p0v8i32( float8          x,
                                               __private int8* signp )
{
    float8  r;
    const __private float* px = (const __private float*)&x;
    __private int*      sign_scalar = (__private int*)signp;
    __private float*    r_scalar = (__private float*)&r;
    for(uint i = 0; i < 8; i++)
    {
        r_scalar[i] = __builtin_spirv_OpenCL_lgamma_r_f32_p0i32( px[i], sign_scalar + i );
    }
    return r;
}

INLINE float16 __builtin_spirv_OpenCL_lgamma_r_v16f32_p0v16i32( float16          x,
                                                  __private int16* signp )
{
    float16 r;
    const __private float* px = (const __private float*)&x;
    __private int*      sign_scalar = (__private int*)signp;
    __private float*    r_scalar = (__private float*)&r;
    for(uint i = 0; i < 16; i++)
    {
        r_scalar[i] = __builtin_spirv_OpenCL_lgamma_r_f32_p0i32( px[i], sign_scalar + i );
    }
    return r;
}

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

INLINE float __builtin_spirv_OpenCL_lgamma_r_f32_p4i32( float          x,
                                          __generic int* signp )
{
    int     s;
    float   r = __builtin_spirv_OpenCL_lgamma_r_f32_p0i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float2 __builtin_spirv_OpenCL_lgamma_r_v2f32_p4v2i32( float2          x,
                                               __generic int2* signp )
{
    int2    s;
    float2  r = __builtin_spirv_OpenCL_lgamma_r_v2f32_p0v2i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float3 __builtin_spirv_OpenCL_lgamma_r_v3f32_p4v3i32( float3          x,
                                               __generic int3* signp )
{
    int3    s;
    float3  r = __builtin_spirv_OpenCL_lgamma_r_v3f32_p0v3i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float4 __builtin_spirv_OpenCL_lgamma_r_v4f32_p4v4i32( float4          x,
                                               __generic int4* signp )
{
    int4    s;
    float4  r = __builtin_spirv_OpenCL_lgamma_r_v4f32_p0v4i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float8 __builtin_spirv_OpenCL_lgamma_r_v8f32_p4v8i32( float8          x,
                                               __generic int8* signp )
{
    int8    s;
    float8  r = __builtin_spirv_OpenCL_lgamma_r_v8f32_p0v8i32(x, &s);
    signp[0] = s;
    return r;
}

INLINE float16 __builtin_spirv_OpenCL_lgamma_r_v16f32_p4v16i32( float16          x,
                                                  __generic int16* signp )
{
    int16   s;
    float16 r = __builtin_spirv_OpenCL_lgamma_r_v16f32_p0v16i32(x, &s);
    signp[0] = s;
    return r;
}

#endif //#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

#if defined(cl_khr_fp16)

INLINE half __builtin_spirv_OpenCL_lgamma_r_f16_p1i32( half          x,
                                         __global int* signp )
{
    return __builtin_spirv_OpFConvert_f16_f32( __builtin_spirv_OpenCL_lgamma_r_f32_p1i32( __builtin_spirv_OpFConvert_f32_f16(x), signp ) );
}

INLINE half2 __builtin_spirv_OpenCL_lgamma_r_v2f16_p1v2i32( half2          x,
                                              __global int2* signp )
{
    return __builtin_spirv_OpFConvert_v2f16_v2f32( __builtin_spirv_OpenCL_lgamma_r_v2f32_p1v2i32( __builtin_spirv_OpFConvert_v2f32_v2f16(x), signp ) );
}

INLINE half3 __builtin_spirv_OpenCL_lgamma_r_v3f16_p1v3i32( half3          x,
                                              __global int3* signp )
{
    return __builtin_spirv_OpFConvert_v3f16_v3f32( __builtin_spirv_OpenCL_lgamma_r_v3f32_p1v3i32( __builtin_spirv_OpFConvert_v3f32_v3f16(x), signp ) );
}

INLINE half4 __builtin_spirv_OpenCL_lgamma_r_v4f16_p1v4i32( half4          x,
                                              __global int4* signp )
{
    return __builtin_spirv_OpFConvert_v4f16_v4f32( __builtin_spirv_OpenCL_lgamma_r_v4f32_p1v4i32( __builtin_spirv_OpFConvert_v4f32_v4f16(x), signp ) );
}

INLINE half8 __builtin_spirv_OpenCL_lgamma_r_v8f16_p1v8i32( half8          x,
                                              __global int8* signp )
{
    return __builtin_spirv_OpFConvert_v8f16_v8f32( __builtin_spirv_OpenCL_lgamma_r_v8f32_p1v8i32( __builtin_spirv_OpFConvert_v8f32_v8f16(x), signp ) );
}

INLINE half16 __builtin_spirv_OpenCL_lgamma_r_v16f16_p1v16i32( half16          x,
                                                 __global int16* signp )
{
    return __builtin_spirv_OpFConvert_v16f16_v16f32( __builtin_spirv_OpenCL_lgamma_r_v16f32_p1v16i32( __builtin_spirv_OpFConvert_v16f32_v16f16(x), signp ) );
}

INLINE half __builtin_spirv_OpenCL_lgamma_r_f16_p0i32( half           x,
                                         __private int* signp )
{
    return __builtin_spirv_OpFConvert_f16_f32( __builtin_spirv_OpenCL_lgamma_r_f32_p0i32( __builtin_spirv_OpFConvert_f32_f16(x), signp ) );
}

INLINE half2 __builtin_spirv_OpenCL_lgamma_r_v2f16_p0v2i32( half2           x,
                                              __private int2* signp )
{
    return __builtin_spirv_OpFConvert_v2f16_v2f32( __builtin_spirv_OpenCL_lgamma_r_v2f32_p0v2i32( __builtin_spirv_OpFConvert_v2f32_v2f16(x), signp ) );
}

INLINE half3 __builtin_spirv_OpenCL_lgamma_r_v3f16_p0v3i32( half3           x,
                                              __private int3* signp )
{
    return __builtin_spirv_OpFConvert_v3f16_v3f32( __builtin_spirv_OpenCL_lgamma_r_v3f32_p0v3i32( __builtin_spirv_OpFConvert_v3f32_v3f16(x), signp ) );
}

INLINE half4 __builtin_spirv_OpenCL_lgamma_r_v4f16_p0v4i32( half4           x,
                                              __private int4* signp )
{
    return __builtin_spirv_OpFConvert_v4f16_v4f32( __builtin_spirv_OpenCL_lgamma_r_v4f32_p0v4i32( __builtin_spirv_OpFConvert_v4f32_v4f16(x), signp ) );
}

INLINE half8 __builtin_spirv_OpenCL_lgamma_r_v8f16_p0v8i32( half8           x,
                                              __private int8* signp )
{
    return __builtin_spirv_OpFConvert_v8f16_v8f32( __builtin_spirv_OpenCL_lgamma_r_v8f32_p0v8i32( __builtin_spirv_OpFConvert_v8f32_v8f16(x), signp ) );
}

INLINE half16 __builtin_spirv_OpenCL_lgamma_r_v16f16_p0v16i32( half16           x,
                                                 __private int16* signp )
{
    return __builtin_spirv_OpFConvert_v16f16_v16f32( __builtin_spirv_OpenCL_lgamma_r_v16f32_p0v16i32( __builtin_spirv_OpFConvert_v16f32_v16f16(x), signp ) );
}

INLINE half __builtin_spirv_OpenCL_lgamma_r_f16_p3i32( half         x,
                                         __local int* signp )
{
    return __builtin_spirv_OpFConvert_f16_f32( __builtin_spirv_OpenCL_lgamma_r_f32_p3i32( __builtin_spirv_OpFConvert_f32_f16(x), signp ) );
}

INLINE half2 __builtin_spirv_OpenCL_lgamma_r_v2f16_p3v2i32( half2         x,
                                              __local int2* signp )
{
    return __builtin_spirv_OpFConvert_v2f16_v2f32( __builtin_spirv_OpenCL_lgamma_r_v2f32_p3v2i32( __builtin_spirv_OpFConvert_v2f32_v2f16(x), signp ) );
}

INLINE half3 __builtin_spirv_OpenCL_lgamma_r_v3f16_p3v3i32( half3         x,
                                              __local int3* signp )
{
    return __builtin_spirv_OpFConvert_v3f16_v3f32( __builtin_spirv_OpenCL_lgamma_r_v3f32_p3v3i32( __builtin_spirv_OpFConvert_v3f32_v3f16(x), signp ) );
}

INLINE half4 __builtin_spirv_OpenCL_lgamma_r_v4f16_p3v4i32( half4         x,
                                              __local int4* signp )
{
    return __builtin_spirv_OpFConvert_v4f16_v4f32( __builtin_spirv_OpenCL_lgamma_r_v4f32_p3v4i32( __builtin_spirv_OpFConvert_v4f32_v4f16(x), signp ) );
}

INLINE half8 __builtin_spirv_OpenCL_lgamma_r_v8f16_p3v8i32( half8         x,
                                              __local int8* signp )
{
    return __builtin_spirv_OpFConvert_v8f16_v8f32( __builtin_spirv_OpenCL_lgamma_r_v8f32_p3v8i32( __builtin_spirv_OpFConvert_v8f32_v8f16(x), signp ) );
}

INLINE half16 __builtin_spirv_OpenCL_lgamma_r_v16f16_p3v16i32( half16         x,
                                                 __local int16* signp )
{
    return __builtin_spirv_OpFConvert_v16f16_v16f32( __builtin_spirv_OpenCL_lgamma_r_v16f32_p3v16i32( __builtin_spirv_OpFConvert_v16f32_v16f16(x), signp ) );
}

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

INLINE half __builtin_spirv_OpenCL_lgamma_r_f16_p4i32( half           x,
                                         __generic int* signp )
{
    return __builtin_spirv_OpFConvert_f16_f32( __builtin_spirv_OpenCL_lgamma_r_f32_p4i32( __builtin_spirv_OpFConvert_f32_f16(x), signp ) );
}

INLINE half2 __builtin_spirv_OpenCL_lgamma_r_v2f16_p4v2i32( half2           x,
                                              __generic int2* signp )
{
    return __builtin_spirv_OpFConvert_v2f16_v2f32( __builtin_spirv_OpenCL_lgamma_r_v2f32_p4v2i32( __builtin_spirv_OpFConvert_v2f32_v2f16(x), signp ) );
}

INLINE half3 __builtin_spirv_OpenCL_lgamma_r_v3f16_p4v3i32( half3           x,
                                              __generic int3* signp )
{
    return __builtin_spirv_OpFConvert_v3f16_v3f32( __builtin_spirv_OpenCL_lgamma_r_v3f32_p4v3i32( __builtin_spirv_OpFConvert_v3f32_v3f16(x), signp ) );
}

INLINE half4 __builtin_spirv_OpenCL_lgamma_r_v4f16_p4v4i32( half4           x,
                                              __generic int4* signp )
{
    return __builtin_spirv_OpFConvert_v4f16_v4f32( __builtin_spirv_OpenCL_lgamma_r_v4f32_p4v4i32( __builtin_spirv_OpFConvert_v4f32_v4f16(x), signp ) );
}

INLINE half8 __builtin_spirv_OpenCL_lgamma_r_v8f16_p4v8i32( half8           x,
                                              __generic int8* signp )
{
    return __builtin_spirv_OpFConvert_v8f16_v8f32( __builtin_spirv_OpenCL_lgamma_r_v8f32_p4v8i32( __builtin_spirv_OpFConvert_v8f32_v8f16(x), signp ) );
}

INLINE half16 __builtin_spirv_OpenCL_lgamma_r_v16f16_p4v16i32( half16           x,
                                                 __generic int16* signp )
{
    return __builtin_spirv_OpFConvert_v16f16_v16f32( __builtin_spirv_OpenCL_lgamma_r_v16f32_p4v16i32( __builtin_spirv_OpFConvert_v16f32_v16f16(x), signp ) );
}

#endif // (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

#endif // (cl_khr_fp16)

#if defined(cl_khr_fp64)

double __builtin_spirv_OpenCL_lgamma_r_f64_p1i32( double        x,
                                           __global int* signp )
{
    return __builtin_spirv_OpFConvert_f64_f32( __builtin_spirv_OpenCL_lgamma_r_f32_p1i32( __builtin_spirv_OpFConvert_f32_f64(x), signp ) );
}

double2 __builtin_spirv_OpenCL_lgamma_r_v2f64_p1v2i32( double2        x,
                                                __global int2* signp )
{
    return __builtin_spirv_OpFConvert_v2f64_v2f32( __builtin_spirv_OpenCL_lgamma_r_v2f32_p1v2i32( __builtin_spirv_OpFConvert_v2f32_v2f64(x), signp ) );
}

double3 __builtin_spirv_OpenCL_lgamma_r_v3f64_p1v3i32( double3        x,
                                                __global int3* signp )
{
    return __builtin_spirv_OpFConvert_v3f64_v3f32( __builtin_spirv_OpenCL_lgamma_r_v3f32_p1v3i32( __builtin_spirv_OpFConvert_v3f32_v3f64(x), signp ) );
}

double4 __builtin_spirv_OpenCL_lgamma_r_v4f64_p1v4i32( double4        x,
                                                __global int4* signp )
{
    return __builtin_spirv_OpFConvert_v4f64_v4f32( __builtin_spirv_OpenCL_lgamma_r_v4f32_p1v4i32( __builtin_spirv_OpFConvert_v4f32_v4f64(x), signp ) );
}

double8 __builtin_spirv_OpenCL_lgamma_r_v8f64_p1v8i32( double8        x,
                                                __global int8* signp )
{
    return __builtin_spirv_OpFConvert_v8f64_v8f32( __builtin_spirv_OpenCL_lgamma_r_v8f32_p1v8i32( __builtin_spirv_OpFConvert_v8f32_v8f64(x), signp ) );
}

double16 __builtin_spirv_OpenCL_lgamma_r_v16f64_p1v16i32( double16        x,
                                                   __global int16* signp )
{
    return __builtin_spirv_OpFConvert_v16f64_v16f32( __builtin_spirv_OpenCL_lgamma_r_v16f32_p1v16i32( __builtin_spirv_OpFConvert_v16f32_v16f64(x), signp ) );
}

double __builtin_spirv_OpenCL_lgamma_r_f64_p0i32( double         x,
                                           __private int* signp )
{
    return __builtin_spirv_OpFConvert_f64_f32( __builtin_spirv_OpenCL_lgamma_r_f32_p0i32( __builtin_spirv_OpFConvert_f32_f64(x), signp ) );
}

double2 __builtin_spirv_OpenCL_lgamma_r_v2f64_p0v2i32( double2         x,
                                                __private int2* signp )
{
    return __builtin_spirv_OpFConvert_v2f64_v2f32( __builtin_spirv_OpenCL_lgamma_r_v2f32_p0v2i32( __builtin_spirv_OpFConvert_v2f32_v2f64(x), signp ) );
}

double3 __builtin_spirv_OpenCL_lgamma_r_v3f64_p0v3i32( double3         x,
                                                __private int3* signp )
{
    return __builtin_spirv_OpFConvert_v3f64_v3f32( __builtin_spirv_OpenCL_lgamma_r_v3f32_p0v3i32( __builtin_spirv_OpFConvert_v3f32_v3f64(x), signp ) );
}

double4 __builtin_spirv_OpenCL_lgamma_r_v4f64_p0v4i32( double4         x,
                                                __private int4* signp )
{
    return __builtin_spirv_OpFConvert_v4f64_v4f32( __builtin_spirv_OpenCL_lgamma_r_v4f32_p0v4i32( __builtin_spirv_OpFConvert_v4f32_v4f64(x), signp ) );
}

double8 __builtin_spirv_OpenCL_lgamma_r_v8f64_p0v8i32( double8         x,
                                                __private int8* signp )
{
    return __builtin_spirv_OpFConvert_v8f64_v8f32( __builtin_spirv_OpenCL_lgamma_r_v8f32_p0v8i32( __builtin_spirv_OpFConvert_v8f32_v8f64(x), signp ) );
}

double16 __builtin_spirv_OpenCL_lgamma_r_v16f64_p0v16i32( double16         x,
                                                   __private int16* signp )
{
    return __builtin_spirv_OpFConvert_v16f64_v16f32( __builtin_spirv_OpenCL_lgamma_r_v16f32_p0v16i32( __builtin_spirv_OpFConvert_v16f32_v16f64(x), signp ) );
}

double __builtin_spirv_OpenCL_lgamma_r_f64_p3i32( double       x,
                                           __local int* signp )
{
    return __builtin_spirv_OpFConvert_f64_f32( __builtin_spirv_OpenCL_lgamma_r_f32_p3i32( __builtin_spirv_OpFConvert_f32_f64(x), signp ) );
}

double2 __builtin_spirv_OpenCL_lgamma_r_v2f64_p3v2i32( double2       x,
                                                __local int2* signp )
{
    return __builtin_spirv_OpFConvert_v2f64_v2f32( __builtin_spirv_OpenCL_lgamma_r_v2f32_p3v2i32( __builtin_spirv_OpFConvert_v2f32_v2f64(x), signp ) );
}

double3 __builtin_spirv_OpenCL_lgamma_r_v3f64_p3v3i32( double3       x,
                                                __local int3* signp )
{
    return __builtin_spirv_OpFConvert_v3f64_v3f32( __builtin_spirv_OpenCL_lgamma_r_v3f32_p3v3i32( __builtin_spirv_OpFConvert_v3f32_v3f64(x), signp ) );
}

double4 __builtin_spirv_OpenCL_lgamma_r_v4f64_p3v4i32( double4       x,
                                                __local int4* signp )
{
    return __builtin_spirv_OpFConvert_v4f64_v4f32( __builtin_spirv_OpenCL_lgamma_r_v4f32_p3v4i32( __builtin_spirv_OpFConvert_v4f32_v4f64(x), signp ) );
}

double8 __builtin_spirv_OpenCL_lgamma_r_v8f64_p3v8i32( double8       x,
                                                __local int8* signp )
{
    return __builtin_spirv_OpFConvert_v8f64_v8f32( __builtin_spirv_OpenCL_lgamma_r_v8f32_p3v8i32( __builtin_spirv_OpFConvert_v8f32_v8f64(x), signp ) );
}

double16 __builtin_spirv_OpenCL_lgamma_r_v16f64_p3v16i32( double16       x,
                                                   __local int16* signp )
{
    return __builtin_spirv_OpFConvert_v16f64_v16f32( __builtin_spirv_OpenCL_lgamma_r_v16f32_p3v16i32( __builtin_spirv_OpFConvert_v16f32_v16f64(x), signp ) );
}

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

double __builtin_spirv_OpenCL_lgamma_r_f64_p4i32( double         x,
                                           __generic int* signp )
{
    return __builtin_spirv_OpFConvert_f64_f32( __builtin_spirv_OpenCL_lgamma_r_f32_p4i32( __builtin_spirv_OpFConvert_f32_f64(x), signp ) );
}

double2 __builtin_spirv_OpenCL_lgamma_r_v2f64_p4v2i32( double2         x,
                                                __generic int2* signp )
{
    return __builtin_spirv_OpFConvert_v2f64_v2f32( __builtin_spirv_OpenCL_lgamma_r_v2f32_p4v2i32( __builtin_spirv_OpFConvert_v2f32_v2f64(x), signp ) );
}

double3 __builtin_spirv_OpenCL_lgamma_r_v3f64_p4v3i32( double3         x,
                                                __generic int3* signp )
{
    return __builtin_spirv_OpFConvert_v3f64_v3f32( __builtin_spirv_OpenCL_lgamma_r_v3f32_p4v3i32( __builtin_spirv_OpFConvert_v3f32_v3f64(x), signp ) );
}

double4 __builtin_spirv_OpenCL_lgamma_r_v4f64_p4v4i32( double4         x,
                                                __generic int4* signp )
{
    return __builtin_spirv_OpFConvert_v4f64_v4f32( __builtin_spirv_OpenCL_lgamma_r_v4f32_p4v4i32( __builtin_spirv_OpFConvert_v4f32_v4f64(x), signp ) );
}

double8 __builtin_spirv_OpenCL_lgamma_r_v8f64_p4v8i32( double8         x,
                                                __generic int8* signp )
{
    return __builtin_spirv_OpFConvert_v8f64_v8f32( __builtin_spirv_OpenCL_lgamma_r_v8f32_p4v8i32( __builtin_spirv_OpFConvert_v8f32_v8f64(x), signp ) );
}

double16 __builtin_spirv_OpenCL_lgamma_r_v16f64_p4v16i32( double16         x,
                                                   __generic int16* signp )
{
    return __builtin_spirv_OpFConvert_v16f64_v16f32( __builtin_spirv_OpenCL_lgamma_r_v16f32_p4v16i32( __builtin_spirv_OpFConvert_v16f32_v16f64(x), signp ) );
}

#endif // (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
#endif // defined(cl_khr_fp64)
