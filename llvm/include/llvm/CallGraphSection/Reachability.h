//===- Reachability.h - Call graph reachability from roots ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Computes which functions of a reconstructed CallGraph may execute, starting
// from the roots registered with CallGraphBuilder::addRoot().
//
// The walk follows CallGraph::callees(), which includes the pseudo-node edges
// (see CallGraph.h). A function reported unreachable is therefore not called
// directly by, and cannot be a type-compatible indirect target of, any
// function that may execute, and code without a record cannot reach it
// either.
//
// What the result is sound with respect to
// ----------------------------------------
//
// Records describe *calls*. Control can also enter a function through
// transfers that are not calls and that no record describes: an indirect
// branch through a jump table into another section (basic block sections), a
// landing pad entered by the unwinder, or a function that the loader or
// kernel invokes from a pointer it was handed. Address-taken functions that
// cannot be excluded by type (the callees of UntypedTargetsNode) are treated
// as roots by default, since nothing is known about how they are entered.
//
// The remaining gap is a *typed*, address-taken function entered by code
// outside the program without going through the graph -- for example a
// signal handler registered through a system call issued by inline assembly
// in a function that does have a record. Clients must supply such entry
// points as roots. Layout clients can accept the gap (misclassification costs
// performance, never correctness); a stack-depth estimator must not.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CALLGRAPHSECTION_REACHABILITY_H
#define LLVM_CALLGRAPHSECTION_REACHABILITY_H

#include "llvm/ADT/BitVector.h"
#include "llvm/CallGraphSection/CallGraph.h"

namespace llvm {
namespace callgraph {

/// Tuning of computeReachable().
struct ReachabilityOptions {
  /// Also start the walk from every address-taken function that no indirect
  /// call site can exclude by type (the callees of UntypedTargetsNode), in
  /// addition to the roots. See the file comment.
  bool UntypedAddressTakenAreRoots = true;

  /// Further nodes to start the walk from, for example functions observed
  /// executing in a measured profile. These are known to run, so everything
  /// they may call is reachable too.
  ArrayRef<NodeId> AdditionalRoots;
};

/// Returns a bit vector with one bit per node of \p Graph (function and
/// pseudo nodes), set for every node reachable from ExternalCallingNode.
BitVector computeReachable(const CallGraph &Graph,
                           const ReachabilityOptions &Options = {});

} // namespace callgraph
} // namespace llvm

#endif // LLVM_CALLGRAPHSECTION_REACHABILITY_H
