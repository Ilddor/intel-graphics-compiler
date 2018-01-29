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

// Arithmetic Instructions

half __builtin_spirv_OpDot_v2f16_v2f16(half2 Vector1, half2 Vector2)
{
    return __builtin_spirv_OpenCL_mad_f16_f16_f16(Vector1.x,  Vector2.x, (Vector1.y * Vector2.y));
}

half __builtin_spirv_OpDot_v3f16_v3f16(half3 Vector1, half3 Vector2)
{
    return __builtin_spirv_OpenCL_mad_f16_f16_f16(Vector1.x, Vector2.x,
           __builtin_spirv_OpenCL_mad_f16_f16_f16(Vector1.y, Vector2.y, (Vector1.z * Vector2.z)));
}

half __builtin_spirv_OpDot_v4f16_v4f16(half4 Vector1, half4 Vector2)
{
    return __builtin_spirv_OpenCL_mad_f16_f16_f16(Vector1.x, Vector2.x,
           __builtin_spirv_OpenCL_mad_f16_f16_f16(Vector1.y, Vector2.y,
           __builtin_spirv_OpenCL_mad_f16_f16_f16(Vector1.z, Vector2.z,
                                           (Vector1.w * Vector2.w))));
}

// TODO: should we support beyond vec4 which is what OCL is limited to?
#if 0
half __builtin_spirv_OpDot_v8f16_v8f16(half8 Vector1, half8 Vector2)
{
}

half __builtin_spirv_OpDot_v16f16_v16f16(half16 Vector1, half16 Vector2)
{
}
#endif

float __builtin_spirv_OpDot_v2f32_v2f32(float2 Vector1, float2 Vector2)
{
    return __builtin_spirv_OpenCL_mad_f32_f32_f32(Vector1.x,  Vector2.x, (Vector1.y * Vector2.y));
}

float __builtin_spirv_OpDot_v3f32_v3f32(float3 Vector1, float3 Vector2)
{
    return __builtin_spirv_OpenCL_mad_f32_f32_f32(Vector1.x, Vector2.x,
           __builtin_spirv_OpenCL_mad_f32_f32_f32(Vector1.y, Vector2.y, (Vector1.z * Vector2.z)));
}

float __builtin_spirv_OpDot_v4f32_v4f32(float4 Vector1, float4 Vector2)
{
    return __builtin_spirv_OpenCL_mad_f32_f32_f32(Vector1.x, Vector2.x,
           __builtin_spirv_OpenCL_mad_f32_f32_f32(Vector1.y, Vector2.y,
           __builtin_spirv_OpenCL_mad_f32_f32_f32(Vector1.z, Vector2.z,
                                           (Vector1.w * Vector2.w))));
}

#if 0
float __builtin_spirv_OpDot_v8f32_v8f32(float8 Vector1, float8 Vector2)
{
}

float __builtin_spirv_OpDot_v16f32_v16f32(float16 Vector1, float16 Vector2)
{
}
#endif

#if defined(cl_khr_fp64)

double __builtin_spirv_OpDot_v2f64_v2f64(double2 Vector1, double2 Vector2)
{
    return __builtin_spirv_OpenCL_mad_f64_f64_f64(Vector1.x,  Vector2.x, (Vector1.y * Vector2.y));
}

double __builtin_spirv_OpDot_v3f64_v3f64(double3 Vector1, double3 Vector2)
{
    return __builtin_spirv_OpenCL_mad_f64_f64_f64(Vector1.x, Vector2.x,
           __builtin_spirv_OpenCL_mad_f64_f64_f64(Vector1.y, Vector2.y, (Vector1.z * Vector2.z)));
}

double __builtin_spirv_OpDot_v4f64_v4f64(double4 Vector1, double4 Vector2)
{
    return __builtin_spirv_OpenCL_mad_f64_f64_f64(Vector1.x, Vector2.x,
           __builtin_spirv_OpenCL_mad_f64_f64_f64(Vector1.y, Vector2.y,
           __builtin_spirv_OpenCL_mad_f64_f64_f64(Vector1.z, Vector2.z,
                                           (Vector1.w * Vector2.w))));
}

#endif

#if 0

#if defined(cl_khr_fp64)
double __builtin_spirv_OpDot_v8f64_v8f64(double8 Vector1, double8 Vector2)
{
}

double __builtin_spirv_OpDot_v16f64_v16f64(double16 Vector1, double16 Vector2)
{
}
#endif

#endif

// unsigned

TwoOp_i8    __builtin_spirv_OpUMulExtended_i8_i8(uchar Operand1, uchar Operand2)
{
    return (TwoOp_i8) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_i8_i8(Operand1, Operand2) };
}

TwoOp_v2i8  __builtin_spirv_OpUMulExtended_v2i8_v2i8(uchar2 Operand1, uchar2 Operand2)
{
    return (TwoOp_v2i8) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v2i8_v2i8(Operand1, Operand2) };
}

TwoOp_v3i8  __builtin_spirv_OpUMulExtended_v3i8_v3i8(uchar3 Operand1, uchar3 Operand2)
{
    return (TwoOp_v3i8) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v3i8_v3i8(Operand1, Operand2) };
}

TwoOp_v4i8  __builtin_spirv_OpUMulExtended_v4i8_v4i8(uchar4 Operand1, uchar4 Operand2)
{
    return (TwoOp_v4i8) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v4i8_v4i8(Operand1, Operand2) };
}

TwoOp_v8i8  __builtin_spirv_OpUMulExtended_v8i8_v8i8(uchar8 Operand1, uchar8 Operand2)
{
    return (TwoOp_v8i8) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v8i8_v8i8(Operand1, Operand2) };
}

TwoOp_v16i8 __builtin_spirv_OpUMulExtended_v16i8_v16i8(uchar16 Operand1, uchar16 Operand2)
{
    return (TwoOp_v16i8) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v16i8_v16i8(Operand1, Operand2) };
}

TwoOp_i16    __builtin_spirv_OpUMulExtended_i16_i16(ushort Operand1, ushort Operand2)
{
    return (TwoOp_i16) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_i16_i16(Operand1, Operand2) };
}

TwoOp_v2i16  __builtin_spirv_OpUMulExtended_v2i16_v2i16(ushort2 Operand1, ushort2 Operand2)
{
    return (TwoOp_v2i16) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v2i16_v2i16(Operand1, Operand2) };
}

TwoOp_v3i16  __builtin_spirv_OpUMulExtended_v3i16_v3i16(ushort3 Operand1, ushort3 Operand2)
{
    return (TwoOp_v3i16) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v3i16_v3i16(Operand1, Operand2) };
}

TwoOp_v4i16  __builtin_spirv_OpUMulExtended_v4i16_v4i16(ushort4 Operand1, ushort4 Operand2)
{
    return (TwoOp_v4i16) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v4i16_v4i16(Operand1, Operand2) };
}

TwoOp_v8i16  __builtin_spirv_OpUMulExtended_v8i16_v8i16(ushort8 Operand1, ushort8 Operand2)
{
    return (TwoOp_v8i16) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v8i16_v8i16(Operand1, Operand2) };
}

TwoOp_v16i16 __builtin_spirv_OpUMulExtended_v16i16_v16i16(ushort16 Operand1, ushort16 Operand2)
{
    return (TwoOp_v16i16) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v16i16_v16i16(Operand1, Operand2) };
}

TwoOp_i32    __builtin_spirv_OpUMulExtended_i32_i32(uint Operand1, uint Operand2)
{
    return (TwoOp_i32) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_i32_i32(Operand1, Operand2) };
}

TwoOp_v2i32  __builtin_spirv_OpUMulExtended_v2i32_v2i32(uint2 Operand1, uint2 Operand2)
{
    return (TwoOp_v2i32) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v2i32_v2i32(Operand1, Operand2) };
}

TwoOp_v3i32  __builtin_spirv_OpUMulExtended_v3i32_v3i32(uint3 Operand1, uint3 Operand2)
{
    return (TwoOp_v3i32) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v3i32_v3i32(Operand1, Operand2) };
}

TwoOp_v4i32  __builtin_spirv_OpUMulExtended_v4i32_v4i32(uint4 Operand1, uint4 Operand2)
{
    return (TwoOp_v4i32) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v4i32_v4i32(Operand1, Operand2) };
}

TwoOp_v8i32  __builtin_spirv_OpUMulExtended_v8i32_v8i32(uint8 Operand1, uint8 Operand2)
{
    return (TwoOp_v8i32) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v8i32_v8i32(Operand1, Operand2) };
}

TwoOp_v16i32 __builtin_spirv_OpUMulExtended_v16i32_v16i32(uint16 Operand1, uint16 Operand2)
{
    return (TwoOp_v16i32) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v16i32_v16i32(Operand1, Operand2) };
}

TwoOp_i64    __builtin_spirv_OpUMulExtended_i64_i64(ulong Operand1, ulong Operand2)
{
    return (TwoOp_i64) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_i64_i64(Operand1, Operand2) };
}

TwoOp_v2i64  __builtin_spirv_OpUMulExtended_v2i64_v2i64(ulong2 Operand1, ulong2 Operand2)
{
    return (TwoOp_v2i64) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v2i64_v2i64(Operand1, Operand2) };
}

TwoOp_v3i64  __builtin_spirv_OpUMulExtended_v3i64_v3i64(ulong3 Operand1, ulong3 Operand2)
{
    return (TwoOp_v3i64) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v3i64_v3i64(Operand1, Operand2) };
}

TwoOp_v4i64  __builtin_spirv_OpUMulExtended_v4i64_v4i64(ulong4 Operand1, ulong4 Operand2)
{
    return (TwoOp_v4i64) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v4i64_v4i64(Operand1, Operand2) };
}

TwoOp_v8i64  __builtin_spirv_OpUMulExtended_v8i64_v8i64(ulong8 Operand1, ulong8 Operand2)
{
    return (TwoOp_v8i64) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v8i64_v8i64(Operand1, Operand2) };
}

TwoOp_v16i64 __builtin_spirv_OpUMulExtended_v16i64_v16i64(ulong16 Operand1, ulong16 Operand2)
{
    return (TwoOp_v16i64) { Operand1 * Operand2, __builtin_spirv_OpenCL_u_mul_hi_v16i64_v16i64(Operand1, Operand2) };
}

// signed

TwoOp_i8    __builtin_spirv_OpSMulExtended_i8_i8(char Operand1, char Operand2)
{
    return (TwoOp_i8) { as_uchar((char)(Operand1 * Operand2)), as_uchar(__builtin_spirv_OpenCL_s_mul_hi_i8_i8(Operand1, Operand2)) };
}

TwoOp_v2i8  __builtin_spirv_OpSMulExtended_v2i8_v2i8(char2 Operand1, char2 Operand2)
{
    return (TwoOp_v2i8) { as_uchar2(Operand1 * Operand2), as_uchar2(__builtin_spirv_OpenCL_s_mul_hi_v2i8_v2i8(Operand1, Operand2)) };
}

TwoOp_v3i8  __builtin_spirv_OpSMulExtended_v3i8_v3i8(char3 Operand1, char3 Operand2)
{
    return (TwoOp_v3i8) { as_uchar3(Operand1 * Operand2), as_uchar3(__builtin_spirv_OpenCL_s_mul_hi_v3i8_v3i8(Operand1, Operand2)) };
}

TwoOp_v4i8  __builtin_spirv_OpSMulExtended_v4i8_v4i8(char4 Operand1, char4 Operand2)
{
    return (TwoOp_v4i8) { as_uchar4(Operand1 * Operand2), as_uchar4(__builtin_spirv_OpenCL_s_mul_hi_v4i8_v4i8(Operand1, Operand2)) };
}

TwoOp_v8i8  __builtin_spirv_OpSMulExtended_v8i8_v8i8(char8 Operand1, char8 Operand2)
{
    return (TwoOp_v8i8) { as_uchar8(Operand1 * Operand2), as_uchar8(__builtin_spirv_OpenCL_s_mul_hi_v8i8_v8i8(Operand1, Operand2)) };
}

TwoOp_v16i8 __builtin_spirv_OpSMulExtended_v16i8_v16i8(char16 Operand1, char16 Operand2)
{
    return (TwoOp_v16i8) { as_uchar16(Operand1 * Operand2), as_uchar16(__builtin_spirv_OpenCL_s_mul_hi_v16i8_v16i8(Operand1, Operand2)) };
}

TwoOp_i16    __builtin_spirv_OpSMulExtended_i16_i16(short Operand1, short Operand2)
{
    return (TwoOp_i16) { as_ushort((short)(Operand1 * Operand2)), as_ushort(__builtin_spirv_OpenCL_s_mul_hi_i16_i16(Operand1, Operand2)) };
}

TwoOp_v2i16  __builtin_spirv_OpSMulExtended_v2i16_v2i16(short2 Operand1, short2 Operand2)
{
    return (TwoOp_v2i16) { as_ushort2(Operand1 * Operand2), as_ushort2(__builtin_spirv_OpenCL_s_mul_hi_v2i16_v2i16(Operand1, Operand2)) };
}

TwoOp_v3i16  __builtin_spirv_OpSMulExtended_v3i16_v3i16(short3 Operand1, short3 Operand2)
{
    return (TwoOp_v3i16) { as_ushort3(Operand1 * Operand2), as_ushort3(__builtin_spirv_OpenCL_s_mul_hi_v3i16_v3i16(Operand1, Operand2)) };
}

TwoOp_v4i16  __builtin_spirv_OpSMulExtended_v4i16_v4i16(short4 Operand1, short4 Operand2)
{
    return (TwoOp_v4i16) { as_ushort4(Operand1 * Operand2), as_ushort4(__builtin_spirv_OpenCL_s_mul_hi_v4i16_v4i16(Operand1, Operand2)) };
}

TwoOp_v8i16  __builtin_spirv_OpSMulExtended_v8i16_v8i16(short8 Operand1, short8 Operand2)
{
    return (TwoOp_v8i16) { as_ushort8(Operand1 * Operand2), as_ushort8(__builtin_spirv_OpenCL_s_mul_hi_v8i16_v8i16(Operand1, Operand2)) };
}

TwoOp_v16i16 __builtin_spirv_OpSMulExtended_v16i16_v16i16(short16 Operand1, short16 Operand2)
{
    return (TwoOp_v16i16) { as_ushort16(Operand1 * Operand2), as_ushort16(__builtin_spirv_OpenCL_s_mul_hi_v16i16_v16i16(Operand1, Operand2)) };
}

TwoOp_i32    __builtin_spirv_OpSMulExtended_i32_i32(int Operand1, int Operand2)
{
    return (TwoOp_i32) { as_uint(Operand1 * Operand2), as_uint(__builtin_spirv_OpenCL_s_mul_hi_i32_i32(Operand1, Operand2)) };
}

TwoOp_v2i32  __builtin_spirv_OpSMulExtended_v2i32_v2i32(int2 Operand1, int2 Operand2)
{
    return (TwoOp_v2i32) { as_uint2(Operand1 * Operand2), as_uint2(__builtin_spirv_OpenCL_s_mul_hi_v2i32_v2i32(Operand1, Operand2)) };
}

TwoOp_v3i32  __builtin_spirv_OpSMulExtended_v3i32_v3i32(int3 Operand1, int3 Operand2)
{
    return (TwoOp_v3i32) { as_uint3(Operand1 * Operand2), as_uint3(__builtin_spirv_OpenCL_s_mul_hi_v3i32_v3i32(Operand1, Operand2)) };
}

TwoOp_v4i32  __builtin_spirv_OpSMulExtended_v4i32_v4i32(int4 Operand1, int4 Operand2)
{
    return (TwoOp_v4i32) { as_uint4(Operand1 * Operand2), as_uint4(__builtin_spirv_OpenCL_s_mul_hi_v4i32_v4i32(Operand1, Operand2)) };
}

TwoOp_v8i32  __builtin_spirv_OpSMulExtended_v8i32_v8i32(int8 Operand1, int8 Operand2)
{
    return (TwoOp_v8i32) { as_uint8(Operand1 * Operand2), as_uint8(__builtin_spirv_OpenCL_s_mul_hi_v8i32_v8i32(Operand1, Operand2)) };
}

TwoOp_v16i32 __builtin_spirv_OpSMulExtended_v16i32_v16i32(int16 Operand1, int16 Operand2)
{
    return (TwoOp_v16i32) { as_uint16(Operand1 * Operand2), as_uint16(__builtin_spirv_OpenCL_s_mul_hi_v16i32_v16i32(Operand1, Operand2)) };
}

TwoOp_i64    __builtin_spirv_OpSMulExtended_i64_i64(long Operand1, long Operand2)
{
    return (TwoOp_i64) { as_ulong(Operand1 * Operand2), as_ulong(__builtin_spirv_OpenCL_s_mul_hi_i64_i64(Operand1, Operand2)) };
}

TwoOp_v2i64  __builtin_spirv_OpSMulExtended_v2i64_v2i64(long2 Operand1, long2 Operand2)
{
    return (TwoOp_v2i64) { as_ulong2(Operand1 * Operand2), as_ulong2(__builtin_spirv_OpenCL_s_mul_hi_v2i64_v2i64(Operand1, Operand2)) };
}

TwoOp_v3i64  __builtin_spirv_OpSMulExtended_v3i64_v3i64(long3 Operand1, long3 Operand2)
{
    return (TwoOp_v3i64) { as_ulong3(Operand1 * Operand2), as_ulong3(__builtin_spirv_OpenCL_s_mul_hi_v3i64_v3i64(Operand1, Operand2)) };
}

TwoOp_v4i64  __builtin_spirv_OpSMulExtended_v4i64_v4i64(long4 Operand1, long4 Operand2)
{
    return (TwoOp_v4i64) { as_ulong4(Operand1 * Operand2), as_ulong4(__builtin_spirv_OpenCL_s_mul_hi_v4i64_v4i64(Operand1, Operand2)) };
}

TwoOp_v8i64  __builtin_spirv_OpSMulExtended_v8i64_v8i64(long8 Operand1, long8 Operand2)
{
    return (TwoOp_v8i64) { as_ulong8(Operand1 * Operand2), as_ulong8(__builtin_spirv_OpenCL_s_mul_hi_v8i64_v8i64(Operand1, Operand2)) };
}

TwoOp_v16i64 __builtin_spirv_OpSMulExtended_v16i64_v16i64(long16 Operand1, long16 Operand2)
{
    return (TwoOp_v16i64) { as_ulong16(Operand1 * Operand2), as_ulong16(__builtin_spirv_OpenCL_s_mul_hi_v16i64_v16i64(Operand1, Operand2)) };
}

