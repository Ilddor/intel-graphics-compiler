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
; RUN: igc_opt %s -S -o - -basicaa -igc-memopt -instcombine | FileCheck %s

target datalayout = "e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f16:16:16-f32:32:32-f64:64:64-f80:128:128-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-a:64:64-f80:128:128-n8:16:32:64"

%struct.S = type { float, float, float, i32 }
%struct.S1 = type { <2 x float>, <2 x i32> }

define float @f0(%struct.S* %src) {
  %p0 = getelementptr inbounds %struct.S* %src, i32 0, i32 0
  %x = load float* %p0, align 4
  %p1 = getelementptr inbounds %struct.S* %src, i32 0, i32 1
  %y = load float* %p1, align 4
  %s0 = fadd float %x, %y
  %p2 = getelementptr inbounds %struct.S* %src, i32 0, i32 2
  %z = load float* %p2, align 4
  %s1 = fadd float %s0, %z
  %p3 = getelementptr inbounds %struct.S* %src, i32 0, i32 3
  %iw = load i32* %p3, align 4
  %w = uitofp i32 %iw to float
  %s2 = fadd float %s1, %w
  ret float %s2
}

; CHECK-LABEL: define float @f0
; CHECK: %1 = bitcast %struct.S* %src to <4 x float>*
; CHECK: %2 = load <4 x float>* %1, align 4
; CHECK: %3 = extractelement <4 x float> %2, i32 0
; CHECK: %4 = extractelement <4 x float> %2, i32 1
; CHECK: %5 = extractelement <4 x float> %2, i32 2
; CHECK: %6 = extractelement <4 x float> %2, i32 3
; CHECK: %7 = bitcast float %6 to i32
; CHECK: %s0 = fadd float %3, %4
; CHECK: %s1 = fadd float %s0, %5
; CHECK: %w = uitofp i32 %7 to float
; CHECK: %s2 = fadd float %s1, %w
; CHECK: ret float %s2


define void @f1(%struct.S* %dst, float %x, float %y, float %z, float %w) {
  %p0 = getelementptr inbounds %struct.S* %dst, i32 0, i32 0
  store float %x, float* %p0, align 4
  %p1 = getelementptr inbounds %struct.S* %dst, i32 0, i32 1
  store float %y, float* %p1, align 4
  %p2 = getelementptr inbounds %struct.S* %dst, i32 0, i32 2
  store float %z, float* %p2, align 4
  %p3 = getelementptr inbounds %struct.S* %dst, i32 0, i32 3
  %iw = fptoui float %w to i32
  store i32 %iw, i32* %p3, align 4
  ret void
}

; CHECK-LABEL: define void @f1
; CHECK: %iw = fptoui float %w to i32
; CHECK: %1 = insertelement <4 x float> undef, float %x, i32 0
; CHECK: %2 = insertelement <4 x float> %1, float %y, i32 1
; CHECK: %3 = insertelement <4 x float> %2, float %z, i32 2
; CHECK: %4 = bitcast i32 %iw to float
; CHECK: %5 = insertelement <4 x float> %3, float %4, i32 3
; CHECK: %6 = bitcast %struct.S* %dst to <4 x float>*
; CHECK: store <4 x float> %5, <4 x float>* %6, align 4
; CHECK: ret void


define float @f2(%struct.S1* %src) {
  %p0 = getelementptr inbounds %struct.S1* %src, i32 0, i32 0, i32 0
  %x = load float* %p0, align 4
  %p1 = getelementptr inbounds %struct.S1* %src, i32 0, i32 0, i32 1
  %y = load float* %p1, align 4
  %s0 = fadd float %x, %y
  %p2 = getelementptr inbounds %struct.S1* %src, i32 0, i32 1, i32 0
  %iz = load i32* %p2, align 4
  %z = uitofp i32 %iz to float
  %s1 = fadd float %s0, %z
  %p3 = getelementptr inbounds %struct.S1* %src, i32 0, i32 1, i32 1
  %iw = load i32* %p3, align 4
  %w = uitofp i32 %iw to float
  %s2 = fadd float %s1, %w
  ret float %s2
}

; CHECK-LABEL: define float @f2
; CHECK:  %1 = bitcast %struct.S1* %src to <4 x float>*
; CHECK:  %2 = load <4 x float>* %1, align 4
; CHECK:  %3 = extractelement <4 x float> %2, i32 0
; CHECK:  %4 = extractelement <4 x float> %2, i32 1
; CHECK:  %5 = extractelement <4 x float> %2, i32 2
; CHECK:  %6 = bitcast float %5 to i32
; CHECK:  %7 = extractelement <4 x float> %2, i32 3
; CHECK:  %8 = bitcast float %7 to i32
; CHECK:  %s0 = fadd float %3, %4
; CHECK:  %z = uitofp i32 %6 to float
; CHECK:  %s1 = fadd float %s0, %z
; CHECK:  %w = uitofp i32 %8 to float
; CHECK:  %s2 = fadd float %s1, %w
; CHECK:  ret float %s2


define void @f3(%struct.S1* %dst, float %x, float %y, i32 %z, i32 %w) {
  %p0 = getelementptr inbounds %struct.S1* %dst, i32 0, i32 0, i32 0
  store float %x, float* %p0, align 4
  %p1 = getelementptr inbounds %struct.S1* %dst, i32 0, i32 0, i32 1
  store float %y, float* %p1, align 4
  %p2 = getelementptr inbounds %struct.S1* %dst, i32 0, i32 1, i32 0
  store i32 %z, i32* %p2, align 4
  %p3 = getelementptr inbounds %struct.S1* %dst, i32 0, i32 1, i32 1
  store i32 %w, i32* %p3, align 4
  ret void
}

; CHECK-LABEL: define void @f3
; CHECK: %1 = insertelement <4 x float> undef, float %x, i32 0
; CHECK: %2 = insertelement <4 x float> %1, float %y, i32 1
; CHECK: %3 = bitcast i32 %z to float
; CHECK: %4 = insertelement <4 x float> %2, float %3, i32 2
; CHECK: %5 = bitcast i32 %w to float
; CHECK: %6 = insertelement <4 x float> %4, float %5, i32 3
; CHECK: %7 = bitcast %struct.S1* %dst to <4 x float>*
; CHECK: store <4 x float> %6, <4 x float>* %7, align 4
; CHECK: ret void
