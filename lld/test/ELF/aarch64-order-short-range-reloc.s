# REQUIRES: aarch64
## A section order must not separate sections that refer to each other through
## short-range relocations that are never redirected through thunks. Here the
## ordering file would put 2 MiB of code between conditional branches in
## .text.a and their target in .text.b, which is out of range of
## R_AARCH64_CONDBR19.

# RUN: rm -rf %t && split-file %s %t && cd %t
# RUN: llvm-mc -filetype=obj -triple=aarch64 a.s -o a.o

## Input order keeps .text.b right after .text.a.
# RUN: ld.lld a.o -o out-none
# RUN: llvm-nm --numeric-sort out-none | FileCheck %s --check-prefix=NONE
# NONE:      T _start
# NONE-NEXT: T a
# NONE-NEXT: T b
# NONE-NEXT: T big

## The order places .text.big right after .text.a and leaves .text.b unordered,
## which would put .text.b after .text.big. Instead .text.b moves with .text.a.
# RUN: ld.lld a.o --symbol-ordering-file=order.txt -o out
# RUN: llvm-nm --numeric-sort out | FileCheck %s
# CHECK:      T a
# CHECK-NEXT: T b
# CHECK-NEXT: T big
# CHECK-NEXT: T _start

#--- order.txt
a
big

#--- a.s
.section .text._start,"ax",@progbits
.globl _start
_start:
  bl a
  ret

.section .text.a,"ax",@progbits
.globl a
a:
  cbz x0, b
  b.eq b
  ret

.section .text.b,"ax",@progbits
.globl b
b:
  ret

.section .text.big,"ax",@progbits
.globl big
big:
  .space 0x200000
  ret
