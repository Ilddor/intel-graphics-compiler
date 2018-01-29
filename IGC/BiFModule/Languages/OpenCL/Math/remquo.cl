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
#include "spirv.h"

float OVERLOADABLE remquo( float         xx,
                           float         yy,
                           __global int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f32_f32_p1i32( xx, yy, quo );
}

INLINE float2 OVERLOADABLE remquo( float2         xx,
                            float2         yy,
                            __global int2* quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f32_v2f32_p1v2i32( xx, yy, quo );
}

INLINE float3 OVERLOADABLE remquo( float3         xx,
                            float3         yy,
                            __global int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f32_v3f32_p1v3i32( xx, yy, quo );
}

INLINE float4 OVERLOADABLE remquo( float4         xx,
                            float4         yy,
                            __global int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f32_v4f32_p1v4i32( xx, yy, quo );
}

INLINE float8 OVERLOADABLE remquo( float8         xx,
                            float8         yy,
                            __global int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f32_v8f32_p1v8i32( xx, yy, quo );
}

INLINE float16 OVERLOADABLE remquo( float16         xx,
                             float16         yy,
                             __global int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f32_v16f32_p1v16i32( xx, yy, quo );
}

float OVERLOADABLE remquo( float          xx,
                           float          yy,
                           __private int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f32_f32_p0i32( xx, yy, quo );
}

INLINE float2 OVERLOADABLE remquo( float2          xx,
                            float2          yy,
                            __private int2* quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f32_v2f32_p0v2i32( xx, yy, quo );
}

INLINE float3 OVERLOADABLE remquo( float3          xx,
                            float3          yy,
                            __private int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f32_v3f32_p0v3i32( xx, yy, quo );
}

INLINE float4 OVERLOADABLE remquo( float4          xx,
                            float4          yy,
                            __private int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f32_v4f32_p0v4i32( xx, yy, quo );
}

INLINE float8 OVERLOADABLE remquo( float8          xx,
                            float8          yy,
                            __private int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f32_v8f32_p0v8i32( xx, yy, quo );
}

INLINE float16 OVERLOADABLE remquo( float16          xx,
                             float16          yy,
                             __private int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f32_v16f32_p0v16i32( xx, yy, quo );
}

float OVERLOADABLE remquo( float        xx,
                           float        yy,
                           __local int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f32_f32_p3i32( xx, yy, quo );
}

INLINE float2 OVERLOADABLE remquo( float2        xx,
                            float2        yy,
                            __local int2* quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f32_v2f32_p3v2i32( xx, yy, quo );
}

INLINE float3 OVERLOADABLE remquo( float3        xx,
                            float3        yy,
                            __local int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f32_v3f32_p3v3i32( xx, yy, quo );
}

INLINE float4 OVERLOADABLE remquo( float4        xx,
                            float4        yy,
                            __local int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f32_v4f32_p3v4i32( xx, yy, quo );
}

INLINE float8 OVERLOADABLE remquo( float8        xx,
                            float8        yy,
                            __local int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f32_v8f32_p3v8i32( xx, yy, quo );
}

INLINE float16 OVERLOADABLE remquo( float16        xx,
                             float16        yy,
                             __local int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f32_v16f32_p3v16i32( xx, yy, quo );
}

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

INLINE float OVERLOADABLE remquo( float          xx,
                           float          yy,
                           __generic int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f32_f32_p4i32( xx, yy, quo );
}

INLINE float2 OVERLOADABLE remquo( float2          xx,
                            float2          yy,
                            __generic int2* quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f32_v2f32_p4v2i32( xx, yy, quo );
}

INLINE float3 OVERLOADABLE remquo( float3          xx,
                            float3          yy,
                            __generic int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f32_v3f32_p4v3i32( xx, yy, quo );
}

INLINE float4 OVERLOADABLE remquo( float4          xx,
                            float4          yy,
                            __generic int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f32_v4f32_p4v4i32( xx, yy, quo );
}

INLINE float8 OVERLOADABLE remquo( float8          xx,
                            float8          yy,
                            __generic int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f32_v8f32_p4v8i32( xx, yy, quo );
}

INLINE float16 OVERLOADABLE remquo( float16          xx,
                             float16          yy,
                             __generic int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f32_v16f32_p4v16i32( xx, yy, quo );
}

#endif //#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

#ifdef cl_khr_fp16
INLINE half OVERLOADABLE remquo( half          xx,
                          half          yy,
                          __global int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f16_f16_p1i32( xx, yy, quo );
}

INLINE half2 OVERLOADABLE remquo( half2          xx,
                           half2          yy,
                           __global int2* quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f16_v2f16_p1v2i32( xx, yy, quo );
}

INLINE half3 OVERLOADABLE remquo( half3          xx,
                           half3          yy,
                           __global int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f16_v3f16_p1v3i32( xx, yy, quo );
}

INLINE half4 OVERLOADABLE remquo( half4          xx,
                           half4          yy,
                           __global int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f16_v4f16_p1v4i32( xx, yy, quo );
}

INLINE half8 OVERLOADABLE remquo( half8          xx,
                           half8          yy,
                           __global int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f16_v8f16_p1v8i32( xx, yy, quo );
}

INLINE half16 OVERLOADABLE remquo( half16          xx,
                            half16          yy,
                            __global int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f16_v16f16_p1v16i32( xx, yy, quo );
}

INLINE half OVERLOADABLE remquo( half           xx,
                          half           yy,
                          __private int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f16_f16_p0i32( xx, yy, quo );
}

INLINE half2 OVERLOADABLE remquo( half2           xx,
                           half2           yy,
                           __private int2* quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f16_v2f16_p0v2i32( xx, yy, quo );
}

INLINE half3 OVERLOADABLE remquo( half3           xx,
                           half3           yy,
                           __private int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f16_v3f16_p0v3i32( xx, yy, quo );
}

INLINE half4 OVERLOADABLE remquo( half4           xx,
                           half4           yy,
                           __private int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f16_v4f16_p0v4i32( xx, yy, quo );
}

INLINE half8 OVERLOADABLE remquo( half8           xx,
                           half8           yy,
                           __private int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f16_v8f16_p0v8i32( xx, yy, quo );
}

INLINE half16 OVERLOADABLE remquo( half16           xx,
                            half16           yy,
                            __private int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f16_v16f16_p0v16i32( xx, yy, quo );
}

INLINE half OVERLOADABLE remquo( half         xx,
                          half         yy,
                          __local int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f16_f16_p3i32( xx, yy, quo );
}

INLINE half2 OVERLOADABLE remquo( half2         xx,
                           half2         yy,
                           __local int2* quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f16_v2f16_p3v2i32( xx, yy, quo );
}

INLINE half3 OVERLOADABLE remquo( half3         xx,
                           half3         yy,
                           __local int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f16_v3f16_p3v3i32( xx, yy, quo );
}

INLINE half4 OVERLOADABLE remquo( half4         xx,
                           half4         yy,
                           __local int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f16_v4f16_p3v4i32( xx, yy, quo );
}

INLINE half8 OVERLOADABLE remquo( half8         xx,
                           half8         yy,
                           __local int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f16_v8f16_p3v8i32( xx, yy, quo );
}

INLINE half16 OVERLOADABLE remquo( half16         xx,
                            half16         yy,
                            __local int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f16_v16f16_p3v16i32( xx, yy, quo );
}
#endif


#if defined(cl_khr_fp16) && (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

INLINE half OVERLOADABLE remquo( half           xx,
                          half           yy,
                          __generic int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f16_f16_p4i32( xx, yy, quo );
}

INLINE half2 OVERLOADABLE remquo( half2           xx,
                           half2           yy,
                           __generic int2* quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f16_v2f16_p4v2i32( xx, yy, quo );
}

INLINE half3 OVERLOADABLE remquo( half3           xx,
                           half3           yy,
                           __generic int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f16_v3f16_p4v3i32( xx, yy, quo );
}

INLINE half4 OVERLOADABLE remquo( half4           xx,
                           half4           yy,
                           __generic int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f16_v4f16_p4v4i32( xx, yy, quo );
}

INLINE half8 OVERLOADABLE remquo( half8           xx,
                           half8           yy,
                           __generic int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f16_v8f16_p4v8i32( xx, yy, quo );
}

INLINE half16 OVERLOADABLE remquo( half16           xx,
                            half16           yy,
                            __generic int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f16_v16f16_p4v16i32( xx, yy, quo );
}
#endif //#if defined(cl_khr_fp16) && (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

#if defined(cl_khr_fp64)

double OVERLOADABLE remquo( double        xx,
                            double        yy,
                            __global int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f64_f64_p1i32( xx, yy, quo );
}

double2 OVERLOADABLE remquo( double2         xx,
                             double2         yy,
                             __global int2*  quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f64_v2f64_p1v2i32( xx, yy, quo );
}

double3 OVERLOADABLE remquo( double3        xx,
                             double3        yy,
                             __global int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f64_v3f64_p1v3i32( xx, yy, quo );
}

double4 OVERLOADABLE remquo( double4        xx,
                             double4        yy,
                             __global int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f64_v4f64_p1v4i32( xx, yy, quo );
}

double8 OVERLOADABLE remquo( double8        xx,
                             double8        yy,
                             __global int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f64_v8f64_p1v8i32( xx, yy, quo );
}

double16 OVERLOADABLE remquo( double16        xx,
                              double16        yy,
                              __global int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f64_v16f64_p1v16i32( xx, yy, quo );
}

double OVERLOADABLE remquo( double         xx,
                            double         yy,
                            __private int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f64_f64_p0i32( xx, yy, quo );
}

double2 OVERLOADABLE remquo( double2         xx,
                             double2         yy,
                             __private int2* quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f64_v2f64_p0v2i32( xx, yy, quo );
}

double3 OVERLOADABLE remquo( double3         xx,
                             double3         yy,
                             __private int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f64_v3f64_p0v3i32( xx, yy, quo );
}

double4 OVERLOADABLE remquo( double4         xx,
                             double4         yy,
                             __private int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f64_v4f64_p0v4i32( xx, yy, quo );
}

double8 OVERLOADABLE remquo( double8         xx,
                             double8         yy,
                             __private int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f64_v8f64_p0v8i32( xx, yy, quo );
}

double16 OVERLOADABLE remquo( double16         xx,
                              double16         yy,
                              __private int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f64_v16f64_p0v16i32( xx, yy, quo );
}

double OVERLOADABLE remquo( double       xx,
                            double       yy,
                            __local int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f64_f64_p3i32( xx, yy, quo );
}

double2 OVERLOADABLE remquo( double2       xx,
                             double2       yy,
                             __local int2* quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f64_v2f64_p3v2i32( xx, yy, quo );
}

double3 OVERLOADABLE remquo( double3       xx,
                             double3       yy,
                             __local int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f64_v3f64_p3v3i32( xx, yy, quo );
}

double4 OVERLOADABLE remquo( double4       xx,
                             double4       yy,
                             __local int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f64_v4f64_p3v4i32( xx, yy, quo );
}

double8 OVERLOADABLE remquo( double8       xx,
                             double8       yy,
                             __local int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f64_v8f64_p3v8i32( xx, yy, quo );
}

double16 OVERLOADABLE remquo( double16       xx,
                              double16       yy,
                              __local int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f64_v16f64_p3v16i32( xx, yy, quo );
}

#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

double OVERLOADABLE remquo( double         xx,
                            double         yy,
                            __generic int* quo )
{
    return __builtin_spirv_OpenCL_remquo_f64_f64_p4i32( xx, yy, quo );
}

double2 OVERLOADABLE remquo( double2         xx,
                             double2         yy,
                             __generic int2* quo )
{
    return __builtin_spirv_OpenCL_remquo_v2f64_v2f64_p4v2i32( xx, yy, quo );
}

double3 OVERLOADABLE remquo( double3         xx,
                             double3         yy,
                             __generic int3* quo )
{
    return __builtin_spirv_OpenCL_remquo_v3f64_v3f64_p4v3i32( xx, yy, quo );
}

double4 OVERLOADABLE remquo( double4         xx,
                             double4         yy,
                             __generic int4* quo )
{
    return __builtin_spirv_OpenCL_remquo_v4f64_v4f64_p4v4i32( xx, yy, quo );
}

double8 OVERLOADABLE remquo( double8         xx,
                             double8         yy,
                             __generic int8* quo )
{
    return __builtin_spirv_OpenCL_remquo_v8f64_v8f64_p4v8i32( xx, yy, quo );
}

double16 OVERLOADABLE remquo( double16         xx,
                              double16         yy,
                              __generic int16* quo )
{
    return __builtin_spirv_OpenCL_remquo_v16f64_v16f64_p4v16i32( xx, yy, quo );
}

#endif // (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)

#endif // defined(cl_khr_fp64)
