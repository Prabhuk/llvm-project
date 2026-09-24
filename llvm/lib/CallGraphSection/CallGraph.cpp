//===- CallGraph.cpp - Reconstructed whole-program call graph -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/CallGraph.h"
#include "llvm/ADT/STLExtras.h"
#include <utility>

using namespace llvm;
using namespace llvm::callgraph;

namespace {
/// Sort and deduplicate in place, giving deterministic output regardless of
/// the order in which facts were recorded.
template <typename VectorT> void sortUnique(VectorT &V) {
  llvm::sort(V);
  V.erase(llvm::unique(V), V.end());
}
} // namespace

NodeId CallGraphBuilder::addFunction(FunctionNode Node) {
  NodeId Id = static_cast<NodeId>(Nodes.size());
  Node.Id = Id;
  Node.Kind = NodeKind::Function;
  Nodes.push_back(std::move(Node));
  return Id;
}

void CallGraphBuilder::addAddressTakenFact(NodeId Target,
                                           ModuleId SourceModule,
                                           bool IsGlobalEscape) {
  if (IsGlobalEscape)
    GloballyAddressTaken.insert(Target);
  else
    LocallyAddressTaken[SourceModule].insert(Target);
}

void CallGraphBuilder::addRoot(NodeId N) { Roots.push_back(N); }

CallGraph CallGraphBuilder::build() {
  CallGraph G;
  G.Nodes = std::move(Nodes);
  Nodes.clear();
  G.NumFunctions = G.Nodes.size();

  // Step 1: whole-program address-taken evidence.
  for (NodeId N : GloballyAddressTaken)
    G.Nodes[N].IsAddressTaken = true;
  for (const auto &ModuleEntry : LocallyAddressTaken)
    for (NodeId N : ModuleEntry.second)
      G.Nodes[N].IsAddressTaken = true;

  // Step 2: verify the producer's indirect-target claims against that
  // evidence, and index the survivors either globally or in each module in
  // which their address is observable.
  DenseMap<uint64_t, SmallVector<NodeId, 4>> GlobalTargets;
  DenseMap<ModuleId, DenseMap<uint64_t, SmallVector<NodeId, 4>>> LocalTargets;

  auto isTypedTarget = [](const FunctionNode &N) {
    return N.IsExecutable && N.IsIndirectTarget;
  };

  for (const FunctionNode &N : G.Nodes) {
    // An external function is marked as an indirect target by every producer
    // that cannot see the whole program, so the claim alone is not evidence.
    if (isTypedTarget(N) && GloballyAddressTaken.contains(N.Id))
      for (uint64_t TypeId : N.TypeIds)
        GlobalTargets[TypeId].push_back(N.Id);
  }
  for (const auto &ModuleEntry : LocallyAddressTaken)
    for (NodeId N : ModuleEntry.second)
      if (isTypedTarget(G.Nodes[N]) && !GloballyAddressTaken.contains(N))
        for (uint64_t TypeId : G.Nodes[N].TypeIds)
          LocalTargets[ModuleEntry.first][TypeId].push_back(N);

  for (auto &Entry : GlobalTargets)
    sortUnique(Entry.second);
  for (auto &ModuleEntry : LocalTargets)
    for (auto &Entry : ModuleEntry.second)
      sortUnique(Entry.second);

  // Address-taken functions, and the subset that no indirect call site can
  // exclude by type: those without a complete record, not claimed as an
  // indirect target, or without a type ID.
  SmallVector<NodeId, 0> AddressTaken, UntypedAddressTaken;
  for (const FunctionNode &N : G.Nodes) {
    if (!N.IsExecutable || !N.IsAddressTaken)
      continue;
    AddressTaken.push_back(N.Id);
    if (!N.HasRecord || !N.IsIndirectTarget || N.TypeIds.empty())
      UntypedAddressTaken.push_back(N.Id);
  }

  // Step 3: resolve each indirect call site to its candidate set. Ranges are
  // recorded first and turned into ArrayRefs afterwards, once the pool has
  // stopped growing.
  struct SiteRange {
    uint64_t TypeId;
    uint32_t Begin;
    uint32_t Size;
  };
  std::vector<SiteRange> SiteRanges;

  for (FunctionNode &N : G.Nodes) {
    sortUnique(N.DirectCallees);
    sortUnique(N.IndirectCalleeTypeIds);

    N.FirstSite = static_cast<uint32_t>(SiteRanges.size());
    N.NumSites = 0;

    for (uint64_t TypeId : N.IndirectCalleeTypeIds) {
      SmallVector<NodeId, 8> Candidates;

      auto GlobalIt = GlobalTargets.find(TypeId);
      if (GlobalIt != GlobalTargets.end())
        Candidates.append(GlobalIt->second.begin(), GlobalIt->second.end());

      auto ModuleIt = LocalTargets.find(N.Module);
      if (ModuleIt != LocalTargets.end()) {
        auto LocalIt = ModuleIt->second.find(TypeId);
        if (LocalIt != ModuleIt->second.end())
          Candidates.append(LocalIt->second.begin(), LocalIt->second.end());
      }

      sortUnique(Candidates);

      // A site with no candidates is still recorded: "calls this signature,
      // targets unknown" is meaningful to consumers such as a stack-depth
      // estimator, which must treat it as unbounded.
      SiteRanges.push_back({TypeId, static_cast<uint32_t>(G.TargetPool.size()),
                            static_cast<uint32_t>(Candidates.size())});
      G.TargetPool.insert(G.TargetPool.end(), Candidates.begin(),
                          Candidates.end());
      ++N.NumSites;
    }
  }

  G.Sites.reserve(SiteRanges.size());
  for (const SiteRange &R : SiteRanges)
    G.Sites.push_back(
        IndirectCallSite{R.TypeId, ArrayRef<NodeId>(G.TargetPool)
                                       .slice(R.Begin, R.Size)});

  // Step 4: pseudo nodes. They are non-executable so that layout policies
  // never treat them as call targets.
  auto addPseudo = [&](NodeKind Kind) {
    FunctionNode P;
    P.Id = static_cast<NodeId>(G.Nodes.size());
    P.Kind = Kind;
    P.IsExecutable = false;
    P.HasRecord = true;
    G.Nodes.push_back(std::move(P));
  };
  addPseudo(NodeKind::ExternalCalling);
  addPseudo(NodeKind::UnknownCallee);
  addPseudo(NodeKind::UntypedTargets);

  const NodeId UnknownCallee = G.unknownCalleeNode();
  const NodeId UntypedTargets = G.untypedTargetsNode();

  // Step 5: adjacency and its inverse. Self-edges are kept so that recursion
  // is visible; layout consumers drop them.
  for (NodeId Id = 0; Id != G.NumFunctions; ++Id) {
    FunctionNode &N = G.Nodes[Id];
    SmallVector<NodeId, 8> All(N.DirectCallees.begin(), N.DirectCallees.end());
    All.append(N.JumpTargets.begin(), N.JumpTargets.end());
    for (const IndirectCallSite &Site : G.indirectCallSites(N.Id))
      All.append(Site.Targets.begin(), Site.Targets.end());
    if (N.IsExecutable) {
      // Any indirect call may reach a function that cannot be excluded by
      // type.
      if (N.NumSites != 0 && !UntypedAddressTaken.empty())
        All.push_back(UntypedTargets);
      // A function without a record may do anything; so may one that calls
      // code outside the graph.
      if (!N.HasRecord || N.CallsUnknown)
        All.push_back(UnknownCallee);
    }
    sortUnique(All);
    N.Callees.assign(All.begin(), All.end());
  }

  sortUnique(Roots);
  if (UnknownCodeIsRoot)
    Roots.push_back(UnknownCallee); // Pseudo nodes follow all functions.
  G.Nodes[G.externalCallingNode()].Callees.assign(Roots.begin(), Roots.end());
  G.Nodes[UnknownCallee].Callees.assign(AddressTaken.begin(),
                                        AddressTaken.end());
  G.Nodes[UntypedTargets].Callees.assign(UntypedAddressTaken.begin(),
                                         UntypedAddressTaken.end());

  for (const FunctionNode &N : G.Nodes)
    for (NodeId Callee : N.Callees)
      G.Nodes[Callee].Callers.push_back(N.Id);
  for (FunctionNode &N : G.Nodes)
    sortUnique(N.Callers);

  // Coverage summary.
  CoverageSummary &C = G.Coverage;
  C.NumFunctions = G.NumFunctions;
  for (const FunctionNode &N : G.functions()) {
    if (!N.IsExecutable)
      continue;
    ++(N.HasRecord ? C.NumWithRecord : C.NumWithoutRecord);
    if (!N.HasRecord || N.CallsUnknown)
      ++C.NumCallingUnknown;
  }
  C.NumAddressTaken = AddressTaken.size();
  C.NumUntypedAddressTaken = UntypedAddressTaken.size();

  return G;
}
