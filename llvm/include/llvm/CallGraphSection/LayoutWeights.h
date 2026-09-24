//===- LayoutWeights.h - Call graph edge weighting for layout ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Turns a reconstructed CallGraph into weighted edges suitable for a
// code-layout algorithm (LLD's hfsort/cdsort, BOLT's function reordering).
//
// This is *policy*, deliberately separated from the graph facts in
// CallGraph.h. Consumers that only need graph structure -- for example a
// stack-depth estimator -- must not depend on this header.
//
// The `.llvm.callgraph` metadata carries no execution counts, so these weights
// are a synthetic stand-in for a profile. The constants below are heuristics
// and are expected to be calibrated against measured profiles; they are
// meaningful only relative to each other.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CALLGRAPHSECTION_LAYOUTWEIGHTS_H
#define LLVM_CALLGRAPHSECTION_LAYOUTWEIGHTS_H

#include "llvm/CallGraphSection/CallGraph.h"
#include <cstdint>
#include <vector>

namespace llvm {
namespace callgraph {

/// Tuning parameters for synthetic edge weights.
struct LayoutWeightConfig {
  /// Weight of a direct call edge.
  uint64_t DirectCallWeight = 100000;

  /// Base weight distributed among the candidates of an indirect call site.
  uint64_t IndirectCallBaseWeight = 50000;

  /// Indirect call sites resolving to more than this many candidates carry no
  /// useful locality signal and are dropped. A generic signature such as
  /// `void(void)` can match thousands of functions; synthesizing a clique of
  /// that size costs time and misleads the layout algorithm.
  uint32_t MaxIndirectFanOut = 64;

  /// Exponent applied to fan-out when attenuating indirect edge weight: the
  /// per-candidate weight is IndirectCallBaseWeight / N^FanOutAttenuation.
  /// 1 models a uniform choice among N candidates; larger values additionally
  /// distrust high fan-out signatures.
  uint32_t FanOutAttenuation = 2;
};

/// A synthesized weighted call graph edge.
struct WeightedEdge {
  NodeId From = InvalidNodeId;
  NodeId To = InvalidNodeId;
  uint64_t Weight = 0;
};

/// Synthesize weighted edges for code layout.
///
/// Self-edges and edges to non-executable nodes are omitted: they carry no
/// placement information. Parallel edges are accumulated. The result is
/// deterministic for a given graph.
std::vector<WeightedEdge>
computeLayoutEdges(const CallGraph &Graph,
                   const LayoutWeightConfig &Config = LayoutWeightConfig());

} // namespace callgraph
} // namespace llvm

#endif // LLVM_CALLGRAPHSECTION_LAYOUTWEIGHTS_H
