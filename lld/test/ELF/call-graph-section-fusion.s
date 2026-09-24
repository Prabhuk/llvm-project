# REQUIRES: x86
## Test --call-graph-section={auto,only} together with a measured call graph
## profile (.llvm.call-graph-profile / --call-graph-ordering-file).
##
## In auto mode, sections covered by the measured profile must be laid out
## exactly as without --call-graph-section (same relative order and same
## addresses), and the static call graph from .llvm.callgraph must only
## organize the remaining sections, which are placed after them.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux fused.s -o fused.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux static.s -o static.o

## Baseline: measured profile only.
# RUN: ld.lld --call-graph-profile-sort=hfsort -e main fused.o -o hf-none
# RUN: ld.lld --call-graph-profile-sort=cdsort -e main fused.o -o cd-none
# RUN: llvm-nm -n hf-none | FileCheck %s --check-prefix=NONE
# RUN: llvm-nm -n cd-none | FileCheck %s --check-prefix=NONE

## auto: the measured tier is bitwise identical (names and addresses).
# RUN: ld.lld --call-graph-profile-sort=hfsort --call-graph-section=auto -e main fused.o -o hf-auto
# RUN: ld.lld --call-graph-profile-sort=cdsort --call-graph-section -e main fused.o -o cd-auto
# RUN: llvm-nm -n hf-none | grep ' hot_' > hf-none.hot
# RUN: llvm-nm -n hf-auto | grep ' hot_' > hf-auto.hot
# RUN: diff hf-none.hot hf-auto.hot
# RUN: llvm-nm -n cd-none | grep ' hot_' > cd-none.hot
# RUN: llvm-nm -n cd-auto | grep ' hot_' > cd-auto.hot
# RUN: diff cd-none.hot cd-auto.hot
# RUN: llvm-nm -n hf-auto | FileCheck %s --check-prefix=AUTO
# RUN: llvm-nm -n cd-auto | FileCheck %s --check-prefix=AUTO

## The same holds when the measured profile comes from
## --call-graph-ordering-file.
# RUN: ld.lld --call-graph-ordering-file=order.txt -e main fused.o -o file-none
# RUN: ld.lld --call-graph-ordering-file=order.txt --call-graph-section -e main fused.o -o file-auto
# RUN: llvm-nm -n file-none | grep ' hot_' > file-none.hot
# RUN: llvm-nm -n file-auto | grep ' hot_' > file-auto.hot
# RUN: diff file-none.hot file-auto.hot
# RUN: llvm-nm -n file-auto | FileCheck %s --check-prefix=AUTO

## only: the measured profile is ignored and the static graph drives the whole
## layout, including edges that touch profiled functions.
# RUN: ld.lld --call-graph-profile-sort=hfsort --call-graph-section=only -e main fused.o -o hf-only
# RUN: llvm-nm -n hf-only | FileCheck %s --check-prefix=ONLY
# RUN: ld.lld --call-graph-ordering-file=order.txt --call-graph-profile-sort=hfsort --call-graph-section=only -e main fused.o -o file-only 2>&1 | FileCheck %s --check-prefix=WARN
# RUN: cmp file-only hf-only
# WARN: warning: --call-graph-ordering-file is ignored with --call-graph-section=only

## Without a measured profile, auto is equivalent to only.
# RUN: ld.lld --call-graph-section=auto -e cold_x static.o -o static-auto
# RUN: ld.lld --call-graph-section=only -e cold_x static.o -o static-only
# RUN: cmp static-auto static-only

## --call-graph-profile-sort=none disables reordering regardless of the source.
# RUN: ld.lld --call-graph-profile-sort=none -e main fused.o -o nosort
# RUN: ld.lld --call-graph-profile-sort=none --call-graph-section=only -e main fused.o -o nosort-only
# RUN: cmp nosort nosort-only

## Invalid mode produces an error.
# RUN: not ld.lld --call-graph-section=foo -e main fused.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR
# ERR: error: unknown --call-graph-section= value: foo

## Without --call-graph-section: the measured tier, then the unordered sections
## in input order. In auto mode the static tier (cold_x, cold_z) follows the
## measured tier and precedes the unordered sections.
# NONE:      T hot_a
# NONE-NEXT: T hot_b
# NONE-NEXT: T hot_c
# NONE-NEXT: T cold_x
# NONE-NEXT: T filler1
# NONE-NEXT: T cold_y
# NONE-NEXT: T cold_z
# NONE-NEXT: T main

# AUTO:      T hot_a
# AUTO-NEXT: T hot_b
# AUTO-NEXT: T hot_c
# AUTO-NEXT: T cold_x
# AUTO-NEXT: T cold_z
# AUTO-NEXT: T filler1
# AUTO-NEXT: T cold_y
# AUTO-NEXT: T main

## Static edges only: hot_c -> hot_a and hot_a -> cold_y now participate, and
## hot_b (reached only through the measured profile) is unordered.
# ONLY:      T cold_x
# ONLY-NEXT: T cold_z
# ONLY-NEXT: T hot_c
# ONLY-NEXT: T hot_a
# ONLY-NEXT: T cold_y
# ONLY-NEXT: T hot_b
# ONLY-NEXT: T filler1
# ONLY-NEXT: T main

#--- order.txt
hot_a hot_b 100
hot_b hot_c 50

#--- fused.s
## Sizes differ so that any shift of the measured tier changes addresses.
    .macro func name, size
    .section .text.\name,"ax",@progbits
    .globl \name
\name:
    .fill \size, 1, 0xcc
    .endm

    func cold_x, 3
    func hot_b, 5
    func filler1, 7
    func cold_y, 11
    func hot_a, 13
    func cold_z, 17
    func hot_c, 19
    func main, 1

## Measured profile.
    .cg_profile hot_a, hot_b, 100
    .cg_profile hot_b, hot_c, 50

## Static call graph.
##   cold_x -> cold_z : both unprofiled, forms the static tier.
##   hot_a  -> cold_y : touches a profiled function, excluded in auto mode.
##   hot_c  -> hot_a  : between profiled functions, excluded in auto mode.
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.cold_x
    .byte 0
    .byte 2
    .quad cold_x
    .quad 0
    .byte 1
    .quad cold_z

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.hot_a
    .byte 0
    .byte 2
    .quad hot_a
    .quad 0
    .byte 1
    .quad cold_y

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.hot_c
    .byte 0
    .byte 2
    .quad hot_c
    .quad 0
    .byte 1
    .quad hot_a

#--- static.s
    .section .text.cold_y,"ax",@progbits
    .globl cold_y
cold_y:
    retq

    .section .text.cold_z,"ax",@progbits
    .globl cold_z
cold_z:
    retq

    .section .text.cold_x,"ax",@progbits
    .globl cold_x
cold_x:
    retq

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.cold_x
    .byte 0
    .byte 2
    .quad cold_x
    .quad 0
    .byte 1
    .quad cold_z
