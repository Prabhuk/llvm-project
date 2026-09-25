# REQUIRES: arm
## On targets with limited branch range, sortISDBySectionOrder places the
## ordered block in the middle of the unordered sections once the output
## section reaches the thunk section spacing (~4 MiB for ARM objects without
## Thumb-2 build attributes). The static call graph tier of
## --call-graph-section=auto must not move that insertion point, so that the
## measured tier (hot_a, hot_b) keeps exactly the addresses it has without
## --call-graph-section.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=armv5-none-linux-gnueabi a.s -o a.o

# RUN: ld.lld --call-graph-profile-sort=hfsort -e main a.o -o none
# RUN: ld.lld --call-graph-profile-sort=hfsort --call-graph-section=auto -e main a.o -o auto
# RUN: llvm-nm -n none | grep ' hot_' > none.hot
# RUN: llvm-nm -n auto | grep ' hot_' > auto.hot
# RUN: diff none.hot auto.hot
# RUN: llvm-nm -n none | FileCheck %s --check-prefix=NONE
# RUN: llvm-nm -n auto | FileCheck %s --check-prefix=AUTO

## Without --call-graph-section the insertion point falls after cold_y.
# NONE:      T pad1
# NONE-NEXT: T cold_x
# NONE-NEXT: T cold_y
# NONE-NEXT: T hot_a
# NONE-NEXT: T hot_b
# NONE-NEXT: T pad2
# NONE-NEXT: T cold_z
# NONE-NEXT: T filler
# NONE-NEXT: T cold_w
# NONE-NEXT: T main

## In auto mode the prefix before the insertion point is unchanged (cold_x and
## cold_y keep their input order even though they are in the static tier), and
## the static-tier sections after it (cold_z, cold_w) are grouped immediately
## after the measured tier.
# AUTO:      T pad1
# AUTO-NEXT: T cold_x
# AUTO-NEXT: T cold_y
# AUTO-NEXT: T hot_a
# AUTO-NEXT: T hot_b
# AUTO-NEXT: T cold_z
# AUTO-NEXT: T cold_w
# AUTO-NEXT: T pad2
# AUTO-NEXT: T filler
# AUTO-NEXT: T main

#--- a.s
    .macro func name, size
    .section .text.\name,"ax",%progbits
    .globl \name
    .type \name, %function
\name:
    .space \size
    .endm

    func pad1, 0x200000
    func cold_x, 4
    func cold_y, 8
    func hot_b, 12
    func pad2, 0x200000
    func cold_z, 16
    func filler, 20
    func hot_a, 24
    func cold_w, 28
    func main, 4

## Measured profile.
    .cg_profile hot_a, hot_b, 100

## Static call graph: cold_x -> cold_y and cold_z -> cold_w.
    .section .llvm.callgraph,"o",%llvm_call_graph,.text.cold_x
    .byte 0
    .byte 2
    .long cold_x
    .quad 0
    .byte 1
    .long cold_y

    .section .llvm.callgraph,"o",%llvm_call_graph,.text.cold_z
    .byte 0
    .byte 2
    .long cold_z
    .quad 0
    .byte 1
    .long cold_w
