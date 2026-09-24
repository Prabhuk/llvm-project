# REQUIRES: x86
## Test --call-graph-section-cold-unreachable: executable sections that the
## call graph reconstructed from .llvm.callgraph proves unreachable from the
## program's roots are placed at the end of their output section.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux main.s -o main.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux asm.s -o asm.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux start2.s -o start2.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux frag.s -o frag.o

## Roots: _start (entry), ctor (.init_array) and pers (personality routine in
## a CIE). ctor and pers are typed indirect targets whose type no call site
## uses, so only the roots keep them reachable. t1 is reached through a's
## indirect call site of type 0x11. t2 is address-taken with type 0x22, which
## no call site uses, so t2 and dead_callee, which only t2 calls, are
## unreachable. So are the unreferenced assembly functions.
# RUN: ld.lld --call-graph-section=only --call-graph-profile-sort=hfsort \
# RUN:   --call-graph-section-cold-unreachable main.o asm.o -o out --verbose 2>&1 | \
# RUN:   FileCheck %s --check-prefix=LOG
# RUN: llvm-nm -n out | FileCheck %s --check-prefix=COLD
# LOG: --call-graph-section-cold-unreachable: 4 executable sections ({{[0-9]+}} bytes) are unreachable from the roots; 0 of 0 sections in the measured profile are not reachable from the static roots

# COLD-DAG:  T _start
# COLD-DAG:  T main
# COLD-DAG:  T a
# COLD-DAG:  T t1
# COLD-DAG:  T ctor
# COLD-DAG:  T thrower
# COLD-DAG:  t pers
# COLD:      T t2
# COLD-NEXT: T dead_callee
# COLD-NEXT: T asm_unref
# COLD-NEXT: T asm_dispatch
# COLD-NOT:  {{ [Tt] }}

## Without the option the static edge t2 -> dead_callee even places the dead
## pair first.
# RUN: ld.lld --call-graph-section=only --call-graph-profile-sort=hfsort \
# RUN:   main.o asm.o -o out.base
# RUN: llvm-nm -n out.base | FileCheck %s --check-prefix=BASE
# BASE:      T t2
# BASE-NEXT: T dead_callee
# BASE-NEXT: T _start

## Soundness: start2 calls asm_dispatch, which has no record and may call
## any function whose address is taken. t2, and so dead_callee, become
## reachable. _start is no longer the entry point and is unreferenced.
# RUN: ld.lld --call-graph-section=only --call-graph-profile-sort=hfsort \
# RUN:   --call-graph-section-cold-unreachable -e start2 main.o asm.o start2.o \
# RUN:   -o out2 --verbose 2>&1 | FileCheck %s --check-prefix=LOG2
# RUN: llvm-nm -n out2 | FileCheck %s --check-prefix=COLD2
# LOG2: --call-graph-section-cold-unreachable: 2 executable sections ({{[0-9]+}} bytes) are unreachable from the roots;
# COLD2-DAG:  T t2
# COLD2-DAG:  T dead_callee
# COLD2-DAG:  T asm_dispatch
# COLD2:      T _start
# COLD2-NEXT: T asm_unref
# COLD2-NOT:  {{ [Tt] }}

## Function fragments. start3's record lists its calls, but not its branch
## to start3.cold (split off by -fsplit-machine-functions), which becomes a
## jump target, nor the landing pad start3.lp that its LSDA places in another
## section, which becomes a root. Neither fragment has a record, so both may
## call any address-taken function, which makes t2 reachable as well.
# RUN: ld.lld --call-graph-section=only --call-graph-profile-sort=hfsort \
# RUN:   --call-graph-section-cold-unreachable -e start3 main.o asm.o frag.o \
# RUN:   -o out3 --verbose 2>&1 | FileCheck %s --check-prefix=LOG3
# RUN: llvm-nm -n out3 | FileCheck %s --check-prefix=COLD3
# LOG3: --call-graph-section-cold-unreachable: 3 executable sections ({{[0-9]+}} bytes) are unreachable from the roots;
# COLD3-DAG:  t start3.cold
# COLD3-DAG:  t start3.lp
# COLD3-DAG:  T t2
# COLD3:      T _start
# COLD3-NEXT: T asm_unref
# COLD3-NEXT: T asm_dispatch
# COLD3-NOT:  {{ [Tt] }}

## A measured profile that observed t2 calling dead_callee contradicts the
## static roots. Observed sections are treated as roots, and the
## contradiction is reported.
# RUN: echo "t2 dead_callee 10" > order.txt
# RUN: ld.lld --call-graph-section=auto --call-graph-profile-sort=hfsort \
# RUN:   --call-graph-ordering-file=order.txt --call-graph-section-cold-unreachable \
# RUN:   main.o asm.o -o out4 --verbose 2>&1 | FileCheck %s --check-prefix=LOG4
# LOG4: --call-graph-section-cold-unreachable: 2 executable sections ({{[0-9]+}} bytes) are unreachable from the roots; 2 of 2 sections in the measured profile are not reachable from the static roots

## In a shared object every exported function is a root, and code outside
## the link may call any function whose address it obtains.
# RUN: ld.lld -shared --call-graph-section=only --call-graph-profile-sort=hfsort \
# RUN:   --call-graph-section-cold-unreachable main.o asm.o -o out.so --verbose 2>&1 | \
# RUN:   FileCheck %s --check-prefix=LOG5
# LOG5: --call-graph-section-cold-unreachable: 0 executable sections (0 bytes) are unreachable from the roots;

# RUN: ld.lld --call-graph-section-cold-unreachable main.o asm.o -o /dev/null 2>&1 | \
# RUN:   FileCheck %s --check-prefix=WARN
# WARN: warning: --call-graph-section-cold-unreachable has no effect without --call-graph-section and --call-graph-profile-sort

#--- main.s
    .section .text._start,"ax",@progbits
    .globl _start
    .type _start, @function
_start:
    call main
    retq

    .section .text.main,"ax",@progbits
    .globl main
    .type main, @function
main:
    call a
    call thrower
    retq

    .section .text.a,"ax",@progbits
    .globl a
    .type a, @function
a:
    movq table(%rip), %rax
    callq *%rax
    retq

    .section .text.t1,"ax",@progbits
    .globl t1
    .type t1, @function
t1:
    retq

    .section .text.t2,"ax",@progbits
    .globl t2
    .type t2, @function
t2:
    call dead_callee
    retq

    .section .text.dead_callee,"ax",@progbits
    .globl dead_callee
    .type dead_callee, @function
dead_callee:
    retq

    .section .text.ctor,"ax",@progbits
    .globl ctor
    .type ctor, @function
ctor:
    retq

    .section .text.thrower,"ax",@progbits
    .globl thrower
    .type thrower, @function
thrower:
    .cfi_startproc
    .cfi_personality 0x1b, pers
    retq
    .cfi_endproc

    .section .text.pers,"ax",@progbits
    .globl pers
    .hidden pers
    .type pers, @function
pers:
    retq

    .section .data.table,"aw",@progbits
    .globl table
    .hidden table
table:
    .quad t1
    .quad t2

    .section .init_array,"aw",@init_array
    .quad ctor

    .section .llvm.callgraph,"o",@llvm_call_graph,.text._start
    .byte 0
    .byte 2
    .quad _start
    .quad 0
    .byte 1
    .quad main

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.main
    .byte 0
    .byte 2
    .quad main
    .quad 0
    .byte 2
    .quad a
    .quad thrower

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.a
    .byte 0
    .byte 4
    .quad a
    .quad 0
    .byte 1
    .quad 0x11

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.t1
    .byte 0
    .byte 1
    .quad t1
    .quad 0x11

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.t2
    .byte 0
    .byte 3
    .quad t2
    .quad 0x22
    .byte 1
    .quad dead_callee

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.dead_callee
    .byte 0
    .byte 0
    .quad dead_callee
    .quad 0

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.ctor
    .byte 0
    .byte 1
    .quad ctor
    .quad 0x99

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.thrower
    .byte 0
    .byte 0
    .quad thrower
    .quad 0

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.pers
    .byte 0
    .byte 1
    .quad pers
    .quad 0x55

#--- asm.s
## Hand-written assembly: no .llvm.callgraph.
    .section .text.asm_unref,"ax",@progbits
    .globl asm_unref
    .type asm_unref, @function
asm_unref:
    retq

    .section .text.asm_dispatch,"ax",@progbits
    .globl asm_dispatch
    .type asm_dispatch, @function
asm_dispatch:
    jmpq *%rdi

#--- start2.s
    .section .text.start2,"ax",@progbits
    .globl start2
    .type start2, @function
start2:
    call main
    call asm_dispatch
    retq

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.start2
    .byte 0
    .byte 2
    .quad start2
    .quad 0
    .byte 2
    .quad main
    .quad asm_dispatch

#--- frag.s
    .section .text.start3,"ax",@progbits
    .globl start3
    .type start3, @function
start3:
    .cfi_startproc
    .cfi_personality 0x1b, pers
    .cfi_lsda 0x1b, .Llsda
    call main
    testl %eax, %eax
    jne start3.cold
    retq
    .cfi_endproc

    .section .text.split.start3,"ax",@progbits
    .type start3.cold, @function
start3.cold:
    ud2

## A basic block section holding a landing pad.
    .section .text.start3.lp,"ax",@progbits
start3.lp:
    ud2

    .section .gcc_except_table.start3,"a",@progbits
.Llsda:
    .quad start3.lp

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.start3
    .byte 0
    .byte 2
    .quad start3
    .quad 0
    .byte 1
    .quad main
