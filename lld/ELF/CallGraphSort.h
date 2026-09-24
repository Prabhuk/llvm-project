//===- CallGraphSort.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_CALL_GRAPH_SORT_H
#define LLD_ELF_CALL_GRAPH_SORT_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

namespace lld::elf {
struct Ctx;
class InputSectionBase;

// Returns the call graph section order. Sections ordered only by the static
// call graph tier (--call-graph-section) are added to \p secondary; they must
// not influence the placement of the other ordered sections.
llvm::DenseMap<const InputSectionBase *, int> computeCallGraphProfileOrder(
    Ctx &, llvm::DenseSet<const InputSectionBase *> &secondary);
} // namespace lld::elf

#endif
