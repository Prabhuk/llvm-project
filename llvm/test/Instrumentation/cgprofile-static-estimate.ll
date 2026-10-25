; Without a profile, -cg-profile-static-estimate weighs direct calls by the
; static block frequency of the call site relative to the function entry: once
; per call is 1024, a loop body is weighted by its estimated trip count, and a
; call on a cold path rounds down to nothing and is dropped.

; RUN: opt < %s -passes=cg-profile -S | FileCheck %s --check-prefix=NONE
; RUN: opt < %s -passes=cg-profile -cg-profile-static-estimate -S | FileCheck %s

; NONE-NOT: CG Profile

declare void @once()
declare void @hot()
declare void @cold()

define void @driver(i32 %n, i1 %err) {
entry:
  call void @once()
  %cmp0 = icmp sgt i32 %n, 0
  br i1 %cmp0, label %loop, label %exit

loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  call void @hot()
  %inc = add nsw i32 %i, 1
  %cmp = icmp slt i32 %inc, %n
  br i1 %cmp, label %loop, label %exit

exit:
  br i1 %err, label %fail, label %done, !prof !0

fail:
  call void @cold()
  br label %done

done:
  ret void
}

!0 = !{!"branch_weights", i32 1, i32 1048575}

; CHECK: !llvm.module.flags = !{![[FLAG:[0-9]+]]}
; CHECK: ![[FLAG]] = !{i32 5, !"CG Profile", ![[LIST:[0-9]+]]}
; CHECK: ![[LIST]] = distinct !{![[ONCE:[0-9]+]], ![[HOT:[0-9]+]]}
; CHECK: ![[ONCE]] = !{ptr @driver, ptr @once, i64 1024}
; CHECK: ![[HOT]] = !{ptr @driver, ptr @hot, i64 {{[0-9][0-9][0-9][0-9][0-9]+}}}
