# REQUIRES: x86
## ICF folds `f2` into `f1`, which have identical code but different indirect
## call type IDs. The output keeps the call graph records of both functions,
## both at the address of `f1`, so that the folded code stays an indirect call
## target of type 0x22 as well as of type 0x11.

# RUN: llvm-mc -filetype=obj -triple=x86_64 %s -o %t.o
# RUN: ld.lld --icf=all --print-icf-sections %t.o -o %t | FileCheck %s --check-prefix=ICF
# RUN: llvm-readelf -x .llvm.callgraph %t | FileCheck %s
# RUN: ld.lld --icf=all --call-graph-section %t.o -o %t.sorted
# RUN: llvm-readelf -x .llvm.callgraph %t.sorted | FileCheck %s --check-prefix=SORT

# ICF:      selected section {{.*}}:(.text.f1)
# ICF-NEXT:   removing identical section {{.*}}:(.text.f2)

## The records of `f1` (type 0x11) and `f2` (type 0x22) both hold the address
## of `f1` (0x201130), followed by the record of `_start`.
# CHECK:      0x00000000 00013011 20000000 00001100 00000000
# CHECK-NEXT: 0x00000010 00000001 30112000 00000000 22000000
# CHECK-NEXT: 0x00000020 00000000 00043611 20000000 00000000
# CHECK-NEXT: 0x00000030 00000000 00000111 00000000 000000

## Layout from the call graph places `_start` first and `f1` at 0x201137. The
## records follow the code order, and both records still hold that address.
# SORT:      0x00000000 00043011 20000000 00000000 00000000
# SORT-NEXT: 0x00000010 00000111 00000000 00000000 01371120
# SORT-NEXT: 0x00000020 00000000 00110000 00000000 00000137
# SORT-NEXT: 0x00000030 11200000 00000022 00000000 000000

    .section .text.f1,"ax",@progbits
    .globl f1
f1:
    movl $1, %eax
    retq

    .section .text.f2,"ax",@progbits
    .globl f2
f2:
    movl $1, %eax
    retq

    .section .text._start,"ax",@progbits
    .globl _start
_start:
    callq *table+8(%rip)
    retq

    .section .rodata.table,"a",@progbits
table:
    .quad f1
    .quad f2

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.f1
    .byte 0, 1
    .quad f1
    .quad 0x11
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.f2
    .byte 0, 1
    .quad f2
    .quad 0x22
    .section .llvm.callgraph,"o",@llvm_call_graph,.text._start
    .byte 0, 4
    .quad _start
    .quad 0
    .byte 1
    .quad 0x11
