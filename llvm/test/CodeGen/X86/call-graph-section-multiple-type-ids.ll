;; Test that a function with several type IDs gets one call graph record for
;; its first type ID, followed by a record for each other type ID that only
;; marks it as an indirect target. Duplicate type IDs are emitted once.

; RUN: llc -mtriple=x86_64-unknown-linux --call-graph-section -o - < %s | FileCheck %s

declare void @callee()

; CHECK: _ZN7Derived1fEv:
define void @_ZN7Derived1fEv(ptr %this, ptr %fp) !callgraph !0 !callgraph !1 !callgraph !2 !callgraph !1 {
entry:
  call void @callee()
  call void %fp(ptr %this), !callee_type !3
  ret void
}

!0 = !{!"_ZTSFvvE.member"}
!1 = !{!"_ZTSM7DerivedFvvE.virtual"}
!2 = !{!"_ZTSM4BaseFvvE.virtual"}
!3 = !{!0}

; CHECK: .section .llvm.callgraph,"o",@llvm_call_graph,.text
;; The full record, with the first type ID.
; CHECK-NEXT: .byte   0
;; Flags: indirect target, direct callees and indirect callees.
; CHECK-NEXT: .byte   7
; CHECK-NEXT: .quad   _ZN7Derived1fEv
; CHECK-NEXT: .quad   -7646003550936526070
; CHECK-NEXT: .byte   1
; CHECK-NEXT: .quad   callee
; CHECK-NEXT: .byte   1
; CHECK-NEXT: .quad   -7646003550936526070
;; A record for each other type ID.
; CHECK-NEXT: .byte   0
; CHECK-NEXT: .byte   1
; CHECK-NEXT: .quad   _ZN7Derived1fEv
; CHECK-NEXT: .quad   1489773981807122792
; CHECK-NEXT: .byte   0
; CHECK-NEXT: .byte   1
; CHECK-NEXT: .quad   _ZN7Derived1fEv
; CHECK-NEXT: .quad   939904590958510875
; CHECK-NOT:  .quad   _ZN7Derived1fEv
