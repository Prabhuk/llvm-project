//===- CallGraph.h - Reconstructed whole-program call graph -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A whole-program call graph reconstructed from the per-module `.llvm.callgraph`
// metadata emitted under `-fcall-graph-section`.
//
// This header models *facts* only: which functions exist, who calls whom
// directly, and which functions are plausible targets of each indirect call
// signature. It deliberately contains no layout policy (edge weights, fan-out
// caps, attenuation); see LayoutWeights.h for one such policy.
//
// Clients are identity-agnostic: a "function" is an opaque `NodeId` that the
// client maps to its own representation (LLD maps to `InputSectionBase *`,
// BOLT to `BinaryFunction *`, an offline tool to a symbol). The builder is fed
// facts the client alone can establish (module membership, linkage, and
// address-taken evidence from relocations) and returns a queryable graph.
//
// Reconstruction performed by CallGraphBuilder::build():
//
//   1. Address-taken verification. The compiler conservatively marks every
//      non-local-linkage function as a potential indirect target because, per
//      module, it cannot prove otherwise. Whole-program relocation evidence
//      supplied via addAddressTakenFact() prunes targets whose address is
//      never actually taken anywhere in the program.
//   2. Scoped candidate maps. Internal-linkage functions whose address never
//      escapes their module can only be called indirectly from within that
//      module, so they are indexed per-module; everything else is indexed
//      globally. This prevents same-signature statics in unrelated modules
//      from being treated as interchangeable.
//   3. Per-call-site resolution. Each indirect signature used by a function
//      resolves to a candidate target set, retained per signature so that
//      fan-out remains visible to consumers.
//   4. Adjacency and reverse index, including self-edges, so that recursion
//      and strongly connected components are observable (a stack-depth
//      estimator needs this; a layout pass simply ignores self-edges).
//
// `GraphTraits` is specialized for `const CallGraph *`, so `df_iterator`,
// `po_iterator` and `scc_iterator` work directly on the result.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CALLGRAPHSECTION_CALLGRAPH_H
#define LLVM_CALLGRAPHSECTION_CALLGRAPH_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include <cstdint>
#include <vector>

namespace llvm {
namespace callgraph {

/// Opaque identifier of a function in a CallGraph.
using NodeId = uint32_t;
inline constexpr NodeId InvalidNodeId = ~NodeId(0);

/// Opaque identifier of a module (translation unit / input object file).
using ModuleId = uint32_t;
inline constexpr ModuleId InvalidModuleId = 0;

/// One function in the reconstructed call graph.
struct FunctionNode {
  /// Index of this node; also its position in CallGraph::nodes().
  NodeId Id = InvalidNodeId;

  /// Module this function was defined in. Used to scope internal-linkage
  /// indirect targets.
  ModuleId Module = InvalidModuleId;

  /// Generalized function type ID, or 0 if unknown.
  uint64_t TypeId = 0;

  /// The producing module believed this function may be called indirectly.
  /// This is an over-approximation until build() cross-checks it against
  /// whole-program address-taken evidence.
  bool IsIndirectTarget = false;

  /// The function has non-local linkage (visible outside its module).
  bool IsExternal = false;

  /// The function occupies executable storage. Non-executable nodes are
  /// retained but are never treated as call targets.
  bool IsExecutable = true;

  /// Direct callees, as supplied by the client. May contain duplicates and
  /// self-references; build() canonicalizes them.
  SmallVector<NodeId, 4> DirectCallees;

  /// Type IDs of the signatures this function calls indirectly.
  SmallVector<uint64_t, 2> IndirectCalleeTypeIds;

  /// @name Populated by CallGraphBuilder::build()
  /// @{

  /// True if whole-program evidence confirms the address is taken somewhere.
  bool IsAddressTaken = false;

  /// Deduplicated union of direct callees and resolved indirect candidates.
  /// Self-edges are preserved. This is the adjacency used by GraphTraits.
  SmallVector<NodeId, 4> Callees;

  /// Reverse of Callees.
  SmallVector<NodeId, 4> Callers;

  /// Range of this node's entries in CallGraph's indirect call site pool.
  uint32_t FirstSite = 0;
  uint32_t NumSites = 0;

  /// @}
};

/// The candidate target set of one indirect call signature used by a function.
struct IndirectCallSite {
  /// Generalized type ID of the callee signature.
  uint64_t TypeId = 0;

  /// Candidate targets, sorted and deduplicated. `Targets.size()` is the
  /// fan-out of this call site, which consumers use as a confidence signal.
  ArrayRef<NodeId> Targets;
};

/// A reconstructed whole-program call graph. Produced by CallGraphBuilder.
///
/// Move-only: indirect call sites reference an internal pool by ArrayRef.
/// Moving is safe (the pools' heap buffers are transferred); copying is not,
/// so it is disallowed.
class CallGraph {
public:
  CallGraph() = default;
  CallGraph(CallGraph &&) = default;
  CallGraph &operator=(CallGraph &&) = default;
  CallGraph(const CallGraph &) = delete;
  CallGraph &operator=(const CallGraph &) = delete;

  ArrayRef<FunctionNode> nodes() const { return Nodes; }
  size_t size() const { return Nodes.size(); }
  bool empty() const { return Nodes.empty(); }
  bool isValid(NodeId N) const { return N < Nodes.size(); }

  const FunctionNode &operator[](NodeId N) const { return Nodes[N]; }

  /// Direct callees only, canonicalized (sorted, deduplicated).
  ArrayRef<NodeId> directCallees(NodeId N) const {
    return Nodes[N].DirectCallees;
  }

  /// Union of direct callees and resolved indirect candidates, including
  /// self-edges.
  ArrayRef<NodeId> callees(NodeId N) const { return Nodes[N].Callees; }

  /// Inverse of callees().
  ArrayRef<NodeId> callers(NodeId N) const { return Nodes[N].Callers; }

  /// Indirect call sites of \p N, one per distinct callee signature.
  ArrayRef<IndirectCallSite> indirectCallSites(NodeId N) const {
    const FunctionNode &Node = Nodes[N];
    return ArrayRef<IndirectCallSite>(Sites).slice(Node.FirstSite,
                                                   Node.NumSites);
  }

private:
  friend class CallGraphBuilder;

  std::vector<FunctionNode> Nodes;
  std::vector<IndirectCallSite> Sites;
  std::vector<NodeId> TargetPool;
};

/// Accumulates facts about functions and their references, then reconstructs
/// a CallGraph. See the file comment for the algorithm.
class CallGraphBuilder {
public:
  /// Register a function and return its assigned NodeId.
  NodeId addFunction(FunctionNode Node);

  FunctionNode &getFunction(NodeId N) { return Nodes[N]; }
  const FunctionNode &getFunction(NodeId N) const { return Nodes[N]; }

  size_t size() const { return Nodes.size(); }

  /// Record whole-program evidence that the address of \p Target is taken.
  ///
  /// \param SourceModule Module containing the reference.
  /// \param IsGlobalEscape True when the address becomes observable outside
  ///        \p SourceModule -- for example when it is stored into an exported
  ///        table, or referenced from a different module. Such a target may be
  ///        invoked indirectly from anywhere; otherwise the evidence only
  ///        applies within \p SourceModule.
  void addAddressTakenFact(NodeId Target, ModuleId SourceModule,
                           bool IsGlobalEscape);

  /// Resolve indirect call sites and materialize the graph. The builder may
  /// be discarded afterwards.
  CallGraph build();

private:
  std::vector<FunctionNode> Nodes;
  DenseSet<NodeId> GloballyAddressTaken;
  DenseMap<ModuleId, DenseSet<NodeId>> LocallyAddressTaken;
};

/// Maps an adjacency entry to its node pointer. Implementation detail of
/// GraphTraits.
struct NodeIdToPointer {
  const FunctionNode *Base = nullptr;
  const FunctionNode *operator()(NodeId Id) const { return Base + Id; }
};

} // namespace callgraph

template <> struct GraphTraits<const callgraph::CallGraph *> {
  using NodeRef = const callgraph::FunctionNode *;
  using ChildIteratorType =
      mapped_iterator<const callgraph::NodeId *, callgraph::NodeIdToPointer>;
  using nodes_iterator = pointer_iterator<const callgraph::FunctionNode *>;

  static NodeRef getEntryNode(const callgraph::CallGraph *G) {
    return G->nodes().begin();
  }

  static ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N->Callees.begin(),
                             callgraph::NodeIdToPointer{N - N->Id});
  }
  static ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType(N->Callees.end(),
                             callgraph::NodeIdToPointer{N - N->Id});
  }

  static nodes_iterator nodes_begin(const callgraph::CallGraph *G) {
    return nodes_iterator(G->nodes().begin());
  }
  static nodes_iterator nodes_end(const callgraph::CallGraph *G) {
    return nodes_iterator(G->nodes().end());
  }
  static size_t size(const callgraph::CallGraph *G) { return G->size(); }
};

template <>
struct GraphTraits<callgraph::CallGraph *>
    : GraphTraits<const callgraph::CallGraph *> {};

} // namespace llvm

#endif // LLVM_CALLGRAPHSECTION_CALLGRAPH_H
