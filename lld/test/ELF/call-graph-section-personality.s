# REQUIRES: x86
## A personality routine referenced from a CIE is called by the unwinder
## through that reference, so --call-graph-section records it as
## address-taken (a candidate for indirect calls of its type) even though no
## other relocation refers to it. FDE references to the functions they
## describe are not address-takes.

# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t.o
# RUN: ld.lld --call-graph-section=only --call-graph-profile-sort=hfsort %t.o \
# RUN:   -e thrower -o %t --verbose 2>&1 | FileCheck %s

## Address-taken: pers only. thrower and unwinder have FDEs but are not
## address-taken.
# CHECK: --call-graph-section: 3 of 3 executable sections ({{[0-9]+}} of {{[0-9]+}} bytes) are fully described by .llvm.callgraph records; 0 may call unknown code; 0 of 1 address-taken are untyped

    .section .text.thrower,"ax",@progbits
    .globl thrower
    .type thrower, @function
thrower:
    .cfi_startproc
    .cfi_personality 0x1b, pers
    call unwinder
    retq
    .cfi_endproc

## Stands in for the unwinder: calls the personality routine's type
## indirectly.
    .section .text.unwinder,"ax",@progbits
    .globl unwinder
    .type unwinder, @function
unwinder:
    .cfi_startproc
    callq *%rax
    retq
    .cfi_endproc

    .section .text.pers,"ax",@progbits
    .globl pers
    .hidden pers
    .type pers, @function
pers:
    retq

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.thrower
    .byte 0
    .byte 2
    .quad thrower
    .quad 0
    .byte 1
    .quad unwinder

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.unwinder
    .byte 0
    .byte 4
    .quad unwinder
    .quad 0
    .byte 1
    .quad 0x55

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.pers
    .byte 0
    .byte 1
    .quad pers
    .quad 0x55
