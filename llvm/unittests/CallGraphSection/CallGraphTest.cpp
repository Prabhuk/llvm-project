//===- CallGraphTest.cpp - Tests for call graph reconstruction ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/CallGraph.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/CallGraphSection/LayoutWeights.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::callgraph;

namespace {

/// Convenience: register an indirect target belonging to \p Module.
static NodeId addTarget(CallGraphBuilder &B, ModuleId Module, uint64_t TypeId,
                        bool IsExternal) {
  FunctionNode Node;
  Node.Module = Module;
  Node.TypeId = TypeId;
  Node.IsIndirectTarget = true;
  Node.IsExternal = IsExternal;
  return B.addFunction(std::move(Node));
}

/// Convenience: register a function that performs an indirect call.
static NodeId addCaller(CallGraphBuilder &B, ModuleId Module, uint64_t TypeId) {
  FunctionNode Node;
  Node.Module = Module;
  Node.IsExternal = true;
  Node.IndirectCalleeTypeIds.push_back(TypeId);
  return B.addFunction(std::move(Node));
}

TEST(CallGraphTest, ShadowedLocalSymbolsIsolatedPerModule) {
  CallGraphBuilder Builder;

  // Two internal-linkage helpers in different modules share a signature. A
  // caller in module 1 must only see the helper from module 1.
  NodeId Caller = addCaller(Builder, /*Module=*/1, 0xABCD);
  NodeId HelperM1 = addTarget(Builder, /*Module=*/1, 0xABCD,
                              /*IsExternal=*/false);
  NodeId HelperM2 = addTarget(Builder, /*Module=*/2, 0xABCD,
                              /*IsExternal=*/false);
  Builder.addAddressTakenFact(HelperM1, /*SourceModule=*/1,
                              /*IsGlobalEscape=*/false);
  Builder.addAddressTakenFact(HelperM2, /*SourceModule=*/2,
                              /*IsGlobalEscape=*/false);

  CallGraph Graph = Builder.build();

  ASSERT_EQ(Graph.indirectCallSites(Caller).size(), 1u);
  EXPECT_EQ(Graph.indirectCallSites(Caller)[0].TypeId, 0xABCDu);
  EXPECT_EQ(Graph.indirectCallSites(Caller)[0].Targets,
            ArrayRef<NodeId>(HelperM1));
  EXPECT_FALSE(is_contained(Graph.callees(Caller), HelperM2));
}

TEST(CallGraphTest, EscapedLocalSymbolReachableFromOtherModules) {
  CallGraphBuilder Builder;

  NodeId Caller = addCaller(Builder, /*Module=*/1, 0x1234);
  NodeId Escaped = addTarget(Builder, /*Module=*/2, 0x1234,
                             /*IsExternal=*/false);
  // Address stored into an exported table: observable program-wide.
  Builder.addAddressTakenFact(Escaped, /*SourceModule=*/2,
                              /*IsGlobalEscape=*/true);

  CallGraph Graph = Builder.build();

  ASSERT_EQ(Graph.indirectCallSites(Caller).size(), 1u);
  EXPECT_EQ(Graph.indirectCallSites(Caller)[0].Targets,
            ArrayRef<NodeId>(Escaped));
  EXPECT_TRUE(Graph[Escaped].IsAddressTaken);
}

TEST(CallGraphTest, NonAddressTakenExternalPrunedAtLinkTime) {
  CallGraphBuilder Builder;

  NodeId Caller = addCaller(Builder, /*Module=*/1, 0x5678);
  // Marked IsIndirectTarget by the producer simply because it is external,
  // but its address is never taken anywhere in the program.
  addTarget(Builder, /*Module=*/2, 0x5678, /*IsExternal=*/true);
  NodeId Taken = addTarget(Builder, /*Module=*/3, 0x5678,
                           /*IsExternal=*/true);
  Builder.addAddressTakenFact(Taken, /*SourceModule=*/3,
                              /*IsGlobalEscape=*/true);

  CallGraph Graph = Builder.build();

  ASSERT_EQ(Graph.indirectCallSites(Caller).size(), 1u);
  EXPECT_EQ(Graph.indirectCallSites(Caller)[0].Targets, ArrayRef<NodeId>(Taken));
}

TEST(CallGraphTest, UnresolvedIndirectSiteIsRetained) {
  CallGraphBuilder Builder;

  // No target carries this signature. The site must still be reported, so a
  // consumer can tell "calls an unknown target" from "makes no indirect call".
  NodeId Caller = addCaller(Builder, /*Module=*/1, 0x9999);

  CallGraph Graph = Builder.build();

  ASSERT_EQ(Graph.indirectCallSites(Caller).size(), 1u);
  EXPECT_EQ(Graph.indirectCallSites(Caller)[0].TypeId, 0x9999u);
  EXPECT_TRUE(Graph.indirectCallSites(Caller)[0].Targets.empty());
}

TEST(CallGraphTest, FanOutIsVisibleToConsumers) {
  CallGraphBuilder Builder;

  NodeId Caller = addCaller(Builder, /*Module=*/1, 0xDEAD);
  for (uint32_t I = 0; I < 65; ++I) {
    NodeId Target = addTarget(Builder, /*Module=*/2, 0xDEAD,
                              /*IsExternal=*/true);
    Builder.addAddressTakenFact(Target, /*SourceModule=*/2,
                                /*IsGlobalEscape=*/true);
  }

  CallGraph Graph = Builder.build();

  // The graph reports the full candidate set: bounding fan-out is a layout
  // policy decision, not a fact about the program.
  ASSERT_EQ(Graph.indirectCallSites(Caller).size(), 1u);
  EXPECT_EQ(Graph.indirectCallSites(Caller)[0].Targets.size(), 65u);

  // The layout policy is what drops an uninformative clique.
  EXPECT_TRUE(computeLayoutEdges(Graph).empty());

  LayoutWeightConfig Permissive;
  Permissive.MaxIndirectFanOut = 128;
  EXPECT_EQ(computeLayoutEdges(Graph, Permissive).size(), 65u);
}

TEST(CallGraphTest, SelfRecursionIsPreservedInGraphButNotInLayout) {
  CallGraphBuilder Builder;

  FunctionNode Node;
  Node.Module = 1;
  Node.IsExternal = true;
  NodeId Recursive = Builder.addFunction(Node);
  Builder.getFunction(Recursive).DirectCallees.push_back(Recursive);

  CallGraph Graph = Builder.build();

  // A stack-depth estimator needs to see the self-edge...
  EXPECT_EQ(Graph.callees(Recursive), ArrayRef<NodeId>(Recursive));
  EXPECT_EQ(Graph.callers(Recursive), ArrayRef<NodeId>(Recursive));
  // ... while a layout pass has no use for it.
  EXPECT_TRUE(computeLayoutEdges(Graph).empty());
}

TEST(CallGraphTest, ReverseEdgesAndDirectWeights) {
  CallGraphBuilder Builder;

  FunctionNode CalleeNode;
  CalleeNode.Module = 1;
  CalleeNode.IsExternal = true;
  NodeId Callee = Builder.addFunction(CalleeNode);

  FunctionNode CallerNode;
  CallerNode.Module = 1;
  CallerNode.IsExternal = true;
  // Duplicated on purpose: build() canonicalizes direct callees.
  CallerNode.DirectCallees.push_back(Callee);
  CallerNode.DirectCallees.push_back(Callee);
  NodeId Caller = Builder.addFunction(CallerNode);

  CallGraph Graph = Builder.build();

  EXPECT_EQ(Graph.directCallees(Caller), ArrayRef<NodeId>(Callee));
  EXPECT_EQ(Graph.callers(Callee), ArrayRef<NodeId>(Caller));

  std::vector<WeightedEdge> Edges = computeLayoutEdges(Graph);
  ASSERT_EQ(Edges.size(), 1u);
  EXPECT_EQ(Edges[0].From, Caller);
  EXPECT_EQ(Edges[0].To, Callee);
  EXPECT_EQ(Edges[0].Weight, LayoutWeightConfig().DirectCallWeight);
}

TEST(CallGraphTest, GraphTraitsFindsMutualRecursion) {
  CallGraphBuilder Builder;

  FunctionNode A;
  A.Module = 1;
  A.IsExternal = true;
  NodeId IdA = Builder.addFunction(A);

  FunctionNode B;
  B.Module = 1;
  B.IsExternal = true;
  NodeId IdB = Builder.addFunction(B);

  Builder.getFunction(IdA).DirectCallees.push_back(IdB);
  Builder.getFunction(IdB).DirectCallees.push_back(IdA);

  CallGraph Graph = Builder.build();

  // scc_iterator works directly on the graph via GraphTraits, which is what a
  // stack-depth estimator uses to detect unbounded recursion.
  bool FoundCycle = false;
  const CallGraph *G = &Graph;
  for (auto It = scc_begin(G); !It.isAtEnd(); ++It) {
    if (It->size() == 2)
      FoundCycle = true;
  }
  EXPECT_TRUE(FoundCycle);
}

} // namespace
