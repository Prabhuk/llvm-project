# REQUIRES: x86
# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux single.s -o single.o

## Default: without --call-graph-section-sort, sections are placed in input order.
# RUN: ld.lld -e A single.o -o single1
# RUN: llvm-nm -n single1 | FileCheck %s --check-prefix=NO-SORT

## --no-call-graph-section-sort preserves input order.
# RUN: ld.lld --no-call-graph-section-sort -e A single.o -o single2
# RUN: cmp single1 single2

## --call-graph-section-sort=hfsort clusters direct and indirect callers/callees.
# RUN: ld.lld --call-graph-section-sort=hfsort -e A single.o -o single3
# RUN: llvm-nm -n single3 | FileCheck %s --check-prefix=SORTED

## --call-graph-section-sort (defaults to cdsort) clusters sections.
# RUN: ld.lld --call-graph-section-sort -e A single.o -o single4
# RUN: llvm-nm -n single4 | FileCheck %s --check-prefix=SORTED

## Multi-object test: cross-TU direct and indirect calls.
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux multi-caller.s -o multi-caller.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux multi-callee.s -o multi-callee.o
# RUN: ld.lld -e caller multi-caller.o multi-callee.o -o multi-nosort
# RUN: llvm-nm -n multi-nosort | FileCheck %s --check-prefix=MULTI-NOSORT

# RUN: ld.lld --call-graph-section-sort -e caller multi-caller.o multi-callee.o -o multi-sorted
# RUN: llvm-nm -n multi-sorted | FileCheck %s --check-prefix=MULTI-SORTED

## Invalid sort algorithm produces an error.
# RUN: not ld.lld --call-graph-section-sort=foo -e A single.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR
# ERR: error: unknown --call-graph-section-sort= value: foo

# NO-SORT:      {{[0-9a-f]+}} T D
# NO-SORT-NEXT: {{[0-9a-f]+}} T C
# NO-SORT-NEXT: {{[0-9a-f]+}} T B
# NO-SORT-NEXT: {{[0-9a-f]+}} T A

# SORTED:      {{[0-9a-f]+}} T D
# SORTED-NEXT: {{[0-9a-f]+}} T A
# SORTED-NEXT: {{[0-9a-f]+}} T C
# SORTED-NEXT: {{[0-9a-f]+}} T B

# MULTI-NOSORT:      {{[0-9a-f]+}} T caller
# MULTI-NOSORT-NEXT: {{[0-9a-f]+}} T uncalled
# MULTI-NOSORT-NEXT: {{[0-9a-f]+}} T target_direct
# MULTI-NOSORT-NEXT: {{[0-9a-f]+}} T target_indirect

# MULTI-SORTED:      {{[0-9a-f]+}} T caller
# MULTI-SORTED-NEXT: {{[0-9a-f]+}} T target_direct
# MULTI-SORTED-NEXT: {{[0-9a-f]+}} T target_indirect
# MULTI-SORTED-NEXT: {{[0-9a-f]+}} T uncalled

#--- single.s
    .section .text.D,"ax",@progbits
    .globl D
D:
    retq

    .section .text.C,"ax",@progbits
    .globl C
C:
    retq

    .section .text.B,"ax",@progbits
    .globl B
B:
    retq

    .section .text.A,"ax",@progbits
    .globl A
A:
    retq

## Callgraph record for D:
## Calls A directly.
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.D
    .byte 0                # format version 0
    .byte 2                # flags: HasDirectCallees (2)
    .quad D                # function entry PC
    .quad 0                # function type ID
    .byte 1                # 1 direct callee
    .quad A                # direct callee A

## Callgraph record for A:
## Calls C indirectly with type ID 0x1234.
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.A
    .byte 0                # format version 0
    .byte 4                # flags: HasIndirectCallees (4)
    .quad A                # function entry PC
    .quad 0                # function type ID
    .byte 1                # 1 indirect type ID
    .quad 0x1234           # type ID

## Callgraph record for C:
## Is an indirect target with type ID 0x1234.
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.C
    .byte 0                # format version 0
    .byte 1                # flags: IsIndirectTarget (1)
    .quad C                # function entry PC
    .quad 0x1234           # function type ID (0x1234)

#--- multi-caller.s
    .section .text.caller,"ax",@progbits
    .globl caller
caller:
    retq

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.caller
    .byte 0                # format version 0
    .byte 6                # flags: HasDirectCallees (2) | HasIndirectCallees (4)
    .quad caller           # function entry PC
    .quad 0                # function type ID
    .byte 1                # 1 direct callee
    .quad target_direct    # direct callee target_direct
    .byte 1                # 1 indirect type ID
    .quad 0x5678           # type ID 0x5678

#--- multi-callee.s
    .section .text.uncalled,"ax",@progbits
    .globl uncalled
uncalled:
    retq

    .section .text.target_direct,"ax",@progbits
    .globl target_direct
target_direct:
    retq

    .section .text.target_indirect,"ax",@progbits
    .globl target_indirect
target_indirect:
    retq

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.target_indirect
    .byte 0                # format version 0
    .byte 1                # flags: IsIndirectTarget (1)
    .quad target_indirect  # function entry PC
    .quad 0x5678           # function type ID (0x5678)
