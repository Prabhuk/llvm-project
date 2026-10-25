;; Test that -call-graph-section-frequencies records, for each indirect callee
;; type ID, the estimated number of calls per call of the function, summed
;; over its call sites and scaled by 1024.

; RUN: llc -mtriple=x86_64-unknown-linux --call-graph-section \
; RUN:   -call-graph-section-frequencies -o - < %s | FileCheck %s
; RUN: llc -mtriple=x86_64-unknown-linux --call-graph-section -o - < %s \
; RUN:   | FileCheck %s --check-prefix=NOFREQ

;; The call through type _ZTSFvE in %then runs once every four calls (256),
;; and the one in %loop eight times per call (8192). The call through type
;; _ZTSFicE runs only in %loop.
define void @f(i1 %c, ptr %fp, ptr %gp, ptr %hp) {
entry:
  br i1 %c, label %then, label %loop, !prof !4

then:
  call void %fp(), !callee_type !0
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ 0, %then ], [ %next, %loop ]
  call void %gp(), !callee_type !0
  %r = call i32 %hp(i8 0), !callee_type !2
  %next = add i32 %i, %r
  %done = icmp eq i32 %next, 100
  br i1 %done, label %exit, label %loop, !prof !5

exit:
  ret void
}

!0 = !{!1}
!1 = !{!"_ZTSFvE"}
!2 = !{!3}
!3 = !{!"_ZTSFicE"}
!4 = !{!"branch_weights", i32 1, i32 3}
!5 = !{!"branch_weights", i32 1, i32 7}

; CHECK:      .section .llvm.callgraph,"o",@llvm_call_graph,.text
;; Version
; CHECK-NEXT: .byte 0
;; Flags: indirect target, indirect callees and their frequencies.
; CHECK-NEXT: .byte 13
; CHECK-NEXT: .quad f
; CHECK-NEXT: .quad 0
;; Number of indirect callee type IDs, the IDs, then their frequencies.
; CHECK-NEXT: .byte 2
; CHECK-NEXT: .quad 6588678392271548388
; CHECK-NEXT: .quad 142272631118516422
; CHECK-NEXT: .ascii "\200B"
; CHECK-NEXT: .ascii "\200@"

; NOFREQ:      .section .llvm.callgraph,"o",@llvm_call_graph,.text
; NOFREQ-NEXT: .byte 0
; NOFREQ-NEXT: .byte 5
