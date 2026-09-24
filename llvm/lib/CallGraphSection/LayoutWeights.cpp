//===- LayoutWeights.cpp - Call graph edge weighting for layout -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/LayoutWeights.h"
#include "llvm/ADT/DenseMap.h"
#include <algorithm>
#include <utility>

using namespace llvm;
using namespace llvm::callgraph;

std::vector<WeightedEdge>
llvm::callgraph::computeLayoutEdges(const CallGraph &Graph,
                                    const LayoutWeightConfig &Config) {
  std::vector<WeightedEdge> Edges;
  // Used for accumulation only; never iterated, so it does not affect
  // determinism. Edge order follows node order, and targets are sorted.
  DenseMap<std::pair<NodeId, NodeId>, size_t> EdgeIndex;

  auto addEdge = [&](NodeId From, NodeId To, uint64_t Weight) {
    if (From == To || Weight == 0)
      return;
    if (!Graph.isValid(To) || !Graph[To].IsExecutable)
      return;
    auto Key = std::make_pair(From, To);
    auto It = EdgeIndex.find(Key);
    if (It != EdgeIndex.end()) {
      Edges[It->second].Weight += Weight;
      return;
    }
    EdgeIndex[Key] = Edges.size();
    Edges.push_back({From, To, Weight});
  };

  for (const FunctionNode &Caller : Graph.nodes()) {
    if (!Caller.IsExecutable)
      continue;

    for (NodeId Callee : Caller.DirectCallees)
      addEdge(Caller.Id, Callee, Config.DirectCallWeight);

    for (const IndirectCallSite &Site : Graph.indirectCallSites(Caller.Id)) {
      const size_t FanOut = Site.Targets.size();
      if (FanOut == 0 || FanOut > Config.MaxIndirectFanOut)
        continue;

      // Weight per candidate is BaseWeight / FanOut^Attenuation, computed with
      // saturation so that an aggressive attenuation setting cannot overflow.
      uint64_t Denominator = 1;
      for (uint32_t I = 0; I != Config.FanOutAttenuation; ++I) {
        if (Denominator > Config.IndirectCallBaseWeight)
          break;
        Denominator *= FanOut;
      }
      const uint64_t Weight = std::max<uint64_t>(
          1, Config.IndirectCallBaseWeight / std::max<uint64_t>(Denominator, 1));

      for (NodeId Target : Site.Targets)
        addEdge(Caller.Id, Target, Weight);
    }
  }

  return Edges;
}
