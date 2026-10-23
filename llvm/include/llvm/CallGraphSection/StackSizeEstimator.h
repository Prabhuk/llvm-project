//===- StackSizeEstimator.h - Whole-program stack usage estimation --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Condenses an ELFCallGraph into Strongly Connected Components (SCCs) via an
/// iterative Tarjan's algorithm and computes worst-case stack usage, maximal
/// call paths, algebraic lower-bound expressions, and reachability diagnostics
/// for entry points.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CALLGRAPHSECTION_STACKSIZEESTIMATOR_H
#define LLVM_CALLGRAPHSECTION_STACKSIZEESTIMATOR_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CallGraphSection/ELFCallGraph.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llvm::callgraph {

/// A single term in a symbolic stack usage expression:
/// `ConstBytes + Unknowns[0] + Unknowns[1] + ...`.
struct StackUsageTerm {
  uint64_t ConstBytes = 0;
  SmallVector<std::string, 2> Unknowns;

  bool operator==(const StackUsageTerm &Other) const {
    return ConstBytes == Other.ConstBytes && Unknowns == Other.Unknowns;
  }
};

/// A function frame along a call path or diagnostic list.
struct StackFrameEntry {
  uint64_t Address = 0;
  std::string Name;
  std::optional<uint64_t> StackSize;
};

/// An uninstrumented or external callee and the instrumented functions that
/// call it.
struct UninstrumentedCalleeInfo {
  uint64_t TargetAddress = 0;
  std::string TargetName;
  SmallVector<StackFrameEntry, 2> Callers;
};

/// A caller with one or more unresolved indirect call target type IDs.
struct UnresolvedIndirectCallInfo {
  uint64_t CallerAddress = 0;
  std::string CallerName;
  SmallVector<uint64_t, 2> TypeIds;
};

/// Strongly Connected Component (SCC) in the condensed call graph.
struct SCCNode {
  unsigned Id = 0;
  SmallVector<uint64_t, 1> MemberAddresses;
  SmallVector<unsigned, 2> SuccessorSCCs;
  bool IsCycle = false;
  uint64_t LocalStackUsage = 0;
  uint64_t MaxStackUsage = 0;
  std::optional<unsigned> NextSCCOnMaxPath;
  SmallVector<StackUsageTerm, 2> StackTerms;
};

/// Complete stack usage estimate for a single entry point.
struct EntryPointEstimate {
  std::string Name;
  uint64_t Address = 0;
  bool HasCallGraph = true;
  uint64_t MaxStackUsage = 0;
  std::string StackUsageExpr;
  bool IsLowerBound = false;
  SmallVector<StackFrameEntry, 8> MaxStackPath;
  SmallVector<SmallVector<StackFrameEntry, 2>, 0> Cycles;
  SmallVector<StackFrameEntry, 0> UnknownStackSizes;
  SmallVector<UninstrumentedCalleeInfo, 0> UninstrumentedCallees;
  SmallVector<UnresolvedIndirectCallInfo, 0> UnresolvedIndirectCalls;
  SmallVector<std::string, 0> LowerBoundReasons;
};

/// Options for entry point selection and stack estimation.
struct StackEstimateOptions {
  /// Entry point specifications: function symbol names, glob patterns (`*`,
  /// `?`, `[`), or integer addresses (`0x...`). When empty, defaults to the
  /// ELF entry point (`e_entry`) and all exported `STT_FUNC` symbols in
  /// `.dynsym`.
  std::vector<std::string> EntryPoints;
};

/// Complete stack estimation report for an ELF binary.
struct StackEstimateReport {
  std::string BinaryPath;
  uint64_t CallFrameOverhead = 0;
  uint64_t RedZoneSize = 0;
  SmallVector<std::string, 0> SymbolizerMarkupContext;
  SmallVector<SCCNode, 0> SCCs;
  DenseMap<uint64_t, unsigned> AddressToSCC;
  SmallVector<StackFrameEntry, 0> UnknownTypeIndirectTargets;
  SmallVector<EntryPointEstimate, 0> EntryPoints;
};

/// Policy constraints for verifying entry-point stack budgets and leaf
/// functions.
struct StackBudgetPolicy {
  /// Default maximum stack usage in bytes for all entry points without a
  /// per-function limit override.
  std::optional<uint64_t> MaxStackUsage;

  /// Per-entry-point stack usage limits (`<function-name>`, `<bytes>`).
  SmallVector<std::pair<std::string, uint64_t>, 0> Limits;

  /// Entry points required to be leaf functions (making zero calls).
  SmallVector<std::string, 0> LeafFunctions;

  bool empty() const {
    return !MaxStackUsage.has_value() && Limits.empty() &&
           LeafFunctions.empty();
  }
};

/// Verify that \p Report and \p CG satisfy all stack budget and leaf
/// constraints in \p Policy. Returns a list of human-readable error messages
/// (empty if verification succeeds).
LLVM_ABI SmallVector<std::string, 0>
verifyStackBudgets(const ELFCallGraph &CG, const StackEstimateReport &Report,
                   const StackBudgetPolicy &Policy);

/// Estimate stack usage across the given \p CG according to \p Opts.
LLVM_ABI Expected<StackEstimateReport>
estimateStackSizes(const ELFCallGraph &CG,
                   const StackEstimateOptions &Opts = {},
                   StringRef BinaryPath = "");

/// Format a list of Pareto-pruned symbolic stack usage terms into a human-
/// readable algebraic expression (e.g. `"48"`, `"32 + calls(<external>)"`, or
/// `"max(64 + calls(foo), 32 + frame(bar))"`).
LLVM_ABI std::string formatStackUsageExpr(ArrayRef<StackUsageTerm> Terms);

/// Convert a StackEstimateReport to structured JSON matching the Fuchsia/LLVM
/// stack size report schema.
LLVM_ABI json::Value toJSON(const StackEstimateReport &Report);

/// Render a human-readable stack estimation report (including symbolizer
/// markup context and per-entry-point call paths and lower-bound diagnostics)
/// to \p OS.
LLVM_ABI void formatTextReport(const StackEstimateReport &Report,
                               raw_ostream &OS);

} // namespace llvm::callgraph

#endif // LLVM_CALLGRAPHSECTION_STACKSIZEESTIMATOR_H
