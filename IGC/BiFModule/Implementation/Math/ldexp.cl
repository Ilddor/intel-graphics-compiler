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

float __builtin_spirv_OpenCL_ldexp_f32_i32( float x, int n )
{
    int delta = 0;
    float m0 = 1.0f;
    m0 = ( n < (FLT_MIN_EXP+1) ) ? as_float( ( (FLT_MIN_EXP+1) + FLOAT_BIAS ) << FLOAT_MANTISSA_BITS ) : m0;
    m0 = ( n > (FLT_MAX_EXP-1) ) ? as_float( ( (FLT_MAX_EXP-1) + FLOAT_BIAS ) << FLOAT_MANTISSA_BITS ) : m0;
    delta = ( n < (FLT_MIN_EXP+1) ) ? (FLT_MIN_EXP+1) : 0;
    delta = ( n > (FLT_MAX_EXP-1) ) ? (FLT_MAX_EXP-1) : delta;
    n -= delta;

    float m1 = 1.0f;
    m1 = ( n < (FLT_MIN_EXP+1) ) ? as_float( ( (FLT_MIN_EXP+1) + FLOAT_BIAS ) << FLOAT_MANTISSA_BITS ) : m1;
    m1 = ( n > (FLT_MAX_EXP-1) ) ? as_float( ( (FLT_MAX_EXP-1) + FLOAT_BIAS ) << FLOAT_MANTISSA_BITS ) : m1;
    delta = ( n < (FLT_MIN_EXP+1) ) ? (FLT_MIN_EXP+1) : 0;
    delta = ( n > (FLT_MAX_EXP-1) ) ? (FLT_MAX_EXP-1) : delta;
    n -= delta;

    float mn = as_float( ( n + FLOAT_BIAS ) << FLOAT_MANTISSA_BITS );
    mn = ( n == 0 ) ? 1.0f : mn;
    mn = ( n < (FLT_MIN_EXP+1) ) ? as_float( ( (FLT_MIN_EXP+1) + FLOAT_BIAS ) << FLOAT_MANTISSA_BITS ) : mn;
    mn = ( n > (FLT_MAX_EXP-1) ) ? as_float( ( (FLT_MAX_EXP-1) + FLOAT_BIAS ) << FLOAT_MANTISSA_BITS ) : mn;

    float res = x * mn * m0 * m1;

    return res;
}

GENERATE_VECTOR_FUNCTIONS_2ARGS_VV( __builtin_spirv_OpenCL_ldexp, float, float, int, f32, i32 )

#if defined(cl_khr_fp64)

double __builtin_spirv_OpenCL_ldexp_f64_i32( double x, int n )
{
    int delta = 0;
    double m0 = 1.0;
    m0 = ( n < (DBL_MIN_EXP+1) ) ? as_double( (long)( (DBL_MIN_EXP+1) + DOUBLE_BIAS ) << DOUBLE_MANTISSA_BITS ) : m0;
    m0 = ( n > (DBL_MAX_EXP-1) ) ? as_double( (long)( (DBL_MAX_EXP-1) + DOUBLE_BIAS ) << DOUBLE_MANTISSA_BITS ) : m0;
    delta = ( n < (DBL_MIN_EXP+1) ) ? (DBL_MIN_EXP+1) : 0;
    delta = ( n > (DBL_MAX_EXP-1) ) ? (DBL_MAX_EXP-1) : delta;
    n -= delta;

    double m1 = 1.0;
    m1 = ( n < (DBL_MIN_EXP+1) ) ? as_double( (long)( (DBL_MIN_EXP+1) + DOUBLE_BIAS ) << DOUBLE_MANTISSA_BITS ) : m1;
    m1 = ( n > (DBL_MAX_EXP-1) ) ? as_double( (long)( (DBL_MAX_EXP-1) + DOUBLE_BIAS ) << DOUBLE_MANTISSA_BITS ) : m1;
    delta = ( n < (DBL_MIN_EXP+1) ) ? (DBL_MIN_EXP+1) : 0;
    delta = ( n > (DBL_MAX_EXP-1) ) ? (DBL_MAX_EXP-1) : delta;
    n -= delta;

    double mn = as_double( (long)( n + DOUBLE_BIAS ) << DOUBLE_MANTISSA_BITS );
    mn = ( n == 0 ) ? 1.0 : mn;
    mn = ( n < (DBL_MIN_EXP+1) ) ? as_double( (long)( (DBL_MIN_EXP+1) + DOUBLE_BIAS ) << DOUBLE_MANTISSA_BITS ) : mn;
    mn = ( n > (DBL_MAX_EXP-1) ) ? as_double( (long)( (DBL_MAX_EXP-1) + DOUBLE_BIAS ) << DOUBLE_MANTISSA_BITS ) : mn;

    double res = x * mn * m0 * m1;

    return res;
}

GENERATE_VECTOR_FUNCTIONS_2ARGS_VV( __builtin_spirv_OpenCL_ldexp, double, double, int, f64, i32 )

#endif // defined(cl_khr_fp64)

#if defined(cl_khr_fp16)

half __builtin_spirv_OpenCL_ldexp_f16_i32( half x, int n )
{
    float mn = as_float( ( n + FLOAT_BIAS ) << FLOAT_MANTISSA_BITS );
    mn = ( n == 0 ) ? 1.0f : mn;
    mn = ( n < (FLT_MIN_EXP+1) ) ? as_float( ( (FLT_MIN_EXP+1) + FLOAT_BIAS ) << FLOAT_MANTISSA_BITS ) : mn;
    mn = ( n > (FLT_MAX_EXP-1) ) ? as_float( ( (FLT_MAX_EXP-1) + FLOAT_BIAS ) << FLOAT_MANTISSA_BITS ) : mn;

    float res = x * mn;

    return res;
}

GENERATE_VECTOR_FUNCTIONS_2ARGS_VV( __builtin_spirv_OpenCL_ldexp, half, half, int, f16, i32 )

#endif // defined(cl_khr_fp16)
