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

CallGraph CallGraphBuilder::build() {
  CallGraph G;
  G.Nodes = std::move(Nodes);
  Nodes.clear();

  // Step 1 & 2: verify the producer's indirect-target claims against
  // whole-program address-taken evidence, and index the survivors either
  // globally or per module depending on whether their address escapes.
  DenseMap<uint64_t, SmallVector<NodeId, 4>> GlobalTargets;
  DenseMap<ModuleId, DenseMap<uint64_t, SmallVector<NodeId, 4>>> LocalTargets;

  for (FunctionNode &N : G.Nodes) {
    const bool GloballyTaken = GloballyAddressTaken.contains(N.Id);
    bool LocallyTaken = false;
    auto ModuleIt = LocallyAddressTaken.find(N.Module);
    if (ModuleIt != LocallyAddressTaken.end())
      LocallyTaken = ModuleIt->second.contains(N.Id);

    N.IsAddressTaken = GloballyTaken || LocallyTaken;

    if (!N.IsExecutable || !N.IsIndirectTarget)
      continue;

    // An external function is marked as an indirect target by every producer
    // that cannot see the whole program. Require real evidence.
    if (N.IsExternal ? !GloballyTaken : !N.IsAddressTaken)
      continue;

    if (N.IsExternal || GloballyTaken)
      GlobalTargets[N.TypeId].push_back(N.Id);
    else
      LocalTargets[N.Module][N.TypeId].push_back(N.Id);
  }

  for (auto &Entry : GlobalTargets)
    sortUnique(Entry.second);
  for (auto &ModuleEntry : LocalTargets)
    for (auto &Entry : ModuleEntry.second)
      sortUnique(Entry.second);

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

  // Step 4: adjacency and its inverse. Self-edges are kept so that recursion
  // is visible; layout consumers drop them.
  for (FunctionNode &N : G.Nodes) {
    SmallVector<NodeId, 8> All(N.DirectCallees.begin(), N.DirectCallees.end());
    for (const IndirectCallSite &Site : G.indirectCallSites(N.Id))
      All.append(Site.Targets.begin(), Site.Targets.end());
    sortUnique(All);
    N.Callees.assign(All.begin(), All.end());
  }

  for (const FunctionNode &N : G.Nodes)
    for (NodeId Callee : N.Callees)
      G.Nodes[Callee].Callers.push_back(N.Id);
  for (FunctionNode &N : G.Nodes)
    sortUnique(N.Callers);

  return G;
}
