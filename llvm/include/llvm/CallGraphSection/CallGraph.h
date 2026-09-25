//===- CallGraph.h - Reconstructed whole-program call graph -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A whole-program call graph reconstructed from the per-module
// `.llvm.callgraph` metadata emitted under `-fcall-graph-section`.
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
//      fan-out remains visible to consumers. A candidate set depends only on
//      the signature and, if the caller's module has module-local candidates
//      for it, on that module, so call sites share one copy of each distinct
//      set (see TargetSet nodes below).
//   4. Adjacency and reverse index, including self-edges, so that recursion
//      and strongly connected components are observable (a stack-depth
//      estimator needs this; a layout pass simply ignores self-edges).
//
// Shared candidate sets
// ---------------------
//
// Large programs have many indirect call sites but few distinct candidate
// sets (for example, every virtual call through one vtable slot type has the
// same candidates). Listing each set in every caller's adjacency would make
// the graph O(sites x candidates). Instead, build() creates one TargetSet
// pseudo node per distinct non-empty set: a function calls the TargetSet node
// of each of its indirect call sites, and the TargetSet node calls every
// candidate in the set. Paths, and hence reachability and strongly connected
// components of function nodes, are exactly those of the expanded graph; a
// TargetSet node is merely a relay and executes no code.
//
// Partial coverage
// ----------------
//
// Not every function in a program is described by a record: hand-written
// assembly, objects built without -fcall-graph-section, and code from other
// compilers have none. Such a function's outgoing calls are *unknown*, not
// absent, and without a type ID it cannot be matched by signature. The graph
// models this soundly with three pseudo nodes, appended after the function
// nodes, in the style of llvm::CallGraph's external nodes:
//
//   - ExternalCallingNode: calls every root registered with addRoot()
//     (program entry points, exported functions, ...). It is the GraphTraits
//     entry node, so a depth-first walk from it yields every function that may
//     execute.
//   - UnknownCalleeNode: stands for "any code the graph cannot see". It is
//     called by every function without a record and by every function marked
//     CallsUnknown (for example because it calls into a shared library), and
//     it calls every address-taken function, since unknown code may invoke any
//     function pointer it can obtain.
//   - UntypedTargetsNode: calls every address-taken function that cannot be
//     excluded from an indirect call site by type: it has no (complete)
//     record, is not claimed as an indirect target, or has no type ID. It is
//     called by every function with an indirect call site.
//
// Pseudo-node edges appear only in callees()/callers() (and hence GraphTraits).
// DirectCallees and IndirectCallSite::Targets stay exact, and pseudo nodes are
// non-executable, so layout policies built on those are unaffected. Clients
// that need a sound over-approximation (reachability, stack depth) must treat
// UnknownCalleeNode as "anything may happen here", and every pseudo node as
// having no frame of its own. In particular, callers() of an indirect call
// target lists the TargetSet nodes that contain it; the functions calling
// through them are the callers() of those TargetSet nodes.
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

/// Distinguishes real functions from the pseudo nodes described in the file
/// comment.
enum class NodeKind : uint8_t {
  Function,
  ExternalCalling,
  UnknownCallee,
  UntypedTargets,
  /// Relay to the candidates of one distinct indirect call target set.
  TargetSet,
};

/// One function in the reconstructed call graph.
struct FunctionNode {
  /// Index of this node; also its position in CallGraph::nodes().
  NodeId Id = InvalidNodeId;

  /// Set by build(). Clients only ever add NodeKind::Function nodes.
  NodeKind Kind = NodeKind::Function;

  /// Module this function was defined in. Used to scope internal-linkage
  /// indirect targets.
  ModuleId Module = InvalidModuleId;

  /// Generalized function type IDs under which this function may be called
  /// indirectly. Usually one; a node that stands for several functions (for
  /// example a section without -ffunction-sections) may have several. Empty
  /// if unknown.
  SmallVector<uint64_t, 1> TypeIds;

  /// A `.llvm.callgraph` record describes this function, so its direct callees
  /// and indirect call signatures are complete. When false, the function's
  /// behavior is unknown: build() connects it to UnknownCalleeNode, and it is
  /// a candidate for every indirect call site if its address is taken.
  /// DirectCallees may still be supplied (e.g. from relocations); they are
  /// kept, but are not assumed to be exhaustive.
  bool HasRecord = false;

  /// The producing module believed this function may be called indirectly,
  /// under TypeIds. This is an over-approximation until build() cross-checks
  /// it against whole-program address-taken evidence. For a node standing for
  /// several functions it must hold for all of them; otherwise leave it false
  /// and the node is treated as untyped (see UntypedTargetsNode).
  bool IsIndirectTarget = false;

  /// The function has non-local linkage (visible outside its module).
  bool IsExternal = false;

  /// The function occupies executable storage. Non-executable nodes are
  /// retained but are never treated as call targets.
  bool IsExecutable = true;

  /// The function may call code that is not represented in the graph, for
  /// example a function in a shared library or an IFUNC whose implementation
  /// is chosen at run time. build() connects it to UnknownCalleeNode.
  bool CallsUnknown = false;

  /// Direct callees, as supplied by the client. May contain duplicates and
  /// self-references; build() canonicalizes them.
  SmallVector<NodeId, 4> DirectCallees;

  /// Type IDs of the signatures this function calls indirectly.
  SmallVector<uint64_t, 2> IndirectCalleeTypeIds;

  /// Functions this one may transfer control to without calling them, for
  /// example by branching to the cold part of a function split by
  /// -fsplit-machine-functions or to another basic block section. Records
  /// list calls only, so clients supply these from relocations. They are
  /// part of callees() (and hence reachability), but not of DirectCallees,
  /// so layout policies do not pull such targets towards this function.
  SmallVector<NodeId, 0> JumpTargets;

  /// @name Populated by CallGraphBuilder::build()
  /// @{

  /// True if whole-program evidence confirms the address is taken somewhere.
  bool IsAddressTaken = false;

  /// Deduplicated union of direct callees, jump targets and pseudo-node edges
  /// (including the TargetSet node of each indirect call site with
  /// candidates). Self-edges are preserved. This is the adjacency used by
  /// GraphTraits.
  SmallVector<NodeId, 4> Callees;

  /// Reverse of Callees.
  SmallVector<NodeId, 4> Callers;

  /// Range of this node's entries in CallGraph's indirect call site pool.
  uint32_t FirstSite = 0;
  uint32_t NumSites = 0;

  /// @}

  bool isPseudo() const { return Kind != NodeKind::Function; }
};

/// The candidate target set of one indirect call signature used by a function.
struct IndirectCallSite {
  /// Generalized type ID of the callee signature.
  uint64_t TypeId = 0;

  /// Candidate targets matched by type, sorted and deduplicated.
  /// `Targets.size()` is the fan-out of this call site, which consumers use as
  /// a confidence signal. Functions that cannot be excluded by type (see
  /// UntypedTargetsNode) are not listed here.
  ArrayRef<NodeId> Targets;

  /// The TargetSet pseudo node relaying to Targets, shared by every call site
  /// with the same candidate set. InvalidNodeId if Targets is empty.
  NodeId TargetSetNode = InvalidNodeId;
};

/// How much of the program the records describe. Counts function nodes only.
struct CoverageSummary {
  /// Function nodes, i.e. excluding pseudo nodes.
  size_t NumFunctions = 0;
  /// Executable functions described by a record.
  size_t NumWithRecord = 0;
  /// Executable functions without a record.
  size_t NumWithoutRecord = 0;
  /// Functions connected to UnknownCalleeNode (no record, or CallsUnknown).
  size_t NumCallingUnknown = 0;
  /// Callees of UnknownCalleeNode: address-taken executable functions.
  size_t NumAddressTaken = 0;
  /// Callees of UntypedTargetsNode.
  size_t NumUntypedAddressTaken = 0;
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

  /// All nodes: function nodes first (NodeIds as assigned by the builder),
  /// followed by the pseudo nodes.
  ArrayRef<FunctionNode> nodes() const { return Nodes; }
  size_t size() const { return Nodes.size(); }
  bool empty() const { return Nodes.empty(); }
  bool isValid(NodeId N) const { return N < Nodes.size(); }

  /// Function nodes only, i.e. the nodes the client added.
  ArrayRef<FunctionNode> functions() const {
    return ArrayRef<FunctionNode>(Nodes).take_front(NumFunctions);
  }
  size_t numFunctions() const { return NumFunctions; }
  bool isFunction(NodeId N) const { return N < NumFunctions; }

  /// Pseudo nodes. See the file comment. TargetSet nodes follow these three.
  NodeId externalCallingNode() const { return NumFunctions; }
  NodeId unknownCalleeNode() const { return NumFunctions + 1; }
  NodeId untypedTargetsNode() const { return NumFunctions + 2; }

  const FunctionNode &operator[](NodeId N) const { return Nodes[N]; }

  /// Direct callees only, canonicalized (sorted, deduplicated).
  ArrayRef<NodeId> directCallees(NodeId N) const {
    return Nodes[N].DirectCallees;
  }

  /// Union of direct callees, jump targets and pseudo-node edges, including
  /// self-edges. Indirect candidates are reached through TargetSet nodes.
  ArrayRef<NodeId> callees(NodeId N) const { return Nodes[N].Callees; }

  /// Inverse of callees().
  ArrayRef<NodeId> callers(NodeId N) const { return Nodes[N].Callers; }

  /// Indirect call sites of \p N, one per distinct callee signature.
  ArrayRef<IndirectCallSite> indirectCallSites(NodeId N) const {
    const FunctionNode &Node = Nodes[N];
    return ArrayRef<IndirectCallSite>(Sites).slice(Node.FirstSite,
                                                   Node.NumSites);
  }

  const CoverageSummary &coverage() const { return Coverage; }

private:
  friend class CallGraphBuilder;

  std::vector<FunctionNode> Nodes;
  std::vector<IndirectCallSite> Sites;
  std::vector<NodeId> TargetPool;
  size_t NumFunctions = 0;
  CoverageSummary Coverage;
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

  /// Record that \p N may be invoked from outside the graph without being
  /// called by any function in it: a program entry point, an exported
  /// function, a constructor, etc. Roots become callees of
  /// ExternalCallingNode.
  void addRoot(NodeId N);

  /// Resolve indirect call sites and materialize the graph. The builder may
  /// be discarded afterwards.
  CallGraph build();

private:
  std::vector<FunctionNode> Nodes;
  DenseSet<NodeId> GloballyAddressTaken;
  DenseMap<ModuleId, DenseSet<NodeId>> LocallyAddressTaken;
  SmallVector<NodeId, 0> Roots;
};

/// Maps an adjacency entry to its node pointer. Implementation detail of
/// GraphTraits.
struct NodeIdToPointer {
  const FunctionNode *Base = nullptr;
  const FunctionNode *operator()(NodeId Id) const { return Base + Id; }
};

} // namespace callgraph

/// The entry node is ExternalCallingNode, so graph walks start from the roots.
template <> struct GraphTraits<const callgraph::CallGraph *> {
  using NodeRef = const callgraph::FunctionNode *;
  using ChildIteratorType =
      mapped_iterator<const callgraph::NodeId *, callgraph::NodeIdToPointer>;
  using nodes_iterator = pointer_iterator<const callgraph::FunctionNode *>;

  static NodeRef getEntryNode(const callgraph::CallGraph *G) {
    return G->nodes().begin() + G->externalCallingNode();
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
