# REQUIRES: x86
## Test --call-graph-section with partial coverage: objects and functions that
## have no .llvm.callgraph record (hand-written assembly, objects built without
## -fcall-graph-section, and top-level asm sharing a section with compiled
## code).

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux cov.s -o cov.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux asm.s -o asm.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux mixed.s -o mixed.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux ext.s -o ext.o
# RUN: ld.lld -shared ext.o -soname=ext.so -o ext.so

## Direct branches in code without a record are exact call facts, so the
## assembly routine asm_copy is laid out next to asm_step, which it calls.
# RUN: ld.lld --call-graph-section=only --call-graph-profile-sort=hfsort -e c_main \
# RUN:   cov.o asm.o mixed.o ext.so -o out --verbose 2>&1 | FileCheck %s --check-prefix=LOG
# RUN: llvm-nm -n out | FileCheck %s --check-prefix=ORDER

## Coverage report:
## - Fully described: c_main, c_leaf, c_callback (cov.o).
## - Not described: asm_filler, asm_step, asm_copy (asm.o) and .text of
##   mixed.o, which holds a recorded function and an assembly function.
## - Calling unknown code: the four undescribed sections, plus c_leaf, which
##   calls ext_fn in a shared library.
## - Address-taken: c_callback (typed), asm_step (untyped) and mixed.o's .text
##   (untyped: its assembly part has no type).
# LOG: --call-graph-section: 3 of 7 executable sections ({{[0-9]+}} of {{[0-9]+}} bytes) are fully described by .llvm.callgraph records; 5 may call unknown code; 2 of 3 address-taken are untyped

## c_main's record gives c_main -> {asm_copy, c_leaf}; asm_copy's branch
## relocation gives asm_copy -> asm_step. The rest keeps its input order.
# ORDER:      T c_main
# ORDER-NEXT: T c_leaf
# ORDER-NEXT: T asm_copy
# ORDER-NEXT: T asm_step
# ORDER-NEXT: T c_callback
# ORDER-NEXT: T asm_filler
# ORDER-NEXT: T c_in_text
# ORDER-NEXT: T asm_in_text

#--- cov.s
    .section .text.c_main,"ax",@progbits
    .globl c_main
    .type c_main, @function
c_main:
    call asm_copy
    call c_leaf
    retq

    .section .text.c_leaf,"ax",@progbits
    .globl c_leaf
    .type c_leaf, @function
c_leaf:
    jmp ext_fn

    .section .text.c_callback,"ax",@progbits
    .globl c_callback
    .type c_callback, @function
c_callback:
    retq

    .section .data.table,"aw",@progbits
    .globl table
table:
    .quad c_callback

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.c_main
    .byte 0
    .byte 2
    .quad c_main
    .quad 0
    .byte 2
    .quad asm_copy
    .quad c_leaf

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.c_leaf
    .byte 0
    .byte 0
    .quad c_leaf
    .quad 0

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.c_callback
    .byte 0
    .byte 1
    .quad c_callback
    .quad 0x77

#--- asm.s
## Hand-written assembly: no .llvm.callgraph at all.
    .section .text.asm_filler,"ax",@progbits
    .globl asm_filler
    .type asm_filler, @function
asm_filler:
    retq

    .section .text.asm_step,"ax",@progbits
    .globl asm_step
    .type asm_step, @function
asm_step:
    retq

    .section .text.asm_copy,"ax",@progbits
    .globl asm_copy
    .type asm_copy, @function
asm_copy:
    call asm_step
    leaq asm_step(%rip), %rax
    retq

#--- mixed.s
## No -ffunction-sections: a compiled function and a top-level asm function
## share .text. Only the compiled one has a record.
    .text
    .globl c_in_text
    .type c_in_text, @function
c_in_text:
    retq
    .globl asm_in_text
    .type asm_in_text, @function
asm_in_text:
    retq

    .section .data.mixed_table,"aw",@progbits
    .quad asm_in_text

    .section .llvm.callgraph,"o",@llvm_call_graph,.text
    .byte 0
    .byte 1
    .quad c_in_text
    .quad 0x77

#--- ext.s
    .globl ext_fn
    .type ext_fn, @function
ext_fn:
    retq
