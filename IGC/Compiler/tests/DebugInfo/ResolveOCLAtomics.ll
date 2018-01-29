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
; RUN: opt -igc-resolve-atomics -S %s -o - | FileCheck %s

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; This LIT test checks that ResolveOCLAtomics pass handles line debug info.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

target datalayout = "e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f16:16:16-f32:32:32-f64:64:64-f80:128:128-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-a64:64:64-f80:128:128-n8:16:32:64"
target triple = "igil_32_GEN8"

; Function Attrs: alwaysinline nounwind
define void @test(i32 * %dst, <8 x i32> %r0, <8 x i32> %payloadHeader, i8* %privateBase) #0 {
  %res = call i32 @__builtin_IB_atomic_inc_local_i32(i32* %dst) #2, !dbg !2
  ret void

; CHECK-NOT: call i32 @__builtin_IB_atomic_inc_local_i32
; CHECK: [[PtrDstToInt:%[a-zA-Z0-9]+]] = ptrtoint i32* %dst to i32, !dbg !2
; CHECK: [[res1:%[a-zA-Z0-9]+]] = call i32 addrspace(3)* @genx.GenISA.GetBufferPtr.p3i32(i32 0, i32 3), !dbg !2
; CHECK: [[res2:%[a-zA-Z0-9]+]] = call i32 @genx.GenISA.dwordatomicraw.p3i32(i32 addrspace(3)* [[res1]], i32 [[PtrDstToInt]], i32 0, i32 2), !dbg !2
}

declare i32 @__builtin_IB_atomic_inc_local_i32(i32*)

attributes #0 = { nounwind }

!igc.compiler.options = !{!0}
;; This hack named metadata is needed to assure metadata order
!hack_order = !{!0, !1, !2}

!0 = metadata !{metadata !"-fp32-correctly-rounded-divide-sqrt"}
!1 = metadata !{}
!2 = metadata !{i32 5, i32 0, metadata !1, null}
