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

define void @A(i8 addrspace(1)* %B, i32 %S, <8 x i32> %r0, <8 x i32> %payloadHeader) #0 {
entry:
; CHECK-LABEL: A
; CHECK:  %pIV = alloca i32
; CHECK:  %0 = icmp ugt i32 %S, 1
; CHECK:  %umax = select i1 %0, i32 %S, i32 1
; CHECK:  store i32 0, i32* %pIV
; CHECK:  [[CMP0:%[a-zA-Z0-9]+]] = icmp ult i32 0, %umax
; CHECK:  br i1 [[CMP0]], label %memset.body, label %memset.post
; CHECK: memset.body:
; CHECK:   %IV = load i32* %pIV
; CHECK:   [[GEP0:%[a-zA-Z0-9]+]] = getelementptr i8 addrspace(1)* %B, i32 %IV
; CHECK:   store i8 42, i8 addrspace(1)* [[GEP0]], align 1
; CHECK:   [[INC0:%[a-zA-Z0-9]+]] = add i32 %IV, 1
; CHECK:   store i32 [[INC0]], i32* %pIV
; CHECK:   [[CMP1:%[a-zA-Z0-9]+]] = icmp ult i32 [[INC0]], %umax
; CHECK:   br i1 [[CMP1]], label %memset.body, label %memset.post
; CHECK: memset.post:
; CHECK: ret
  %0 = icmp ugt i32 %S, 1
  %umax = select i1 %0, i32 %S, i32 1
  call void @llvm.memset.p1i8.i32(i8 addrspace(1)* %B, i8 42, i32 %umax, i32 1, i1 false)
  ret void
}

define void @B(i8 addrspace(1)* %B, i32 %S, <8 x i32> %r0, <8 x i32> %payloadHeader) #0 {
entry:
; CHECK-LABEL: B
; CHECK-NOT: call
; CHECK: ret
  call void @llvm.memset.p1i8.i32(i8 addrspace(1)* %B, i8 42, i32 %S, i32 1, i1 false)
  ret void
}

define void @C(i8 addrspace(1)* %B, <8 x i32> %r0, <8 x i32> %payloadHeader) #0 {
entry:
; CHECK-LABEL: C
; CHECK-NOT: call
; CHECK: ret
  call void @llvm.memset.p1i8.i32(i8 addrspace(1)* %B, i8 42, i32 1, i32 1, i1 false)
  ret void
}

declare void @llvm.memset.p1i8.i32(i8 addrspace(1)* nocapture, i8, i32, i32, i1) #1

attributes #0 = { alwaysinline nounwind }
attributes #1 = { nounwind }
