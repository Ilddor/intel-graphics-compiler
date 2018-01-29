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
#include "../include/exp_for_hyper.cl"

#if defined(cl_khr_fp64)

    #include "../ExternalLibraries/libclc/doubles.cl"

#endif // defined(cl_khr_fp64)

float __builtin_spirv_OpenCL_tanh_f32( float x )
{
    float result;

    if( __intel_relaxed_isnan(x) )
    {
        result = __builtin_spirv_OpenCL_nan_i32((uint)0);
    }
    else if(__builtin_spirv_OpenCL_fabs_f32(x) < as_float(0x3A71E7A0))     // 0.00092279352247715
    {
        result = x;
    }
    else if( __builtin_spirv_OpenCL_fabs_f32(x) < as_float(0x3EACB527) )   // 0.33731958270072937
    {
        float sinhx, coshx;
        {
            float x2 = x * x;
            float x3 = x * x2;
            float x5 = x3 * x2;
            sinhx = (as_float(0x3C088889) * x5) + (as_float(0x3E2AAAAB) * x3) + x;
        }
        {
            float pexp = __intel_exp_for_tanh( x, -2.0f);
            float nexp = __intel_exp_for_tanh(-x, -2.0f);
            coshx = 2.0f * ( pexp + nexp );
        }
        result = sinhx / coshx;
    }
    else if (__builtin_spirv_OpenCL_fabs_f32(x) < as_float(0x41987E0C))    // 19.061546325683594
    {
        float exp2x = __intel_exp_for_hyper(2 * x, 0.0f);
        result = (exp2x - 1) / (exp2x + 1);
    }
    else
    {
        result = (x > 0) ? 1.0f : -1.0f;
    }

    return result;
}

GENERATE_VECTOR_FUNCTIONS_1ARG_LOOP( __builtin_spirv_OpenCL_tanh, float, float, f32 )

#if defined(cl_khr_fp64)

INLINE double __builtin_spirv_OpenCL_tanh_f64( double x )
{
        return libclc_tanh_f64(x);
}

GENERATE_VECTOR_FUNCTIONS_1ARG_LOOP( __builtin_spirv_OpenCL_tanh, double, double, f64 )

#endif // defined(cl_khr_fp64)

#if defined(cl_khr_fp16)

INLINE half __builtin_spirv_OpenCL_tanh_f16( half x )
{
    return __builtin_spirv_OpenCL_tanh_f32((float)x);
}

GENERATE_VECTOR_FUNCTIONS_1ARG_LOOP( __builtin_spirv_OpenCL_tanh, half, half, f16 )

#endif // defined(cl_khr_fp16)
