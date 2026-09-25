# REQUIRES: x86
# RUN: rm -rf %t && split-file %s %t && cd %t

# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux single.s -o single.o

## Default: without --call-graph-section, sections are placed in input order.
# RUN: ld.lld -e A single.o -o single1
# RUN: llvm-nm -n single1 | FileCheck %s --check-prefix=NO-SORT

## --no-call-graph-section preserves input order.
# RUN: ld.lld --no-call-graph-section -e A single.o -o single2
# RUN: cmp single1 single2

## --call-graph-section=only --call-graph-profile-sort=hfsort clusters direct and indirect callers/callees.
# RUN: ld.lld --call-graph-section=only --call-graph-profile-sort=hfsort -e A single.o -o single3
# RUN: llvm-nm -n single3 | FileCheck %s --check-prefix=SORTED

## --call-graph-section (only; the sort algorithm defaults to cdsort) clusters sections.
# RUN: ld.lld --call-graph-section -e A single.o -o single4
# RUN: llvm-nm -n single4 | FileCheck %s --check-prefix=SORTED

## Multi-object test: cross-TU direct and indirect calls.
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux multi-caller.s -o multi-caller.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux multi-callee.s -o multi-callee.o
# RUN: ld.lld -e caller multi-caller.o multi-callee.o -o multi-nosort
# RUN: llvm-nm -n multi-nosort | FileCheck %s --check-prefix=MULTI-NOSORT

# RUN: ld.lld --call-graph-section -e caller multi-caller.o multi-callee.o -o multi-sorted
# RUN: llvm-nm -n multi-sorted | FileCheck %s --check-prefix=MULTI-SORTED

## STB_LOCAL scoping test: internal linkage helpers with identical TypeIDs
## (0x9999) in different TUs must only match callers within their own TU.
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux local-tu1.s -o local-tu1.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux local-tu2.s -o local-tu2.o
# RUN: ld.lld --call-graph-section -e caller_tu1 local-tu2.o local-tu1.o -o local-scoped
# RUN: llvm-nm -n local-scoped | FileCheck %s --check-prefix=LOCAL-SCOPED

## Escaped STB_LOCAL test: an internal linkage function whose address is stored
## in an exported STB_GLOBAL data table (.rodata.vtable) escapes its TU and
## must match indirect callers in other TUs.
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux escape-caller.s -o escape-caller.o
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux escape-callee.s -o escape-callee.o
# RUN: ld.lld --call-graph-section -e escape_caller escape-caller.o escape-callee.o -o escape-sorted
# RUN: llvm-nm -n escape-sorted | FileCheck %s --check-prefix=ESCAPED-LOCAL

## STT_SECTION relocation test: relocations referencing section symbols
## (.text.sec_callee) with -ffunction-sections resolve to the section's
## effective binding and cluster properly.
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux section-sym.s -o section-sym.o
# RUN: ld.lld --call-graph-section -e sec_caller section-sym.o -o section-sym-sorted
# RUN: llvm-nm -n section-sym-sorted | FileCheck %s --check-prefix=SEC-SYM

## Fan-out cap test: when an indirect TypeID (0xDEAD) matches > 64 targets (65),
## the unconstrained clique is dropped so direct call edges dominate placement.
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux fanout.s -o fanout.o
# RUN: ld.lld --call-graph-section -e fanout_caller fanout.o -o fanout-sorted
# RUN: llvm-nm -n fanout-sorted | FileCheck %s --check-prefix=FANOUT-CAP

## Invalid sort algorithm produces an error.
# RUN: not ld.lld --call-graph-section=foo -e A single.o -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR
# ERR: error: unknown --call-graph-section= value: foo

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

# LOCAL-SCOPED:      {{[0-9a-f]+}} T caller_tu1
# LOCAL-SCOPED-NEXT: {{[0-9a-f]+}} t local_helper_tu1
# LOCAL-SCOPED-NEXT: {{[0-9a-f]+}} t local_helper_tu2
# LOCAL-SCOPED-NEXT: {{[0-9a-f]+}} T unrelated_tu1

# ESCAPED-LOCAL:      {{[0-9a-f]+}} T escape_caller
# ESCAPED-LOCAL-NEXT: {{[0-9a-f]+}} t escaped_local_target
# ESCAPED-LOCAL-NEXT: {{[0-9a-f]+}} t unescaped_local_other

# SEC-SYM:      {{[0-9a-f]+}} T sec_caller
# SEC-SYM-NEXT: {{[0-9a-f]+}} T sec_callee
# SEC-SYM-NEXT: {{[0-9a-f]+}} T sec_unrelated

# FANOUT-CAP:      {{[0-9a-f]+}} T fanout_caller
# FANOUT-CAP-NEXT: {{[0-9a-f]+}} T direct_after_fanout



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

    .section .rodata.fp,"a",@progbits
    .globl fp_table_single
fp_table_single:
    .quad C

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

    .section .rodata.fp,"a",@progbits
    .globl fp_table_multi
fp_table_multi:
    .quad target_indirect

## Note: uncalled also has IsIndirectTarget (1) and TypeID 0x5678 in .llvm.callgraph
## (simulating Clang AsmPrinter marking all STB_GLOBAL functions IsIndirectTarget=1),
## but its address is NOT taken in any code/data relocation, so link-time
## address-taken verification prunes it while retaining target_indirect.
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.uncalled
    .byte 0                # format version 0
    .byte 1                # flags: IsIndirectTarget (1)
    .quad uncalled         # function entry PC
    .quad 0x5678           # function type ID (0x5678)

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.target_indirect
    .byte 0                # format version 0
    .byte 1                # flags: IsIndirectTarget (1)
    .quad target_indirect  # function entry PC
    .quad 0x5678           # function type ID (0x5678)

#--- local-tu1.s
    .section .text.unrelated_tu1,"ax",@progbits
    .globl unrelated_tu1
unrelated_tu1:
    retq

    .section .text.local_helper_tu1,"ax",@progbits
local_helper_tu1:
    retq

    .section .text.caller_tu1,"ax",@progbits
    .globl caller_tu1
caller_tu1:
    retq

    .section .rodata.local_fp1,"a",@progbits
local_table_tu1:
    .quad local_helper_tu1

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.caller_tu1
    .byte 0                # format version 0
    .byte 4                # flags: HasIndirectCallees (4)
    .quad caller_tu1       # function entry PC
    .quad 0                # function type ID
    .byte 1                # 1 indirect type ID
    .quad 0x9999           # type ID 0x9999

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.local_helper_tu1
    .byte 0                # format version 0
    .byte 1                # flags: IsIndirectTarget (1)
    .quad local_helper_tu1 # function entry PC
    .quad 0x9999           # function type ID (0x9999)

#--- local-tu2.s
    .section .text.local_helper_tu2,"ax",@progbits
local_helper_tu2:
    retq

    .section .rodata.local_fp2,"a",@progbits
local_table_tu2:
    .quad local_helper_tu2

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.local_helper_tu2
    .byte 0                # format version 0
    .byte 1                # flags: IsIndirectTarget (1)
    .quad local_helper_tu2 # function entry PC
    .quad 0x9999           # function type ID (0x9999)

#--- escape-caller.s
    .section .text.escape_caller,"ax",@progbits
    .globl escape_caller
escape_caller:
    retq

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.escape_caller
    .byte 0                # format version 0
    .byte 4                # flags: HasIndirectCallees (4)
    .quad escape_caller    # function entry PC
    .quad 0                # function type ID
    .byte 1                # 1 indirect type ID
    .quad 0xAAAA           # type ID 0xAAAA

#--- escape-callee.s
    .section .text.unescaped_local_other,"ax",@progbits
unescaped_local_other:
    retq

    .section .text.escaped_local_target,"ax",@progbits
escaped_local_target:
    retq

    .section .rodata.vtable,"a",@progbits
    .globl exported_vtable
exported_vtable:
    .quad escaped_local_target

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.escaped_local_target
    .byte 0                # format version 0
    .byte 1                # flags: IsIndirectTarget (1)
    .quad escaped_local_target # function entry PC
    .quad 0xAAAA           # function type ID (0xAAAA)

#--- section-sym.s
    .section .text.sec_unrelated,"ax",@progbits
    .globl sec_unrelated
sec_unrelated:
    retq

    .section .text.sec_callee,"ax",@progbits
    .globl sec_callee
sec_callee:
    retq

    .section .text.sec_caller,"ax",@progbits
    .globl sec_caller
sec_caller:
    retq

## Direct call relocation references the STT_SECTION symbol (.text.sec_callee)
## rather than the named STT_FUNC symbol sec_callee.
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.sec_caller
    .byte 0                # format version 0
    .byte 2                # flags: HasDirectCallees (2)
    .quad .text.sec_caller # STT_SECTION relocation for caller
    .quad 0                # function type ID
    .byte 1                # 1 direct callee
    .quad .text.sec_callee # STT_SECTION relocation for callee

#--- fanout.s
    .section .text.direct_after_fanout,"ax",@progbits
    .globl direct_after_fanout
direct_after_fanout:
    retq

    .altmacro
    .macro def_fanout_target id
    .section .text.fanout_\id,"ax",@progbits
    .globl fanout_\id
fanout_\id:
    retq
    .section .rodata.fanout_fp,"a",@progbits
    .quad fanout_\id
    .section .llvm.callgraph,"o",@llvm_call_graph,.text.fanout_\id
    .byte 0
    .byte 1
    .quad fanout_\id
    .quad 0xDEAD
    .endm

    .set idx, 0
    .rept 65
    def_fanout_target %idx
    .set idx, idx+1
    .endr

    .section .text.fanout_caller,"ax",@progbits
    .globl fanout_caller
fanout_caller:
    retq

    .section .llvm.callgraph,"o",@llvm_call_graph,.text.fanout_caller
    .byte 0                # format version 0
    .byte 6                # flags: HasDirectCallees (2) | HasIndirectCallees (4)
    .quad fanout_caller    # function entry PC
    .quad 0                # function type ID
    .byte 1                # 1 direct callee
    .quad direct_after_fanout
    .byte 1                # 1 indirect type ID (matches 65 > 64 targets -> dropped)
    .quad 0xDEAD



