;===================== begin_copyright_notice ==================================

;Copyright (c) 2017 Intel Corporation

;Permission is hereby granted, free of charge, to any person obtaining a
;copy of this software and associated documentation files (the
;"Software"), to deal in the Software without restriction, including
;without limitation the rights to use, copy, modify, merge, publish,
;distribute, sublicense, and/or sell copies of the Software, and to
;permit persons to whom the Software is furnished to do so, subject to
;the following conditions:

;The above copyright notice and this permission notice shall be included
;in all copies or substantial portions of the Software.

;THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
;OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
;MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
;IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
;CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
;TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
;SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


;======================= end_copyright_notice ==================================
; RUN: opt -igc-replace-intrinsics -verify -S %s -o %t
; RUN: FileCheck %s --check-prefix=NOCALL < %t
; RUN: FileCheck %s < %t

; NOCALL: target datalayout
; NOCALL-NOT: call
; NOCALL: attributes
target datalayout = "e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f16:16:16-f32:32:32-f64:64:64-f80:128:128-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-a64:64:64-f80:128:128-n8:16:32:64"
target triple = "igil_32_GEN8"

define void @A(i8 * %Src, i8 * %Dst, i32 %S, <8 x i32> %r0, <8 x i32> %payloadHeader) #0 {
entry:
; CHECK-LABEL: A
; CHECK:  %pIV = alloca i32
; CHECK:  %0 = icmp ugt i32 %S, 1
; CHECK:  %umax = select i1 %0, i32 %S, i32 1
; CHECK:  store i32 0, i32* %pIV
; CHECK:  [[CMP0:%[a-zA-Z0-9]+]] = icmp ult i32 0, %umax
; CHECK:  br i1 [[CMP0]], label %memcpy.body, label %memcpy.post
; CHECK: memcpy.body:
; CHECK:   %IV = load i32* %pIV
; CHECK:   [[GEP0:%[a-zA-Z0-9]+]] = getelementptr i8* %Src, i32 %IV
; CHECK:   [[GEP1:%[a-zA-Z0-9]+]] = getelementptr i8* %Dst, i32 %IV
; CHECK:   [[LD:%[a-zA-Z0-9]+]] = load i8* [[GEP0]], align 1
; CHECK:   store i8 [[LD]], i8* [[GEP1]], align 1
; CHECK:   [[INC0:%[a-zA-Z0-9]+]] = add i32 %IV, 1
; CHECK:   store i32 [[INC0]], i32* %pIV
; CHECK:   [[CMP1:%[a-zA-Z0-9]+]] = icmp ult i32 [[INC0]], %umax
; CHECK:   br i1 [[CMP1]], label %memcpy.body, label %memcpy.post
; CHECK: memcpy.post:
; CHECK: ret
  %0 = icmp ugt i32 %S, 1
  %umax = select i1 %0, i32 %S, i32 1
  call void @llvm.memcpy.p0i8.p0i8.i32(i8 * %Dst, i8 * %Src, i32 %umax, i32 1, i1 false)
  ret void
}

define void @B(i8 * %Src, i8 * %Dst, <8 x i32> %r0, <8 x i32> %payloadHeader) #0 {
entry:
; CHECK-LABEL: B
; CHECK: ret
  call void @llvm.memcpy.p0i8.p0i8.i32(i8 * %Dst, i8 * %Src, i32 4096, i32 1, i1 false)
  ret void
}

define void @C(i8 * %Src, i8 * %Dst, <8 x i32> %r0, <8 x i32> %payloadHeader) #0 {
entry:
; CHECK-LABEL: C
; CHECK: ret
  call void @llvm.memcpy.p0i8.p0i8.i32(i8 * %Dst, i8 * %Src, i32 1, i32 1, i1 false)
  ret void
}

@.str = private unnamed_addr constant [4 x i8] c"ocl\00", align 1
@.str1 = private unnamed_addr constant [4 x i8] c"c99\00", align 1
@.str2 = private unnamed_addr constant [4 x i8] c"gcc\00", align 1
@ocl_test_kernel.args = private unnamed_addr constant [3 x i8 addrspace(2)*] [i8 addrspace(2)* bitcast ([4 x i8]* @.str to i8 addrspace(2)*), i8 addrspace(2)* bitcast ([4 x i8]* @.str1 to i8 addrspace(2)*), i8 addrspace(2)* bitcast ([4 x i8]* @.str2 to i8 addrspace(2)*)], align 4

define void @string_array(i32 addrspace(1)* %ocl_test_results, <8 x i32> %r0, <8 x i32> %payloadHeader) #0 {
entry:
; CHECK-LABEL: string_array
; CHECK:      entry:
; CHECK-NEXT:  %args = alloca [3 x i8 addrspace(2)*], align 4
; CHECK-NEXT:  %0 = bitcast [3 x i8 addrspace(2)*]* %args to i8*
; CHECK-NEXT:  %1 = bitcast [3 x i8 addrspace(2)*]* @ocl_test_kernel.args to i8*
; CHECK-NEXT:  [[memcpy_rem:%[a-zA-Z0-9_]+]] = bitcast i8* %1 to <3 x i32>*
; CHECK-NEXT:  [[memcpy_rem1:%[a-zA-Z0-9_]+]] = bitcast i8* %0 to <3 x i32>*
; CHECK-NEXT:  [[load1:%[a-zA-Z0-9_]+]] = load <3 x i32>* [[memcpy_rem]], align 4
; CHECK-NEXT:  store <3 x i32> [[load1]], <3 x i32>* [[memcpy_rem1]], align 4
; CHECK-NEXT:   %arrayidx = getelementptr inbounds i32 addrspace(1)* %ocl_test_results, i32 0
; CHECK:      ret
  %args = alloca [3 x i8 addrspace(2)*], align 4
  %0 = bitcast [3 x i8 addrspace(2)*]* %args to i8*
  %1 = bitcast [3 x i8 addrspace(2)*]* @ocl_test_kernel.args to i8*
  call void @llvm.memcpy.p0i8.p0i8.i32(i8* %0, i8* %1, i32 12, i32 4, i1 false)
  %arrayidx = getelementptr inbounds i32 addrspace(1)* %ocl_test_results, i32 0
  store i32 1, i32 addrspace(1)* %arrayidx, align 4
  %arraydecay = getelementptr inbounds [3 x i8 addrspace(2)*]* %args, i32 0, i32 0
  %arrayidx.i = getelementptr inbounds i32 addrspace(1)* %ocl_test_results, i32 1
  %2 = load i32 addrspace(1)* %arrayidx.i, align 4
  %3 = load i32 addrspace(1)* %ocl_test_results, align 4
  %arrayidx2.i = getelementptr inbounds i8 addrspace(2)** %arraydecay, i32 %3
  %4 = load i8 addrspace(2)** %arrayidx2.i, align 4
  %arrayidx3.i = getelementptr inbounds i8 addrspace(2)* %4, i32 %2
  %5 = load i8 addrspace(2)* %arrayidx3.i, align 1
  %conv.i = sext i8 %5 to i32
  store i32 %conv.i, i32 addrspace(1)* %ocl_test_results, align 4
  %arrayidx1 = getelementptr inbounds i32 addrspace(1)* %ocl_test_results, i32 3
  store i32 undef, i32 addrspace(1)* %arrayidx1, align 4
  %arrayidx2 = getelementptr inbounds i32 addrspace(1)* %ocl_test_results, i32 0
  store i32 2, i32 addrspace(1)* %arrayidx2, align 4
  ret void
}

declare void @llvm.memcpy.p0i8.p0i8.i32(i8* nocapture, i8* nocapture, i32, i32, i1) #0

attributes #0 = { alwaysinline nounwind }
attributes #1 = { nounwind }
