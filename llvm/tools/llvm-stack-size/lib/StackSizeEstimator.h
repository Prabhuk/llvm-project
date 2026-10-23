//===- StackSizeEstimator.h - Worst-case stack usage of entry points ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// StackSizeEstimator condenses a StackSizeGraph into its strongly connected
// components and computes, for any entry point, the largest stack usage of a
// call path from it: the sum of the frame sizes along the path, plus the
// per-call overhead of the return address and the red zone of a leaf where
// the target ABI has them.
//
// The estimate is exact when every function on every path from the entry
// point has a known frame size and known callees. Otherwise it is a lower
// bound, and the estimate records why.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_STACK_SIZE_LIB_STACKSIZEESTIMATOR_H
#define LLVM_TOOLS_LLVM_STACK_SIZE_LIB_STACKSIZEESTIMATOR_H

#include "StackSizeGraph.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
namespace stacksize {

/// Target ABI parameters of the estimate.
struct EstimatorOptions {
  /// Bytes a call pushes on the stack before the callee's frame: the return
  /// address on x86, nothing on architectures that pass it in a register.
  uint64_t CallFrameOverhead = 0;

  /// Bytes below the stack pointer that a leaf may use without moving it.
  /// .stack_sizes does not account for them, so they are added to every leaf.
  uint64_t RedZoneSize = 0;
};

/// The call frame overhead of the ABI of \p Machine (an ELF e_machine).
uint64_t defaultCallFrameOverhead(uint16_t Machine);

/// The red zone size of the ABI of \p Machine (an ELF e_machine).
uint64_t defaultRedZoneSize(uint16_t Machine);

/// An entry point to analyze.
struct EntryPoint {
  Function *F = nullptr;
  /// The name the entry point was selected by, for reports.
  std::string Name;
};

/// Resolves the entry points named by \p Specs, each a symbol name, a glob
/// pattern over symbol names, or an address. A name defined at several
/// addresses selects all of them. Without \p Specs the entry points are the
/// ELF entry point and the exported functions.
Expected<std::vector<EntryPoint>>
resolveEntryPoints(const StackSizeGraph &G, ArrayRef<std::string> Specs);

/// An uninstrumented callee: a function without a call graph record, whose
/// own callees are therefore unknown, with the functions that call it.
struct UninstrumentedCallee {
  const Function *Callee = nullptr;
  /// In address order.
  SmallVector<const Function *, 2> Callers;
};

/// One step of a max stack path: a single function, or every member of a
/// recursive component, since all of their frames count.
struct PathStep {
  /// The function, or the members of the component in address order.
  ArrayRef<const Function *> Functions;
  /// The recursive component, as an index into StackSizeEstimator::cycles(),
  /// or none for a single function.
  std::optional<unsigned> Cycle;
};

/// The estimate for one entry point. It refers to the graph and to the
/// estimator it came from, which must outlive it.
struct Estimate {
  EntryPoint Entry;

  /// The largest stack usage of a path from the entry point, in bytes. A
  /// lower bound when IsLowerBound is set.
  uint64_t MaxStackUsage = 0;

  /// That path, in call order, starting with the step that holds the entry
  /// point.
  SmallVector<PathStep, 8> MaxStackPath;

  /// Whether MaxStackUsage is only a lower bound, for the reasons below.
  bool IsLowerBound = false;

  /// Reachable recursive components, as indexes into
  /// StackSizeEstimator::cycles(), in increasing order.
  SmallVector<unsigned, 1> Cycles;

  /// Reachable functions with a call graph record but no frame size, in
  /// address order, like every list below.
  SmallVector<const Function *, 2> UnknownFrames;

  /// Reachable functions without a call graph record.
  SmallVector<UninstrumentedCallee, 2> UninstrumentedCallees;

  /// Reachable functions with an indirect call whose type matches nothing.
  SmallVector<const Function *, 2> UnresolvedIndirectCallers;

  /// Reachable functions that make indirect calls while the file has indirect
  /// call targets of unknown type, which any of those calls might reach.
  SmallVector<const Function *, 2> IndirectCallersWithUntypedTargets;
};

/// Computes stack usage estimates over one graph.
class StackSizeEstimator {
public:
  StackSizeEstimator(const StackSizeGraph &G, const EstimatorOptions &Opts);

  const EstimatorOptions &getOptions() const { return Opts; }

  /// The bytes \p F contributes to a path: its frame, the red zone if it is a
  /// leaf, and the call frame overhead. Unknown frames contribute 0.
  uint64_t getFrameUsage(const Function &F) const;

  /// The red zone bytes counted for \p F.
  uint64_t getRedZone(const Function &F) const {
    return F.isLeaf() ? Opts.RedZoneSize : 0;
  }

  /// Whether \p F is on a cycle, including a call to itself.
  bool isRecursive(const Function &F) const;

  /// Whether \p F makes an indirect call whose targets are not all known:
  /// one whose type matches no function, or any one while the file has
  /// indirect call targets of unknown type.
  bool callsUnknownTargets(const Function &F) const;

  /// The recursive components of the graph, each as its members in address
  /// order, ordered by their first member.
  ArrayRef<ArrayRef<const Function *>> cycles() const { return Cycles; }

  Estimate estimate(const EntryPoint &Entry) const;

private:
  struct Component {
    SmallVector<const Function *, 1> Members;
    SmallVector<unsigned, 2> Successors;
    /// The index in Cycles when the component is recursive.
    std::optional<unsigned> Cycle;
    /// The members that make an estimate a lower bound: those without a
    /// record or a frame size, or with indirect calls to unknown targets.
    SmallVector<const Function *, 0> ReasonMembers;
    /// The usage of the members together.
    uint64_t LocalUsage = 0;
    /// The largest usage of a path starting in this component.
    uint64_t MaxUsage = 0;
    std::optional<unsigned> NextOnMaxPath;
  };

  const StackSizeGraph &G;
  EstimatorOptions Opts;
  std::vector<Component> Components;
  DenseMap<const Function *, unsigned> ComponentOf;
  std::vector<ArrayRef<const Function *>> Cycles;
  /// The callers of every function without a call graph record, so that an
  /// estimate finds them without scanning everything it reaches.
  DenseMap<const Function *, SmallVector<const Function *, 2>>
      CallersOfUninstrumented;
};

} // namespace stacksize
} // namespace llvm

#endif // LLVM_TOOLS_LLVM_STACK_SIZE_LIB_STACKSIZEESTIMATOR_H
