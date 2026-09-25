# REQUIRES: aarch64, x86
## Link-time address-taken verification classifies code references by
## relocation type: a direct call/branch relocation is not an address-take,
## while an address materialization is.
##
## `helper` and `target` share indirect type ID 0x77 and are both marked
## IsIndirectTarget (as the compiler does for every external function). `caller`
## calls type 0x77 indirectly. Only functions whose address is actually taken
## may become indirect candidates of `caller`.

# RUN: rm -rf %t && split-file %s %t && cd %t

## AArch64: `user` reaches `helper` with BL (R_AARCH64_CALL26), so `helper` is
## not address-taken and `caller` clusters with `target` only.
# RUN: llvm-mc -filetype=obj -triple=aarch64 a64-call.s -o a64-call.o
# RUN: ld.lld --call-graph-section -e caller a64-call.o -o a64-call
# RUN: llvm-nm -n a64-call | FileCheck %s --check-prefix=CALL

## AArch64: `user` materializes the address of `helper` with ADRP+ADD, so
## `helper` becomes an indirect candidate too and joins the cluster.
# RUN: llvm-mc -filetype=obj -triple=aarch64 a64-addr.s -o a64-addr.o
# RUN: ld.lld --call-graph-section -e caller a64-addr.o -o a64-addr
# RUN: llvm-nm -n a64-addr | FileCheck %s --check-prefix=ADDR

## x86-64: CALL (R_X86_64_PLT32) is not an address-take.
# RUN: llvm-mc -filetype=obj -triple=x86_64 x64-call.s -o x64-call.o
# RUN: ld.lld --call-graph-section -e caller x64-call.o -o x64-call
# RUN: llvm-nm -n x64-call | FileCheck %s --check-prefix=CALL

## x86-64: LEA (R_X86_64_PC32) is an address-take.
# RUN: llvm-mc -filetype=obj -triple=x86_64 x64-addr.s -o x64-addr.o
# RUN: ld.lld --call-graph-section -e caller x64-addr.o -o x64-addr
# RUN: llvm-nm -n x64-addr | FileCheck %s --check-prefix=ADDR

## With a call, only `target` is an indirect candidate: `caller` clusters with
## `target` and `helper` keeps its place. With an address-take, `helper` joins
## the cluster ahead of `target`, so a misclassified call fails CALL-NEXT.
# CALL:      T caller
# CALL-NEXT: T target
# CALL-NEXT: T helper
# CALL-NEXT: T user

# ADDR:      T caller
# ADDR-NEXT: T helper
# ADDR-NEXT: T target
# ADDR-NEXT: T user

#--- a64-call.s
    .section .text.helper,"ax",@progbits
    .globl helper
helper:
    ret

    .section .text.user,"ax",@progbits
    .globl user
user:
    bl helper
    ret

    .section .text.target,"ax",@progbits
    .globl target
target:
    ret

    .section .text.caller,"ax",@progbits
    .globl caller
caller:
    ret

    .section .rodata.fp,"a",@progbits
    .globl fp_table
fp_table:
    .quad target

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.helper
    .byte 0, 1
    .quad helper
    .quad 0x77
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.target
    .byte 0, 1
    .quad target
    .quad 0x77
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.caller
    .byte 0, 4
    .quad caller
    .quad 0
    .byte 1
    .quad 0x77

#--- a64-addr.s
    .section .text.helper,"ax",@progbits
    .globl helper
helper:
    ret

    .section .text.user,"ax",@progbits
    .globl user
user:
    adrp x0, helper
    add x0, x0, :lo12:helper
    ret

    .section .text.target,"ax",@progbits
    .globl target
target:
    ret

    .section .text.caller,"ax",@progbits
    .globl caller
caller:
    ret

    .section .rodata.fp,"a",@progbits
    .globl fp_table
fp_table:
    .quad target

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.helper
    .byte 0, 1
    .quad helper
    .quad 0x77
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.target
    .byte 0, 1
    .quad target
    .quad 0x77
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.caller
    .byte 0, 4
    .quad caller
    .quad 0
    .byte 1
    .quad 0x77

#--- x64-call.s
    .section .text.helper,"ax",@progbits
    .globl helper
helper:
    retq

    .section .text.user,"ax",@progbits
    .globl user
user:
    callq helper
    retq

    .section .text.target,"ax",@progbits
    .globl target
target:
    retq

    .section .text.caller,"ax",@progbits
    .globl caller
caller:
    retq

    .section .rodata.fp,"a",@progbits
    .globl fp_table
fp_table:
    .quad target

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.helper
    .byte 0, 1
    .quad helper
    .quad 0x77
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.target
    .byte 0, 1
    .quad target
    .quad 0x77
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.caller
    .byte 0, 4
    .quad caller
    .quad 0
    .byte 1
    .quad 0x77

#--- x64-addr.s
    .section .text.helper,"ax",@progbits
    .globl helper
helper:
    retq

    .section .text.user,"ax",@progbits
    .globl user
user:
    leaq helper(%rip), %rax
    retq

    .section .text.target,"ax",@progbits
    .globl target
target:
    retq

    .section .text.caller,"ax",@progbits
    .globl caller
caller:
    retq

    .section .rodata.fp,"a",@progbits
    .globl fp_table
fp_table:
    .quad target

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.helper
    .byte 0, 1
    .quad helper
    .quad 0x77
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.target
    .byte 0, 1
    .quad target
    .quad 0x77
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.caller
    .byte 0, 4
    .quad caller
    .quad 0
    .byte 1
    .quad 0x77
