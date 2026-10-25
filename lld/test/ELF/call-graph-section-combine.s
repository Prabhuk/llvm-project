# REQUIRES: x86
## --call-graph-section=combine keeps the object files' call graph profile and
## adds what that profile cannot see: indirect calls, resolved to their
## type-compatible targets through .llvm.callgraph records, and the direct
## calls of code without records.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux a.s -o a.o

## The profile alone clusters A with its direct callee B only.
# RUN: ld.lld --call-graph-profile-sort=cdsort -e A a.o -o profile
# RUN: llvm-nm -n profile | FileCheck %s --check-prefix=PROFILE

## Combined, A's indirect target C and the assembly call from Asm to E add
## edges as well.
# RUN: ld.lld --call-graph-section=combine --call-graph-profile-sort=cdsort -e A a.o -o combine
# RUN: llvm-nm -n combine | FileCheck %s --check-prefix=COMBINE

## Without the call graph section, only A and B are ordered; C, E and Asm keep
## their input order after D.
# PROFILE:      T B
# PROFILE-NEXT: T A
# PROFILE-NEXT: T D
# PROFILE-NEXT: T C
# PROFILE-NEXT: T E
# PROFILE-NEXT: T Asm

## Combined, C (indirect target of A) and E and Asm (assembly call edge) are
## ordered as well, ahead of the unrelated D.
# COMBINE:      T B
# COMBINE-NEXT: T C
# COMBINE-NEXT: T E
# COMBINE-NEXT: T A
# COMBINE-NEXT: T Asm
# COMBINE-NEXT: T D

#--- a.s
    .section .text.D,"ax",@progbits
    .globl D
D:
    retq

    .section .text.C,"ax",@progbits
    .globl C
C:
    retq

    .section .text.E,"ax",@progbits
    .globl E
E:
    retq

## No call graph record: its direct call becomes an edge from its relocation.
    .section .text.Asm,"ax",@progbits
    .globl Asm
Asm:
    callq E
    retq

    .section .text.A,"ax",@progbits
    .globl A
A:
    callq B
    retq

    .section .text.B,"ax",@progbits
    .globl B
B:
    retq

    .section .rodata.fp,"a",@progbits
    .globl fp_table
fp_table:
    .quad C

    .cg_profile A, B, 1024

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.A
    .byte 0                # format version 0
    .byte 6                # flags: HasDirectCallees | HasIndirectCallees
    .quad A
    .quad 0
    .byte 1
    .quad B
    .byte 1
    .quad 0x1234           # indirect callee type ID

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.B
    .byte 0
    .byte 0
    .quad B
    .quad 0

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.C
    .byte 0
    .byte 1                # flags: IsIndirectTarget
    .quad C
    .quad 0x1234

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.D
    .byte 0
    .byte 0
    .quad D
    .quad 0

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.E
    .byte 0
    .byte 0
    .quad E
    .quad 0
