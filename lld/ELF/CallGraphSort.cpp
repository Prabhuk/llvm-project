//===- CallGraphSort.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// The file is responsible for sorting sections using LLVM call graph profile
/// data by placing frequently executed code sections together. The goal of the
/// placement is to improve the runtime performance of the final executable by
/// arranging code sections so that i-TLB misses and i-cache misses are reduced.
///
/// The algorithm first builds a call graph based on the profile data and then
/// iteratively merges "chains" (ordered lists) of input sections which will be
/// laid out as a unit. There are two implementations for deciding how to
/// merge a pair of chains:
///  - a simpler one, referred to as Call-Chain Clustering (C^3), that follows
///    "Optimizing Function Placement for Large-Scale Data-Center Applications"
/// https://research.fb.com/wp-content/uploads/2017/01/cgo2017-hfsort-final1.pdf
/// - a more advanced one, referred to as Cache-Directed-Sort (CDSort), which
///   typically produces layouts with higher locality, and hence, yields fewer
///   instruction cache misses on large binaries.
//===----------------------------------------------------------------------===//

#include "CallGraphSort.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "Symbols.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/Utils/CodeLayout.h"

#include <numeric>

using namespace llvm;
using namespace lld;
using namespace lld::elf;

using SectionPair =
    std::pair<const InputSectionBase *, const InputSectionBase *>;
using EdgeMap = MapVector<SectionPair, uint64_t>;

namespace {
struct Edge {
  int from;
  uint64_t weight;
};

struct Cluster {
  Cluster(int sec, size_t s) : next(sec), prev(sec), size(s) {}

  double getDensity() const {
    if (size == 0)
      return 0;
    return double(weight) / double(size);
  }

  int next;
  int prev;
  uint64_t size;
  uint64_t weight = 0;
  uint64_t initialWeight = 0;
  Edge bestPred = {-1, 0};
};

/// Implementation of the Call-Chain Clustering (C^3). The goal of this
/// algorithm is to improve runtime performance of the executable by arranging
/// code sections such that page table and i-cache misses are minimized.
///
/// Definitions:
/// * Cluster
///   * An ordered list of input sections which are laid out as a unit. At the
///     beginning of the algorithm each input section has its own cluster and
///     the weight of the cluster is the sum of the weight of all incoming
///     edges.
/// * Call-Chain Clustering (C³) Heuristic
///   * Defines when and how clusters are combined. Pick the highest weighted
///     input section then add it to its most likely predecessor if it wouldn't
///     penalize it too much.
/// * Density
///   * The weight of the cluster divided by the size of the cluster. This is a
///     proxy for the amount of execution time spent per byte of the cluster.
///
/// It does so given a call graph profile by the following:
/// * Build a weighted call graph from the call graph profile
/// * Sort input sections by weight
/// * For each input section starting with the highest weight
///   * Find its most likely predecessor cluster
///   * Check if the combined cluster would be too large, or would have too low
///     a density.
///   * If not, then combine the clusters.
/// * Sort non-empty clusters by density
class CallGraphSort {
public:
  CallGraphSort(const EdgeMap &profile);

  SmallVector<const InputSectionBase *, 0> run();

private:
  std::vector<Cluster> clusters;
  std::vector<const InputSectionBase *> sections;
};

// Maximum amount the combined cluster density can be worse than the original
// cluster to consider merging.
constexpr int MAX_DENSITY_DEGRADATION = 8;

// Maximum cluster size in bytes.
constexpr uint64_t MAX_CLUSTER_SIZE = 1024 * 1024;
} // end anonymous namespace

// Take the edge list in \p profile and generate a graph between InputSections
// with the provided weights.
CallGraphSort::CallGraphSort(const EdgeMap &profile) {
  DenseMap<const InputSectionBase *, int> secToCluster;

  auto getOrCreateNode = [&](const InputSectionBase *isec) -> int {
    auto res = secToCluster.try_emplace(isec, clusters.size());
    if (res.second) {
      sections.push_back(isec);
      clusters.emplace_back(clusters.size(), isec->getSize());
    }
    return res.first->second;
  };

  // Create the graph.
  for (const std::pair<SectionPair, uint64_t> &c : profile) {
    const auto *fromSB = cast<InputSectionBase>(c.first.first);
    const auto *toSB = cast<InputSectionBase>(c.first.second);
    uint64_t weight = c.second;

    // Ignore edges between input sections belonging to different output
    // sections.  This is done because otherwise we would end up with clusters
    // containing input sections that can't actually be placed adjacently in the
    // output.  This messes with the cluster size and density calculations.  We
    // would also end up moving input sections in other output sections without
    // moving them closer to what calls them.
    if (fromSB->getOutputSection() != toSB->getOutputSection())
      continue;

    int from = getOrCreateNode(fromSB);
    int to = getOrCreateNode(toSB);

    clusters[to].weight += weight;

    if (from == to)
      continue;

    // Remember the best edge.
    Cluster &toC = clusters[to];
    if (toC.bestPred.from == -1 || toC.bestPred.weight < weight) {
      toC.bestPred.from = from;
      toC.bestPred.weight = weight;
    }
  }
  for (Cluster &c : clusters)
    c.initialWeight = c.weight;
}

// It's bad to merge clusters which would degrade the density too much.
static bool isNewDensityBad(Cluster &a, Cluster &b) {
  double newDensity = double(a.weight + b.weight) / double(a.size + b.size);
  return newDensity < a.getDensity() / MAX_DENSITY_DEGRADATION;
}

// Find the leader of V's belonged cluster (represented as an equivalence
// class). We apply union-find path-halving technique (simple to implement) in
// the meantime as it decreases depths and the time complexity.
static int getLeader(int *leaders, int v) {
  while (leaders[v] != v) {
    leaders[v] = leaders[leaders[v]];
    v = leaders[v];
  }
  return v;
}

static void mergeClusters(std::vector<Cluster> &cs, Cluster &into, int intoIdx,
                          Cluster &from, int fromIdx) {
  int tail1 = into.prev, tail2 = from.prev;
  into.prev = tail2;
  cs[tail2].next = intoIdx;
  from.prev = tail1;
  cs[tail1].next = fromIdx;
  into.size += from.size;
  into.weight += from.weight;
  from.size = 0;
  from.weight = 0;
}

// Group InputSections into clusters using the Call-Chain Clustering heuristic
// then sort the clusters by density. Returns the sections in layout order.
SmallVector<const InputSectionBase *, 0> CallGraphSort::run() {
  std::vector<int> sorted(clusters.size());
  std::unique_ptr<int[]> leaders(new int[clusters.size()]);

  std::iota(leaders.get(), leaders.get() + clusters.size(), 0);
  std::iota(sorted.begin(), sorted.end(), 0);
  llvm::stable_sort(sorted, [&](int a, int b) {
    return clusters[a].getDensity() > clusters[b].getDensity();
  });

  for (int l : sorted) {
    // The cluster index is the same as the index of its leader here because
    // clusters[L] has not been merged into another cluster yet.
    Cluster &c = clusters[l];

    // Don't consider merging if the edge is unlikely.
    if (c.bestPred.from == -1 || c.bestPred.weight * 10 <= c.initialWeight)
      continue;

    int predL = getLeader(leaders.get(), c.bestPred.from);
    if (l == predL)
      continue;

    Cluster *predC = &clusters[predL];
    if (c.size + predC->size > MAX_CLUSTER_SIZE)
      continue;

    if (isNewDensityBad(*predC, c))
      continue;

    leaders[l] = predL;
    mergeClusters(clusters, *predC, predL, c, l);
  }

  // Sort remaining non-empty clusters by density.
  sorted.clear();
  for (int i = 0, e = (int)clusters.size(); i != e; ++i)
    if (clusters[i].size > 0)
      sorted.push_back(i);
  llvm::stable_sort(sorted, [&](int a, int b) {
    return clusters[a].getDensity() > clusters[b].getDensity();
  });

  SmallVector<const InputSectionBase *, 0> order;
  order.reserve(clusters.size());
  for (int leader : sorted) {
    for (int i = leader;;) {
      order.push_back(sections[i]);
      i = clusters[i].next;
      if (i == leader)
        break;
    }
  }
  return order;
}

// Sort sections by the profile data using the Cache-Directed Sort algorithm.
// The placement is done by optimizing the locality by co-locating frequently
// executed code sections together. Returns the sections in layout order.
static SmallVector<const InputSectionBase *, 0>
computeCacheDirectedSortOrder(const EdgeMap &profile) {
  SmallVector<uint64_t, 0> funcSizes;
  SmallVector<uint64_t, 0> funcCounts;
  SmallVector<codelayout::EdgeCount, 0> callCounts;
  SmallVector<uint64_t, 0> callOffsets;
  SmallVector<const InputSectionBase *, 0> sections;
  DenseMap<const InputSectionBase *, size_t> secToTargetId;

  auto getOrCreateNode = [&](const InputSectionBase *inSec) -> size_t {
    auto res = secToTargetId.try_emplace(inSec, sections.size());
    if (res.second) {
      // inSec does not appear before in the graph.
      sections.push_back(inSec);
      funcSizes.push_back(inSec->getSize());
      funcCounts.push_back(0);
    }
    return res.first->second;
  };

  // Create the graph.
  for (const std::pair<SectionPair, uint64_t> &c : profile) {
    const InputSectionBase *fromSB = cast<InputSectionBase>(c.first.first);
    const InputSectionBase *toSB = cast<InputSectionBase>(c.first.second);
    // Ignore edges between input sections belonging to different sections.
    if (fromSB->getOutputSection() != toSB->getOutputSection())
      continue;

    uint64_t weight = c.second;
    // Ignore edges with zero weight.
    if (weight == 0)
      continue;

    size_t from = getOrCreateNode(fromSB);
    size_t to = getOrCreateNode(toSB);
    // Ignore self-edges (recursive calls).
    if (from == to)
      continue;

    callCounts.push_back({from, to, weight});
    // Assume that the jump is at the middle of the input section. The profile
    // data does not contain jump offsets.
    callOffsets.push_back((funcSizes[from] + 1) / 2);
    funcCounts[to] += weight;
  }

  // Run the layout algorithm.
  std::vector<uint64_t> sortedSections = codelayout::computeCacheDirectedLayout(
      funcSizes, funcCounts, callCounts, callOffsets);

  SmallVector<const InputSectionBase *, 0> order;
  order.reserve(sortedSections.size());
  for (uint64_t secIdx : sortedSections)
    order.push_back(sections[secIdx]);
  return order;
}

static SmallVector<const InputSectionBase *, 0>
computeOrder(CGProfileSortKind kind, const EdgeMap &profile) {
  if (kind == CGProfileSortKind::Cdsort)
    return computeCacheDirectedSortOrder(profile);
  return CallGraphSort(profile).run();
}

static void printSymbolOrder(Ctx &ctx,
                             ArrayRef<const InputSectionBase *> order) {
  std::error_code ec;
  raw_fd_ostream os(ctx.arg.printSymbolOrder, ec, sys::fs::OF_None);
  if (ec) {
    ErrAlways(ctx) << "cannot open " << ctx.arg.printSymbolOrder << ": "
                   << ec.message();
    return;
  }
  for (const InputSectionBase *isec : order) {
    // Search all the symbols in the file of the section
    // and find out a Defined symbol with name that is within the section.
    for (Symbol *sym : isec->file->getSymbols())
      if (!sym->isSection()) // Filter out section-type symbols here.
        if (auto *d = dyn_cast<Defined>(sym))
          if (isec == d->section)
            os << sym->getName() << "\n";
  }
}

// Sort sections by call graph edges, using the C³ or Cache-Directed-Sort
// algorithm selected by --call-graph-profile-sort.
//
// Edges come from two sources that are laid out as two tiers:
//
//  1. Measured profile edges (--call-graph-ordering-file or
//     SHT_LLVM_CALL_GRAPH_PROFILE). This tier is computed from exactly the same
//     input as when --call-graph-section is not given, so the relative order of
//     these sections is unchanged.
//
//  2. Static edges reconstructed from SHT_LLVM_CALL_GRAPH sections
//     (--call-graph-section). Only edges whose endpoints were both left
//     unplaced by tier 1 participate, and every tier 2 section is placed after
//     every tier 1 section. The measured profile therefore stays authoritative
//     for the code it covers, and the static graph organizes the rest.
//
// With --call-graph-section=only, tier 1 is empty and the static graph drives
// the whole layout.
//
// Tier 2 sections are reported in \p secondary so that sortISDBySectionOrder
// can place the other ordered sections (tier 1 and --symbol-ordering-file)
// exactly where they would be without them.
DenseMap<const InputSectionBase *, int> elf::computeCallGraphProfileOrder(
    Ctx &ctx, DenseSet<const InputSectionBase *> &secondary) {
  CGProfileSortKind kind = ctx.arg.callGraphProfileSort;

  SmallVector<const InputSectionBase *, 0> order;
  if (!ctx.arg.callGraphProfile.empty())
    order = computeOrder(kind, ctx.arg.callGraphProfile);

  if (!ctx.arg.callGraphSectionProfile.empty()) {
    DenseSet<const InputSectionBase *> placed(order.begin(), order.end());
    EdgeMap residual;
    for (const std::pair<SectionPair, uint64_t> &c :
         ctx.arg.callGraphSectionProfile)
      if (!placed.contains(c.first.first) && !placed.contains(c.first.second))
        residual.insert(c);
    if (!residual.empty()) {
      SmallVector<const InputSectionBase *, 0> tier2 =
          computeOrder(kind, residual);
      secondary.insert(tier2.begin(), tier2.end());
      order.append(tier2);
    }
  }

  DenseMap<const InputSectionBase *, int> orderMap;
  int curOrder = -order.size();
  for (const InputSectionBase *isec : order)
    orderMap[isec] = curOrder++;

  // Only C³ has historically honored --print-symbol-order.
  if (kind == CGProfileSortKind::Hfsort && !ctx.arg.printSymbolOrder.empty())
    printSymbolOrder(ctx, order);

  return orderMap;
}
