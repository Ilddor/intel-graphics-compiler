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

#ifndef IGCBIF_INTRINSICS_CL
#define IGCBIF_INTRINSICS_CL

#pragma OPENCL EXTENSION cl_khr_fp16 : enable
#pragma OPENCL EXTENSION cl_khr_fp64 : enable

// Access of image and sampler parameters

int    __builtin_IB_get_address_mode(int) __attribute__((const));
int    __builtin_IB_is_normalized_coords(int) __attribute__((const));
int    __builtin_IB_get_image_array_size(int) __attribute__((const));
int    __builtin_IB_get_snap_wa_reqd(int) __attribute__((const));
int    __builtin_IB_get_image_height(int) __attribute__((const));
int    __builtin_IB_get_image_width(int) __attribute__((const));
int    __builtin_IB_get_image_depth(int) __attribute__((const));
int    __builtin_IB_get_image_channel_data_type(int) __attribute__((const));
int    __builtin_IB_get_image_srgb_channel_order(int) __attribute__((const));
int    __builtin_IB_get_image_channel_order(int) __attribute__((const));
int    __builtin_IB_get_image_num_samples(int) __attribute__((const));
int    __builtin_IB_get_image_num_mip_levels(int) __attribute__((const));

// Image sampling and loads
float4 __builtin_IB_OCL_1d_sample_l(int, int, float,  float);
float4 __builtin_IB_OCL_1darr_sample_l(int, int, float2,  float);
float4 __builtin_IB_OCL_2d_sample_l(int, int, float2, float);
float4 __builtin_IB_OCL_2darr_sample_l(int, int, float4, float);
float4 __builtin_IB_OCL_3d_sample_l(int, int, float4, float);

float4 __builtin_IB_OCL_1d_sample_d(int, int, float,  float, float);
float4 __builtin_IB_OCL_1darr_sample_d(int, int, float2,  float, float);
float4 __builtin_IB_OCL_2d_sample_d(int, int, float2, float2, float2);
float4 __builtin_IB_OCL_2darr_sample_d(int, int, float4, float2, float2);
float4 __builtin_IB_OCL_3d_sample_d(int, int, float4, float4, float4);

// versions that return uint for read_imageui
uint4 __builtin_IB_OCL_1d_sample_lui(int, int, float,  float);
uint4 __builtin_IB_OCL_1darr_sample_lui(int, int, float2,  float);
uint4 __builtin_IB_OCL_2d_sample_lui(int, int, float2, float);
uint4 __builtin_IB_OCL_2darr_sample_lui(int, int, float4, float);
uint4 __builtin_IB_OCL_3d_sample_lui(int, int, float4, float);

uint4 __builtin_IB_OCL_1d_sample_dui(int, int, float,  float, float);
uint4 __builtin_IB_OCL_1darr_sample_dui(int, int, float2,  float, float);
uint4 __builtin_IB_OCL_2d_sample_dui(int, int, float2, float2, float2);
uint4 __builtin_IB_OCL_2darr_sample_dui(int, int, float4, float2, float2);
uint4 __builtin_IB_OCL_3d_sample_dui(int, int, float4, float4, float4);

uint4 __builtin_IB_OCL_1d_ldui(int, int,  int);
uint4 __builtin_IB_OCL_1darr_ldui(int, int2,  int);
uint4 __builtin_IB_OCL_2d_ldui(int, int2, int);
uint4 __builtin_IB_OCL_2darr_ldui(int, int4, int);
uint4 __builtin_IB_OCL_3d_ldui(int, int4, int);

float4 __builtin_IB_OCL_1d_ld(int, int,  int);
float4 __builtin_IB_OCL_1darr_ld(int, int2,  int);
float4 __builtin_IB_OCL_2d_ld(int, int2, int);
float4 __builtin_IB_OCL_2darr_ld(int, int4, int);
float4 __builtin_IB_OCL_3d_ld(int, int4, int);

float4 __builtin_IB_OCL_2d_ldmcs(int, int2);
float4 __builtin_IB_OCL_2darr_ldmcs(int, int4);
float4 __builtin_IB_OCL_2d_ld2dms(int, int2, int, float4);
uint4  __builtin_IB_OCL_2d_ld2dmsui(int, int2, int, float4);
float4 __builtin_IB_OCL_2darr_ld2dms(int, int4, int, float4);
uint4  __builtin_IB_OCL_2darr_ld2dmsui(int, int4, int, float4);

int __builtin_IB_convert_sampler_to_int(sampler_t);

// Convert Functions for pipes and samplers
#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
__global void* __builtin_IB_convert_pipe_ro_to_intel_pipe(pipe int);
__global void* __builtin_IB_convert_pipe_wo_to_intel_pipe(write_only pipe int);
#endif

// Image writes
void     __builtin_IB_write_1darr_ui(int, int2, uint4, int);
void     __builtin_IB_write_1d_ui(int, int, uint4, int);
void     __builtin_IB_write_2darr_ui(int, int4, uint4, int);
void     __builtin_IB_write_2d_ui(int, int2, uint4, int);
void     __builtin_IB_write_3d_ui(int, int4, uint4, int);
void     __builtin_IB_write_2darr_f(int, int4, float4, int);
void     __builtin_IB_write_2d_f(int, int2, float4, int);

// Workgroup functions
local uchar* __builtin_IB_AllocLocalMemPool(uint, uint);

// Memory fences
// See GenISAIntrinsics.td for documentation
void     __builtin_IB_memfence(bool commitEnable, bool flushRW, bool flushConstant, bool flushTexture, bool flushIcache, bool isGlobal, bool invalidateL1);
void     __builtin_IB_flush_sampler_cache(void);

// Workitem functions
uint     __builtin_IB_get_work_dim(void) __attribute__((const));
uint     __builtin_IB_get_group_id(uint) __attribute__((const));
uint     __builtin_IB_get_global_offset(uint) __attribute__((const));
uint     __builtin_IB_get_local_size(uint) __attribute__((const));
uint     __builtin_IB_get_local_id_x(void) __attribute__((const));
uint     __builtin_IB_get_local_id_y(void) __attribute__((const));
uint     __builtin_IB_get_local_id_z(void) __attribute__((const));
uint     __builtin_IB_get_global_size(uint) __attribute__((const));
uint     __builtin_IB_get_num_groups(uint) __attribute__((const));
uint     __builtin_IB_get_enqueued_local_size(uint) __attribute__((const));

// Double precision conversions
half      __builtin_IB_ftoh_rtn(float) __attribute__((const));
half      __builtin_IB_ftoh_rtp(float) __attribute__((const));
half      __builtin_IB_ftoh_rtz(float)  __attribute__((const));
#if defined(cl_khr_fp64)
#endif // defined(cl_khr_fp64)

// Debug Built-In Functions
uint2     __builtin_IB_read_cycle_counter(void) __attribute__((const));
void      __builtin_IB_source_value(uint reg);
uint      __builtin_IB_set_dbg_register(uint dgb0_0);
uint      __builtin_IB_movreg(uint reg) __attribute__((const));
uint      __builtin_IB_movflag(uint flag) __attribute__((const));
uint      __builtin_IB_movcr(uint reg) __attribute__((const));
uint      __builtin_IB_hw_thread_id(void) __attribute__((const));
uint      __builtin_IB_slice_id(void) __attribute__((const));
uint      __builtin_IB_subslice_id(void) __attribute__((const));
uint      __builtin_IB_eu_id(void) __attribute__((const));
uint      __builtin_IB_eu_thread_id(void) __attribute__((const));
void      __builtin_IB_profile_snapshot(int point_type,int point_index) __attribute__((const));
void      __builtin_IB_profile_aggregated(int point_type,int point_index) __attribute__((const));

// int -> float operations
float __builtin_IB_itof_rtn(int);
float __builtin_IB_itof_rtp(int);
float __builtin_IB_itof_rtz(int);
float __builtin_IB_uitof_rtn(uint);
float __builtin_IB_uitof_rtp(uint);
float __builtin_IB_uitof_rtz(uint);

#if defined(cl_khr_fp64)
// long -> double operations
double __builtin_IB_itofp64_rtn(long);
double __builtin_IB_itofp64_rtp(long);
double __builtin_IB_itofp64_rtz(long);
double __builtin_IB_uitofp64_rtn(ulong);
double __builtin_IB_uitofp64_rtp(ulong);
double __builtin_IB_uitofp64_rtz(ulong);
#endif

// Native integer operations
uint     __builtin_IB_bfrev(uint) __attribute__((const));
char     __builtin_IB_popcount_1u8(char) __attribute__((const));
short    __builtin_IB_popcount_1u16(short) __attribute__((const));
int      __builtin_IB_popcount_1u32(int) __attribute__((const));

// Native math operations - float version
float    __builtin_IB_frnd_ne(float) __attribute__((const));
float    __builtin_IB_frnd_ni(float) __attribute__((const));
float    __builtin_IB_frnd_pi(float) __attribute__((const));
float    __builtin_IB_frnd_zi(float) __attribute__((const));
float    __builtin_IB_native_exp2f(float) __attribute__((const));
float    __builtin_IB_native_cosf(float) __attribute__((const));
float    __builtin_IB_native_log2f(float) __attribute__((const));
float    __builtin_IB_native_powrf(float, float) __attribute__((const));
float    __builtin_IB_native_sinf(float) __attribute__((const));
float    __builtin_IB_native_sqrtf(float) __attribute__((const));
float    __builtin_IB_fmax(float, float) __attribute__((const));
float    __builtin_IB_fmin(float, float) __attribute__((const));
half     __builtin_IB_HMAX(half, half) __attribute__((const));
half     __builtin_IB_HMIN(half, half) __attribute__((const));

// Native math operations - fp16 version
half     __builtin_IB_native_cosh(half) __attribute__((const));
half     __builtin_IB_native_exp2h(half) __attribute__((const));
half     __builtin_IB_native_log2h(half) __attribute__((const));
half     __builtin_IB_native_sinh(half) __attribute__((const));
half     __builtin_IB_native_sqrth(half) __attribute__((const));
half     __builtin_IB_fmah(half, half, half) __attribute__((const));

// Native math operations - fp64 version
#if defined(cl_khr_fp64)
double    __builtin_IB_native_sqrtd(double) __attribute__((const));
double    __builtin_IB_dmin(double, double) __attribute__((const));
double    __builtin_IB_dmax(double, double) __attribute__((const));
#endif

// Atomic operations
int      __builtin_IB_atomic_add_global_i32(__global int*, int);
int      __builtin_IB_atomic_add_local_i32(__local int*, int);
int      __builtin_IB_atomic_sub_global_i32(__global int*, int);
int      __builtin_IB_atomic_sub_local_i32(__local int*, int);
int      __builtin_IB_atomic_xchg_global_i32(__global int*, int);
int      __builtin_IB_atomic_xchg_local_i32(__local int*, int);
int      __builtin_IB_atomic_min_global_i32(__global int*, int);
uint     __builtin_IB_atomic_min_global_u32(__global uint*, uint);
float    __builtin_IB_atomic_min_global_f32(__global float*, float);
int      __builtin_IB_atomic_min_local_i32(__local int*, int);
uint     __builtin_IB_atomic_min_local_u32(__local uint*, uint);
float    __builtin_IB_atomic_min_local_f32(__local float*, float);
int      __builtin_IB_atomic_max_global_i32(__global int*, int);
uint     __builtin_IB_atomic_max_global_u32(__global uint*, uint);
float    __builtin_IB_atomic_max_global_f32(__global float*, float);
int      __builtin_IB_atomic_max_local_i32(__local int*, int);
uint     __builtin_IB_atomic_max_local_u32(__local uint*, uint);
float    __builtin_IB_atomic_max_local_f32(__local float*, float);
int      __builtin_IB_atomic_and_global_i32(__global int*, int);
int      __builtin_IB_atomic_and_local_i32(__local int*, int);
int      __builtin_IB_atomic_or_global_i32(__global int*, int);
int      __builtin_IB_atomic_or_local_i32(__local int*, int);
int      __builtin_IB_atomic_xor_global_i32(__global int*, int);
int      __builtin_IB_atomic_xor_local_i32(__local int*, int);
int      __builtin_IB_atomic_inc_global_i32(__global int*);
int      __builtin_IB_atomic_inc_local_i32(__local int*);
int      __builtin_IB_atomic_dec_global_i32(__global int*);
int      __builtin_IB_atomic_dec_local_i32(__local int*);
int      __builtin_IB_atomic_cmpxchg_global_i32(__global int*, int, int);
float    __builtin_IB_atomic_cmpxchg_global_f32(__global float*, float, float);
int      __builtin_IB_atomic_cmpxchg_local_i32(__local int*, int, int);
float    __builtin_IB_atomic_cmpxchg_local_f32(__local float*, float, float);


int      __builtin_IB_image_atomic_add_i32(int, int4, int);
int      __builtin_IB_image_atomic_sub_i32(int, int4, int);
int      __builtin_IB_image_atomic_xchg_i32(int, int4, int);
int      __builtin_IB_image_atomic_min_i32(int, int4, int);
uint     __builtin_IB_image_atomic_min_u32(int, int4, uint);
int      __builtin_IB_image_atomic_max_i32(int, int4, int);
uint     __builtin_IB_image_atomic_max_u32(int, int4, uint);
int      __builtin_IB_image_atomic_and_i32(int, int4, int);
int      __builtin_IB_image_atomic_or_i32(int, int4, int);
int      __builtin_IB_image_atomic_xor_i32(int, int4, int);
int      __builtin_IB_image_atomic_inc_i32(int, int4);
int      __builtin_IB_image_atomic_dec_i32(int, int4);
int      __builtin_IB_image_atomic_cmpxchg_i32(int, int4, int, int);

void __builtin_IB_memcpy_global_to_private(private uchar *dst, global uchar *src, uint size, uint align);
void __builtin_IB_memcpy_constant_to_private(private uchar *dst, constant uchar *src, uint size, uint align);
void __builtin_IB_memcpy_local_to_private(private uchar *dst, local uchar *src, uint size, uint align);
void __builtin_IB_memcpy_private_to_private(private uchar *dst, private uchar *src, uint size, uint align);
#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
void __builtin_IB_memcpy_generic_to_private(private uchar *dst, generic uchar *src, uint size, uint align);
#endif

void __builtin_IB_memcpy_private_to_global(global uchar *dst, private uchar *src, uint size, uint align);
void __builtin_IB_memcpy_private_to_constant(constant uchar *dst, private uchar *src, uint size, uint align);
void __builtin_IB_memcpy_private_to_local(local uchar *dst, private uchar *src, uint size, uint align);
void __builtin_IB_memcpy_private_to_private(private uchar *dst, private uchar *src, uint size, uint align);
#if (__OPENCL_C_VERSION__ >= CL_VERSION_2_0)
void __builtin_IB_memcpy_private_to_generic(generic uchar *dst, private uchar *src, uint size, uint align);
#endif

// Correctly rounded sqrt and division
float   __builtin_IB_ieee_sqrt(float) __attribute__((const));
float   __builtin_IB_ieee_divide(float, float) __attribute__((const));

#if defined(cl_khr_fp64)
double   __builtin_IB_ieee_divide_f64(double, double) __attribute__((const));
#endif

// SIMD information
ushort __builtin_IB_simd_lane_id() __attribute__((const));

// an opaque handle pointing to a blob of registers.
typedef uint GRFHandle;

// legacy message phase builtins for old vme (not device side)
void __builtin_IB_set_message_phase_legacy_dw(uint messagePhases, uint phaseIndex, uint dwIndex, uint val);
void __builtin_IB_set_message_phase_legacy_uw(uint messagePhases, uint phaseIndex, uint dwIndex, ushort val);
void __builtin_IB_set_message_phase_legacy_ub(uint messagePhases, uint phaseIndex, uint dwIndex, uchar val);

void __builtin_IB_set_message_phase_legacy(uint messagePhases, uint phaseIndex, uint val);

// Message Phases manipulation
uint __builtin_IB_create_message_phases(uint numPhases);
uint2 __builtin_IB_create_message_phases_uint2(uint numPhases);
uint4 __builtin_IB_create_message_phases_uint4(uint numPhases);
uint8 __builtin_IB_create_message_phases_uint8(uint numPhases);

uint __builtin_IB_create_message_phases_no_init(uint numPhases);
uint2 __builtin_IB_create_message_phases_no_init_uint2(uint numPhases);
uint4 __builtin_IB_create_message_phases_no_init_uint4(uint numPhases);
uint8 __builtin_IB_create_message_phases_no_init_uint8(uint numPhases);

uint __builtin_IB_get_message_phase_dw(uint messagePhases, uint phaseIndex, uint dwIndex);
uint __builtin_IB_get_message_phase_dw_uint2(uint2 messagePhases, uint phaseIndex, uint dwIndex);
uint __builtin_IB_get_message_phase_dw_uint4(uint4 messagePhases, uint phaseIndex, uint dwIndex);
uint __builtin_IB_get_message_phase_dw_uint8(uint8 messagePhases, uint phaseIndex, uint dwIndex);

ulong __builtin_IB_get_message_phase_uq(uint messagePhases, uint phaseIndex, uint dwIndex);
ulong __builtin_IB_get_message_phase_uq_uint2(uint2 messagePhases, uint phaseIndex, uint dwIndex);
ulong __builtin_IB_get_message_phase_uq_uint4(uint4 messagePhases, uint phaseIndex, uint dwIndex);
ulong __builtin_IB_get_message_phase_uq_uint8(uint8 messagePhases, uint phaseIndex, uint dwIndex);

uint __builtin_IB_set_message_phase_dw(uint messagePhases, uint phaseIndex, uint dwIndex, uint val);
uint2 __builtin_IB_set_message_phase_dw_uint2(uint2 messagePhases, uint phaseIndex, uint dwIndex, uint val);
uint4 __builtin_IB_set_message_phase_dw_uint4(uint4 messagePhases, uint phaseIndex, uint dwIndex, uint val);
uint8 __builtin_IB_set_message_phase_dw_uint8(uint8 messagePhases, uint phaseIndex, uint dwIndex, uint val);

uint __builtin_IB_get_message_phase(uint messagePhases, uint phaseIndex);
uint __builtin_IB_get_message_phase_uint2(uint2 messagePhases, uint phaseIndex);
uint __builtin_IB_get_message_phase_uint4(uint4 messagePhases, uint phaseIndex);
uint __builtin_IB_get_message_phase_uint8(uint8 messagePhases, uint phaseIndex);

uint __builtin_IB_set_message_phase(uint messagePhases, uint phaseIndex, uint val);
uint2 __builtin_IB_set_message_phase_uint2(uint2 messagePhases, uint phaseIndex, uint val);
uint4 __builtin_IB_set_message_phase_uint4(uint4 messagePhases, uint phaseIndex, uint val);
uint8 __builtin_IB_set_message_phase_uint8(uint8 messagePhases, uint phaseIndex, uint val);

ushort __builtin_IB_get_message_phase_uw(uint messagePhases, uint phaseIndex, uint wIndex);
ushort __builtin_IB_get_message_phase_uw_uint2(uint2 messagePhases, uint phaseIndex, uint wIndex);
ushort __builtin_IB_get_message_phase_uw_uint4(uint4 messagePhases, uint phaseIndex, uint wIndex);
ushort __builtin_IB_get_message_phase_uw_uint8(uint8 messagePhases, uint phaseIndex, uint wIndex);

uint __builtin_IB_set_message_phase_uw(uint messagePhases, uint phaseIndex, uint dwIndex, ushort val);
uint2 __builtin_IB_set_message_phase_uw_uint2(uint2 messagePhases, uint phaseIndex, uint dwIndex, ushort val);
uint4 __builtin_IB_set_message_phase_uw_uint4(uint4 messagePhases, uint phaseIndex, uint dwIndex, ushort val);
uint8 __builtin_IB_set_message_phase_uw_uint8(uint8 messagePhases, uint phaseIndex, uint dwIndex, ushort val);

uchar __builtin_IB_get_message_phase_ub(uint messagePhases, uint phaseIndex, uint dwIndex);
uchar __builtin_IB_get_message_phase_ub_uint2(uint2 messagePhases, uint phaseIndex, uint dwIndex);
uchar __builtin_IB_get_message_phase_ub_uint4(uint4 messagePhases, uint phaseIndex, uint dwIndex);
uchar __builtin_IB_get_message_phase_ub_uint8(uint8 messagePhases, uint phaseIndex, uint dwIndex);

uint __builtin_IB_set_message_phase_ub(uint messagePhases, uint phaseIndex, uint dwIndex, uchar val);
uint2 __builtin_IB_set_message_phase_ub_uint2(uint2 messagePhases, uint phaseIndex, uint dwIndex, uchar val);
uint4 __builtin_IB_set_message_phase_ub_uint4(uint4 messagePhases, uint phaseIndex, uint dwIndex, uchar val);
uint8 __builtin_IB_set_message_phase_ub_uint8(uint8 messagePhases, uint phaseIndex, uint dwIndex, uchar val);

// Broadcast a phase value to all work-items in a sub-group
uchar  __builtin_IB_broadcast_message_phase_ub(uint messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
uchar  __builtin_IB_broadcast_message_phase_ub_uint2(uint2 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
uchar  __builtin_IB_broadcast_message_phase_ub_uint4(uint4 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
uchar  __builtin_IB_broadcast_message_phase_ub_uint8(uint8 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);

ushort __builtin_IB_broadcast_message_phase_uw(uint messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
ushort __builtin_IB_broadcast_message_phase_uw_uint2(uint2 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
ushort __builtin_IB_broadcast_message_phase_uw_uint4(uint4 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
ushort __builtin_IB_broadcast_message_phase_uw_uint8(uint8 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);

uint   __builtin_IB_broadcast_message_phase_dw(uint messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
uint   __builtin_IB_broadcast_message_phase_dw_uint2(uint2 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
uint   __builtin_IB_broadcast_message_phase_dw_uint4(uint4 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
uint   __builtin_IB_broadcast_message_phase_dw_uint8(uint8 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);

ulong  __builtin_IB_broadcast_message_phase_uq(uint messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
ulong  __builtin_IB_broadcast_message_phase_uq_uint2(uint2 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
ulong  __builtin_IB_broadcast_message_phase_uq_uint4(uint4 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);
ulong  __builtin_IB_broadcast_message_phase_uq_uint8(uint8 messagePhases, uint phaseIndex, uint phaseSubindex, uint width);

// Copy the value phase(s) to all work-items in a sub-group
ushort __builtin_IB_simd_get_message_phase_uw(uint messagePhases, uint phaseIndex, uint numPhases);
ushort __builtin_IB_simd_get_message_phase_uw_uint2(uint2 messagePhases, uint phaseIndex, uint numPhases);
ushort __builtin_IB_simd_get_message_phase_uw_uint4(uint4 messagePhases, uint phaseIndex, uint numPhases);
ushort __builtin_IB_simd_get_message_phase_uw_uint8(uint8 messagePhases, uint phaseIndex, uint numPhases);

ulong  __builtin_IB_simd_get_message_phase_uq(uint messagePhases, uint phaseIndex, uint numPhases);
ulong  __builtin_IB_simd_get_message_phase_uq_uint2(uint2 messagePhases, uint phaseIndex, uint numPhases);
ulong  __builtin_IB_simd_get_message_phase_uq_uint4(uint4 messagePhases, uint phaseIndex, uint numPhases);
ulong  __builtin_IB_simd_get_message_phase_uq_uint8(uint8 messagePhases, uint phaseIndex, uint numPhases);

uint __builtin_IB_simd_set_message_phase_ub(uint messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, uchar val);
uint2 __builtin_IB_simd_set_message_phase_ub_uint2(uint2 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, uchar val);
uint4 __builtin_IB_simd_set_message_phase_ub_uint4(uint4 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, uchar val);
uint8 __builtin_IB_simd_set_message_phase_ub_uint8(uint8 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, uchar val);

uint __builtin_IB_simd_set_message_phase_uw(uint messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, ushort val);
uint2 __builtin_IB_simd_set_message_phase_uw_uint2(uint2 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, ushort val);
uint4 __builtin_IB_simd_set_message_phase_uw_uint4(uint4 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, ushort val);
uint8 __builtin_IB_simd_set_message_phase_uw_uint8(uint8 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, ushort val);

uint __builtin_IB_simd_set_message_phase_dw(uint messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, uint val);
uint2 __builtin_IB_simd_set_message_phase_dw_uint2(uint2 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, uint val);
uint4 __builtin_IB_simd_set_message_phase_dw_uint4(uint4 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, uint val);
uint8 __builtin_IB_simd_set_message_phase_dw_uint8(uint8 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, uint val);

uint __builtin_IB_simd_set_message_phase_uq(uint messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, ulong val);
uint2 __builtin_IB_simd_set_message_phase_uq_uint2(uint2 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, ulong val);
uint4 __builtin_IB_simd_set_message_phase_uq_uint4(uint4 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, ulong val);
uint8 __builtin_IB_simd_set_message_phase_uq_uint8(uint8 messagePhases, uint phaseIndex, uint numPhases, uint subReg, uint numLanes, ulong val);

void __builtin_IB_simdMediaRegionCopy(GRFHandle dst, uint dbyteoffset, uint dstride, uint dnumelem,
                                      GRFHandle src, uint sbyteoffset, uint vstride, uint width, uint hstride, uint typesize, uint execsize, uint snumelem);

void __builtin_IB_extract_mv_and_sad(GRFHandle MVMin, GRFHandle SADMin, GRFHandle result, uint blockType);
void __builtin_IB_cmp_sads(GRFHandle MVCurr, GRFHandle SADCurr, GRFHandle MVMin, GRFHandle SADMin);

// VME
uint __builtin_IB_vme_mb_block_type() __attribute__((const));
uint __builtin_IB_vme_subpixel_mode() __attribute__((const));
uint __builtin_IB_vme_sad_adjust_mode() __attribute__((const));
uint __builtin_IB_vme_search_path_type() __attribute__((const));
void __builtin_IB_vme_send_ime(GRFHandle res, GRFHandle universalInputMsg, GRFHandle imeMsg, __read_only image2d_t srcImg, __read_only image2d_t refImg, uint ref0Coord, uint ref1Coord, uint costCenter);
void __builtin_IB_vme_send_fbr(GRFHandle res, GRFHandle universalInputMsg, GRFHandle fbrMsg, __read_only image2d_t srcImg, __read_only image2d_t refImg, uint interMbMode, uint subMbShape, uint subMbPredMode);
void __builtin_IB_vme_send_sic(GRFHandle res, GRFHandle universalInputMsg, GRFHandle sicMsg, __read_only image2d_t srcImg, __read_only image2d_t refImg0, __read_only image2d_t refImg1);

uint4 __builtin_IB_vme_send_ime_new_uint4_uint8(uint8 inputMsg, __read_only image2d_t srcImg, __read_only image2d_t fwdRefImg, __read_only image2d_t bwdRefImg, sampler_t accelerator, uint streamMode);
uint8 __builtin_IB_vme_send_ime_new_uint8_uint8(uint8 inputMsg, __read_only image2d_t srcImg, __read_only image2d_t fwdRefImg, __read_only image2d_t bwdRefImg, sampler_t accelerator, uint streamMode);
uint4 __builtin_IB_vme_send_ime_new_uint4_uint4(uint4 inputMsg, __read_only image2d_t srcImg, __read_only image2d_t fwdRefImg, __read_only image2d_t bwdRefImg, sampler_t accelerator, uint streamMode);
uint8 __builtin_IB_vme_send_ime_new_uint8_uint4(uint4 inputMsg, __read_only image2d_t srcImg, __read_only image2d_t fwdRefImg, __read_only image2d_t bwdRefImg, sampler_t accelerator, uint streamMode);

uint4 __builtin_IB_vme_send_fbr_new(uint4 inputMsg, __read_only image2d_t srcImg, __read_only image2d_t fwdRefImg, __read_only image2d_t bwdRefImg, sampler_t accelerator);
uint4 __builtin_IB_vme_send_sic_new(uint4 inputMsg, __read_only image2d_t srcImg, __read_only image2d_t fwdRefImg, __read_only image2d_t bwdRefImg, sampler_t accelerator);

uint  __builtin_IB_get_image_bti(uint img);

// ballot intrinsic
uint __builtin_IB_WaveBallot(bool p);

// VA
void   __builtin_IB_va_erode_64x4( __local uchar* dst, float2 coords, int srcImgId, int i_accelerator );
void   __builtin_IB_va_dilate_64x4( __local uchar* dst, float2 coords, int srcImgId, int i_accelerator );
void   __builtin_IB_va_minmaxfilter_16x4_SLM( __local uchar* dst, float2 coords, int srcImgId, int i_accelerator );
void   __builtin_IB_va_convolve_16x4_SLM( __local uchar* dst, float2 coords, int srcImgId, int i_accelerator );
void   __builtin_IB_va_minmax( __local uchar* dst, float2 coords, int srcImgId, int i_accelerator );
void   __builtin_IB_va_centroid( __local uchar* dst, float2 coords, int2 size, int srcImgId, int i_accelerator );
void   __builtin_IB_va_boolcentroid( __local uchar* dst, float2 coords, int2 size, int srcImgId, int i_accelerator );
void   __builtin_IB_va_boolsum( __local uchar* dst, float2 coords, int2 size, int srcImgId, int i_accelerator );
short4 __builtin_IB_va_convolve_16x4( float2 coords, int srcImgId, int i_accelerator );

// Device Enqueue
__global void* __builtin_IB_get_default_device_queue();
__global void* __builtin_IB_get_event_pool();
uint __builtin_IB_get_max_workgroup_size();
uint __builtin_IB_get_parent_event();
uint __builtin_IB_get_prefered_workgroup_multiple();


// Generic Address Space
__local   void* __builtin_IB_to_local(void*);
__private void* __builtin_IB_to_private(void*);

// SubGroup Functions

int     __builtin_IB_get_simd_size( void );
int     __builtin_IB_get_simd_id( void );
int     __builtin_IB_simd_shuffle( int, int );
float 	__builtin_IB_simd_shuffle_f( float, uint );
half 	__builtin_IB_simd_shuffle_h( half, uint );
uint    __builtin_IB_simd_shuffle_down( uint, uint, uint );
ushort  __builtin_IB_simd_shuffle_down_us( ushort, ushort, uint );

uint    __builtin_IB_simd_block_read_1_global( const __global uint* );
uint2   __builtin_IB_simd_block_read_2_global( const __global uint* );
uint4   __builtin_IB_simd_block_read_4_global( const __global uint* );
uint8   __builtin_IB_simd_block_read_8_global( const __global uint* );

ushort    __builtin_IB_simd_block_read_1_global_h( const __global ushort* );
ushort2   __builtin_IB_simd_block_read_2_global_h( const __global ushort* );
ushort4   __builtin_IB_simd_block_read_4_global_h( const __global ushort* );
ushort8   __builtin_IB_simd_block_read_8_global_h( const __global ushort* );

void    __builtin_IB_simd_block_write_1_global( __global uint*, uint );
void    __builtin_IB_simd_block_write_2_global( __global uint*, uint2 );
void    __builtin_IB_simd_block_write_4_global( __global uint*, uint4 );
void    __builtin_IB_simd_block_write_8_global( __global uint*, uint8 );

void    __builtin_IB_simd_block_write_1_global_h( __global ushort*, ushort );
void    __builtin_IB_simd_block_write_2_global_h( __global ushort*, ushort2 );
void    __builtin_IB_simd_block_write_4_global_h( __global ushort*, ushort4 );
void    __builtin_IB_simd_block_write_8_global_h( __global ushort*, ushort8 );

uint    __builtin_IB_simd_media_block_read_1( int, int2 );
uint2   __builtin_IB_simd_media_block_read_2( int, int2 );
uint4   __builtin_IB_simd_media_block_read_4( int, int2 );
uint8   __builtin_IB_simd_media_block_read_8( int, int2 );

ushort   __builtin_IB_simd_media_block_read_1_h( int, int2 );
ushort2  __builtin_IB_simd_media_block_read_2_h( int, int2 );
ushort4  __builtin_IB_simd_media_block_read_4_h( int, int2 );
ushort8  __builtin_IB_simd_media_block_read_8_h( int, int2 );

void    __builtin_IB_media_block_rectangle_read( read_only image2d_t image, int2 coords, int blockWidth, int blockHeight, GRFHandle destination );

void    __builtin_IB_simd_media_block_write_1( int, int2, uint );
void    __builtin_IB_simd_media_block_write_2( int, int2, uint2 );
void    __builtin_IB_simd_media_block_write_4( int, int2, uint4 );
void    __builtin_IB_simd_media_block_write_8( int, int2, uint8 );

void    __builtin_IB_simd_media_block_write_1_h( int, int2, ushort );
void    __builtin_IB_simd_media_block_write_2_h( int, int2, ushort2 );
void    __builtin_IB_simd_media_block_write_4_h( int, int2, ushort4 );
void    __builtin_IB_simd_media_block_write_8_h( int, int2, ushort8 );

uchar   __builtin_IB_media_block_read_uchar(int image, int2 offset, int width, int height);
uchar2  __builtin_IB_media_block_read_uchar2(int image, int2 offset, int width, int height);
uchar4  __builtin_IB_media_block_read_uchar4(int image, int2 offset, int width, int height);
uchar8  __builtin_IB_media_block_read_uchar8(int image, int2 offset, int width, int height);
uchar16 __builtin_IB_media_block_read_uchar16(int image, int2 offset, int width, int height);

ushort   __builtin_IB_media_block_read_ushort(int image, int2 offset, int width, int height);
ushort2  __builtin_IB_media_block_read_ushort2(int image, int2 offset, int width, int height);
ushort4  __builtin_IB_media_block_read_ushort4(int image, int2 offset, int width, int height);
ushort8  __builtin_IB_media_block_read_ushort8(int image, int2 offset, int width, int height);
ushort16 __builtin_IB_media_block_read_ushort16(int image, int2 offset, int width, int height);

uint   __builtin_IB_media_block_read_uint(int image, int2 offset, int width, int height);
uint2  __builtin_IB_media_block_read_uint2(int image, int2 offset, int width, int height);
uint4  __builtin_IB_media_block_read_uint4(int image, int2 offset, int width, int height);
uint8  __builtin_IB_media_block_read_uint8(int image, int2 offset, int width, int height);

void __builtin_IB_media_block_write_uchar(int image, int2 offset, int width, int height, uchar pixels);
void __builtin_IB_media_block_write_uchar2(int image, int2 offset, int width, int height, uchar2 pixels);
void __builtin_IB_media_block_write_uchar4(int image, int2 offset, int width, int height, uchar4 pixels);
void __builtin_IB_media_block_write_uchar8(int image, int2 offset, int width, int height, uchar8 pixels);
void __builtin_IB_media_block_write_uchar16(int image, int2 offset, int width, int height, uchar16 pixels);

void __builtin_IB_media_block_write_ushort(int image, int2 offset, int width, int height, ushort pixels);
void __builtin_IB_media_block_write_ushort2(int image, int2 offset, int width, int height, ushort2 pixels);
void __builtin_IB_media_block_write_ushort4(int image, int2 offset, int width, int height, ushort4 pixels);
void __builtin_IB_media_block_write_ushort8(int image, int2 offset, int width, int height, ushort8 pixels);
void __builtin_IB_media_block_write_ushort16(int image, int2 offset, int width, int height, ushort16 pixels);

void __builtin_IB_media_block_write_uint(int image, int2 offset, int width, int height, uint pixels);
void __builtin_IB_media_block_write_uint2(int image, int2 offset, int width, int height, uint2 pixels);
void __builtin_IB_media_block_write_uint4(int image, int2 offset, int width, int height, uint4 pixels);
void __builtin_IB_media_block_write_uint8(int image, int2 offset, int width, int height, uint8 pixels);


short __builtin_IB_sub_group_reduce_OpGroupIAdd_i16(short x) __attribute__((const));
short __builtin_IB_sub_group_reduce_OpGroupSMax_i16(short x) __attribute__((const));
ushort __builtin_IB_sub_group_reduce_OpGroupUMax_i16(ushort x) __attribute__((const));
short __builtin_IB_sub_group_reduce_OpGroupSMin_i16(short x) __attribute__((const));
ushort __builtin_IB_sub_group_reduce_OpGroupUMin_i16(ushort x) __attribute__((const));
int __builtin_IB_sub_group_reduce_OpGroupIAdd_i32(int x) __attribute__((const));
int __builtin_IB_sub_group_reduce_OpGroupSMax_i32(int x) __attribute__((const));
uint __builtin_IB_sub_group_reduce_OpGroupUMax_i32(uint x) __attribute__((const));
int __builtin_IB_sub_group_reduce_OpGroupSMin_i32(int x) __attribute__((const));
uint __builtin_IB_sub_group_reduce_OpGroupUMin_i32(uint x) __attribute__((const));
long __builtin_IB_sub_group_reduce_OpGroupIAdd_i64(long x) __attribute__((const));
long __builtin_IB_sub_group_reduce_OpGroupSMax_i64(long x) __attribute__((const));
ulong __builtin_IB_sub_group_reduce_OpGroupUMax_i64(ulong x) __attribute__((const));
long __builtin_IB_sub_group_reduce_OpGroupSMin_i64(long x) __attribute__((const));
ulong __builtin_IB_sub_group_reduce_OpGroupUMin_i64(ulong x) __attribute__((const));
half __builtin_IB_sub_group_reduce_OpGroupFAdd_f16(half x) __attribute__((const));
half __builtin_IB_sub_group_reduce_OpGroupFMax_f16(half x) __attribute__((const));
half __builtin_IB_sub_group_reduce_OpGroupFMin_f16(half x) __attribute__((const));
float __builtin_IB_sub_group_reduce_OpGroupFAdd_f32(float x) __attribute__((const));
float __builtin_IB_sub_group_reduce_OpGroupFMax_f32(float x) __attribute__((const));
float __builtin_IB_sub_group_reduce_OpGroupFMin_f32(float x) __attribute__((const));
double __builtin_IB_sub_group_reduce_OpGroupFAdd_f64(double x) __attribute__((const));
double __builtin_IB_sub_group_reduce_OpGroupFMax_f64(double x) __attribute__((const));
double __builtin_IB_sub_group_reduce_OpGroupFMin_f64(double x) __attribute__((const));

// inclusive scan
short __builtin_IB_sub_group_scan_OpGroupIAdd_i16(short x) __attribute__((const));
short __builtin_IB_sub_group_scan_OpGroupSMax_i16(short x) __attribute__((const));
ushort __builtin_IB_sub_group_scan_OpGroupUMax_i16(ushort x) __attribute__((const));
short __builtin_IB_sub_group_scan_OpGroupSMin_i16(short x) __attribute__((const));
ushort __builtin_IB_sub_group_scan_OpGroupUMin_i16(ushort x) __attribute__((const));
int __builtin_IB_sub_group_scan_OpGroupIAdd_i32(int x) __attribute__((const));
int __builtin_IB_sub_group_scan_OpGroupSMax_i32(int x) __attribute__((const));
uint __builtin_IB_sub_group_scan_OpGroupUMax_i32(uint x) __attribute__((const));
int __builtin_IB_sub_group_scan_OpGroupSMin_i32(int x) __attribute__((const));
uint __builtin_IB_sub_group_scan_OpGroupUMin_i32(uint x) __attribute__((const));
long __builtin_IB_sub_group_scan_OpGroupIAdd_i64(long x) __attribute__((const));
long __builtin_IB_sub_group_scan_OpGroupSMax_i64(long x) __attribute__((const));
ulong __builtin_IB_sub_group_scan_OpGroupUMax_i64(ulong x) __attribute__((const));
long __builtin_IB_sub_group_scan_OpGroupSMin_i64(long x) __attribute__((const));
ulong __builtin_IB_sub_group_scan_OpGroupUMin_i64(ulong x) __attribute__((const));
half __builtin_IB_sub_group_scan_OpGroupFAdd_f16(half x) __attribute__((const));
half __builtin_IB_sub_group_scan_OpGroupFMax_f16(half x) __attribute__((const));
half __builtin_IB_sub_group_scan_OpGroupFMin_f16(half x) __attribute__((const));
float __builtin_IB_sub_group_scan_OpGroupFAdd_f32(float x) __attribute__((const));
float __builtin_IB_sub_group_scan_OpGroupFMax_f32(float x) __attribute__((const));
float __builtin_IB_sub_group_scan_OpGroupFMin_f32(float x) __attribute__((const));
double __builtin_IB_sub_group_scan_OpGroupFAdd_f64(double x) __attribute__((const));
double __builtin_IB_sub_group_scan_OpGroupFMax_f64(double x) __attribute__((const));
double __builtin_IB_sub_group_scan_OpGroupFMin_f64(double x) __attribute__((const));


#endif // IGCBIF_INTRINSICS_CL
