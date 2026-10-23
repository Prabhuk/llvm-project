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

/// One term of a stack usage expression: a number of bytes plus the unknowns
/// along one kind of path, such as 80 + calls(<external>).
struct StackUsageTerm {
  uint64_t Constant = 0;
  /// Indexes into StackSizeEstimator::unknowns(), sorted and unique.
  SmallVector<unsigned, 2> Unknowns;
};

/// The stack usage of an entry point as an expression in which the unknowns
/// stay as terms. Every path from the entry point contributes a term, those
/// that cannot be the largest for any value of the unknowns are dropped, and
/// the expression is the maximum of the rest.
struct StackUsageExpression {
  /// In decreasing order of constant. The first term is the max stack usage
  /// with every unknown at zero, and an exact estimate is that one term.
  SmallVector<StackUsageTerm, 2> Terms;

  /// Whether terms were merged to keep the expression within its size limit.
  /// A merged term bounds the paths it stands for rather than equals them, so
  /// the expression is then an upper bound of the exact maximum. Its first
  /// term is unchanged.
  bool Coarsened = false;
};

/// A quantity that an estimate could not see, named in an expression by what
/// is unknown, so that every path that reaches the same unknown quantity
/// shares the same term.
struct Unknown {
  enum class Kind {
    /// The frame of a function without a frame size: frame(f).
    Frame,
    /// The stack beyond a function without call graph information, that is
    /// its callees, and its own frame when that is unknown too: calls(f).
    Calls,
    /// The further invocations of a recursive component: cycle(f, g).
    Cycle,
    /// The stack beyond an indirect call whose targets are not all known:
    /// indirect(type 0x1234) when no function has the type of the call, and
    /// indirect(unknown type) for the indirect call targets of unknown type
    /// in the file, which any indirect call may reach.
    Indirect,
  };
  Kind K;
  /// The function, for Frame and Calls.
  const Function *F = nullptr;
  /// The component, as an index into StackSizeEstimator::cycles(), for Cycle.
  unsigned Cycle = 0;
  /// The type ID of the call, for Indirect, with 0 standing for unknown type
  /// as in the call graph section.
  uint64_t TypeId = 0;
};

/// Formats \p Terms as "max(term, ...)", or as the term itself when there is
/// one, with \p Names the name of each unknown by index.
std::string formatStackUsageExpression(ArrayRef<StackUsageTerm> Terms,
                                       ArrayRef<std::string> Names);

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

  /// The stack usage with the unknowns kept as terms.
  StackUsageExpression Expression;

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

  /// The unknowns of the expressions, by index.
  ArrayRef<Unknown> unknowns() const { return Unknowns; }

  /// The names of the unknowns, by index: frame(f), calls(f), cycle(f, g)
  /// naming at most three members, indirect(type 0x1234) or
  /// indirect(unknown type).
  ArrayRef<std::string> unknownNames() const { return UnknownNames; }

  /// Formats \p E for a report, as "max(80, 16 + calls(<external>))".
  std::string formatExpression(const StackUsageExpression &E) const {
    return formatStackUsageExpression(E.Terms, UnknownNames);
  }

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
    /// The usage of the paths starting in this component.
    StackUsageExpression Expression;
  };

  unsigned internUnknown(const Unknown &U);
  void computeTerms(Component &C);

  const StackSizeGraph &G;
  EstimatorOptions Opts;
  std::vector<Component> Components;
  DenseMap<const Function *, unsigned> ComponentOf;
  std::vector<ArrayRef<const Function *>> Cycles;
  std::vector<Unknown> Unknowns;
  std::vector<std::string> UnknownNames;
  /// The index of each unknown by its kind and its function address, cycle
  /// or type ID.
  DenseMap<std::pair<unsigned, uint64_t>, unsigned> UnknownIds;
  /// The callers of every function without a call graph record, so that an
  /// estimate finds them without scanning everything it reaches.
  DenseMap<const Function *, SmallVector<const Function *, 2>>
      CallersOfUninstrumented;
};

} // namespace stacksize
} // namespace llvm

#endif // LLVM_TOOLS_LLVM_STACK_SIZE_LIB_STACKSIZEESTIMATOR_H
