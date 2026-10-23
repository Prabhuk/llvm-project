//===- CallGraphTest.cpp - Tests for call graph reconstruction ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/CallGraph.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SCCIterator.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::callgraph;

namespace {

/// Convenience: register an indirect target belonging to \p Module, described
/// by a record.
static NodeId addTarget(CallGraphBuilder &B, ModuleId Module, uint64_t TypeId,
                        bool IsExternal) {
  FunctionNode Node;
  Node.HasRecord = true;
  Node.Module = Module;
  Node.TypeIds.push_back(TypeId);
  Node.IsIndirectTarget = true;
  Node.IsExternal = IsExternal;
  return B.addFunction(std::move(Node));
}

/// Convenience: register a function, described by a record, that performs an
/// indirect call.
static NodeId addCaller(CallGraphBuilder &B, ModuleId Module, uint64_t TypeId) {
  FunctionNode Node;
  Node.HasRecord = true;
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
  EXPECT_EQ(Graph.indirectCallSites(Caller)[0].Targets,
            ArrayRef<NodeId>(Taken));
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

  ASSERT_EQ(Graph.indirectCallSites(Caller).size(), 1u);
  EXPECT_EQ(Graph.indirectCallSites(Caller)[0].Targets.size(), 65u);
}

TEST(CallGraphTest, SelfRecursionIsPreservedInGraph) {
  CallGraphBuilder Builder;

  FunctionNode Node;
  Node.HasRecord = true;
  Node.Module = 1;
  Node.IsExternal = true;
  NodeId Recursive = Builder.addFunction(Node);
  Builder.getFunction(Recursive).DirectCallees.push_back(Recursive);

  CallGraph Graph = Builder.build();

  // A stack-depth estimator needs to see the self-edge.
  EXPECT_EQ(Graph.callees(Recursive), ArrayRef<NodeId>(Recursive));
  EXPECT_EQ(Graph.callers(Recursive), ArrayRef<NodeId>(Recursive));
}

TEST(CallGraphTest, ReverseEdgesAndDeduplication) {
  CallGraphBuilder Builder;

  FunctionNode CalleeNode;
  CalleeNode.HasRecord = true;
  CalleeNode.Module = 1;
  CalleeNode.IsExternal = true;
  NodeId Callee = Builder.addFunction(CalleeNode);

  FunctionNode CallerNode;
  CallerNode.HasRecord = true;
  CallerNode.Module = 1;
  CallerNode.IsExternal = true;
  // Duplicated on purpose: build() canonicalizes direct callees.
  CallerNode.DirectCallees.push_back(Callee);
  CallerNode.DirectCallees.push_back(Callee);
  NodeId Caller = Builder.addFunction(CallerNode);

  CallGraph Graph = Builder.build();

  EXPECT_EQ(Graph.directCallees(Caller), ArrayRef<NodeId>(Callee));
  EXPECT_EQ(Graph.callers(Callee), ArrayRef<NodeId>(Caller));
}

TEST(CallGraphTest, GraphTraitsFindsMutualRecursion) {
  CallGraphBuilder Builder;

  FunctionNode A;
  A.HasRecord = true;
  A.Module = 1;
  A.IsExternal = true;
  NodeId IdA = Builder.addFunction(A);

  FunctionNode B;
  B.HasRecord = true;
  B.Module = 1;
  B.IsExternal = true;
  NodeId IdB = Builder.addFunction(B);

  Builder.getFunction(IdA).DirectCallees.push_back(IdB);
  Builder.getFunction(IdB).DirectCallees.push_back(IdA);
  Builder.addRoot(IdA);

  CallGraph Graph = Builder.build();

  bool FoundCycle = false;
  const CallGraph *G = &Graph;
  for (auto It = scc_begin(G); !It.isAtEnd(); ++It) {
    if (It->size() == 2)
      FoundCycle = true;
  }
  EXPECT_TRUE(FoundCycle);
}

TEST(CallGraphTest, PseudoNodesFollowFunctions) {
  CallGraphBuilder Builder;
  NodeId F = addCaller(Builder, /*Module=*/1, 0x1);

  CallGraph Graph = Builder.build();

  EXPECT_EQ(Graph.numFunctions(), 1u);
  EXPECT_EQ(Graph.size(), 4u);
  EXPECT_TRUE(Graph.isFunction(F));
  EXPECT_EQ(Graph.functions().size(), 1u);
  EXPECT_EQ(Graph[Graph.externalCallingNode()].Kind, NodeKind::ExternalCalling);
  EXPECT_EQ(Graph[Graph.unknownCalleeNode()].Kind, NodeKind::UnknownCallee);
  EXPECT_EQ(Graph[Graph.untypedTargetsNode()].Kind, NodeKind::UntypedTargets);
  for (NodeId P : {Graph.externalCallingNode(), Graph.unknownCalleeNode(),
                   Graph.untypedTargetsNode()}) {
    EXPECT_TRUE(Graph[P].isPseudo());
    EXPECT_FALSE(Graph[P].IsExecutable);
    EXPECT_FALSE(Graph.isFunction(P));
  }
}

TEST(CallGraphTest, FunctionWithoutRecordCallsUnknown) {
  CallGraphBuilder Builder;

  NodeId Known = addTarget(Builder, /*Module=*/1, 0x1, /*IsExternal=*/true);
  FunctionNode AsmNode;
  AsmNode.Module = 2;
  AsmNode.IsExternal = true;
  AsmNode.DirectCallees.push_back(Known);
  NodeId Asm = Builder.addFunction(AsmNode);

  NodeId Callback = addTarget(Builder, /*Module=*/1, 0x2,
                              /*IsExternal=*/true);
  Builder.addAddressTakenFact(Callback, /*SourceModule=*/2,
                              /*IsGlobalEscape=*/true);

  CallGraph Graph = Builder.build();
  const NodeId Unknown = Graph.unknownCalleeNode();

  EXPECT_EQ(Graph.directCallees(Asm), ArrayRef<NodeId>(Known));
  EXPECT_TRUE(is_contained(Graph.callees(Asm), Known));
  EXPECT_TRUE(is_contained(Graph.callees(Asm), Unknown));
  EXPECT_EQ(Graph.callees(Unknown), ArrayRef<NodeId>(Callback));
  EXPECT_FALSE(is_contained(Graph.callees(Known), Unknown));
}

TEST(CallGraphTest, CallsUnknownConnectsRecordedFunction) {
  CallGraphBuilder Builder;

  FunctionNode Node;
  Node.HasRecord = true;
  Node.Module = 1;
  Node.CallsUnknown = true;
  NodeId F = Builder.addFunction(Node);

  CallGraph Graph = Builder.build();

  EXPECT_EQ(Graph.callees(F), ArrayRef<NodeId>(Graph.unknownCalleeNode()));
  EXPECT_EQ(Graph.callers(Graph.unknownCalleeNode()), ArrayRef<NodeId>(F));
}

TEST(CallGraphTest, UntypedAddressTakenReachableFromIndirectSites) {
  CallGraphBuilder Builder;

  NodeId Caller = addCaller(Builder, /*Module=*/1, 0xAAAA);
  NodeId Typed = addTarget(Builder, /*Module=*/2, 0xAAAA, /*IsExternal=*/true);
  Builder.addAddressTakenFact(Typed, /*SourceModule=*/2,
                              /*IsGlobalEscape=*/true);

  FunctionNode AsmNode;
  AsmNode.Module = 3;
  AsmNode.IsExternal = true;
  NodeId Asm = Builder.addFunction(AsmNode);
  Builder.addAddressTakenFact(Asm, /*SourceModule=*/3,
                              /*IsGlobalEscape=*/true);

  FunctionNode UnclaimedNode;
  UnclaimedNode.HasRecord = true;
  UnclaimedNode.Module = 3;
  NodeId Unclaimed = Builder.addFunction(UnclaimedNode);
  Builder.addAddressTakenFact(Unclaimed, /*SourceModule=*/3,
                              /*IsGlobalEscape=*/true);

  CallGraph Graph = Builder.build();
  const NodeId Untyped = Graph.untypedTargetsNode();

  ASSERT_EQ(Graph.indirectCallSites(Caller).size(), 1u);
  EXPECT_EQ(Graph.indirectCallSites(Caller)[0].Targets,
            ArrayRef<NodeId>(Typed));
  EXPECT_TRUE(is_contained(Graph.callees(Caller), Untyped));
  EXPECT_EQ(Graph.callees(Untyped), ArrayRef<NodeId>({Asm, Unclaimed}));
  EXPECT_EQ(Graph.coverage().NumUntypedAddressTaken, 2u);
  EXPECT_EQ(Graph.coverage().NumAddressTaken, 3u);
}

TEST(CallGraphTest, NoUntypedEdgeWhenEveryTargetIsTyped) {
  CallGraphBuilder Builder;
  NodeId Caller = addCaller(Builder, /*Module=*/1, 0xBBBB);
  NodeId Typed = addTarget(Builder, /*Module=*/2, 0xBBBB, /*IsExternal=*/true);
  Builder.addAddressTakenFact(Typed, /*SourceModule=*/2,
                              /*IsGlobalEscape=*/true);

  CallGraph Graph = Builder.build();

  const IndirectCallSite &Site = Graph.indirectCallSites(Caller)[0];
  EXPECT_EQ(Graph.callees(Caller), ArrayRef<NodeId>(Site.TargetSetNode));
  EXPECT_EQ(Graph.callees(Site.TargetSetNode), ArrayRef<NodeId>(Typed));
}

TEST(CallGraphTest, CandidateSetsAreSharedBetweenSites) {
  CallGraphBuilder Builder;

  NodeId CallerM1 = addCaller(Builder, /*Module=*/1, 0xEEEE);
  NodeId CallerM2 = addCaller(Builder, /*Module=*/2, 0xEEEE);
  NodeId CallerM3 = addCaller(Builder, /*Module=*/3, 0xEEEE);
  NodeId Global = addTarget(Builder, /*Module=*/4, 0xEEEE, /*IsExternal=*/true);
  Builder.addAddressTakenFact(Global, /*SourceModule=*/4,
                              /*IsGlobalEscape=*/true);
  NodeId LocalM3 = addTarget(Builder, /*Module=*/3, 0xEEEE,
                             /*IsExternal=*/false);
  Builder.addAddressTakenFact(LocalM3, /*SourceModule=*/3,
                              /*IsGlobalEscape=*/false);
  Builder.addRoot(CallerM3);

  CallGraph Graph = Builder.build();

  const IndirectCallSite &S1 = Graph.indirectCallSites(CallerM1)[0];
  const IndirectCallSite &S2 = Graph.indirectCallSites(CallerM2)[0];
  const IndirectCallSite &S3 = Graph.indirectCallSites(CallerM3)[0];
  EXPECT_EQ(S1.TargetSetNode, S2.TargetSetNode);
  EXPECT_EQ(S1.Targets.data(), S2.Targets.data());
  EXPECT_EQ(S1.Targets, ArrayRef<NodeId>(Global));
  EXPECT_NE(S3.TargetSetNode, S1.TargetSetNode);
  EXPECT_EQ(S3.Targets, ArrayRef<NodeId>({Global, LocalM3}));

  EXPECT_EQ(Graph[S1.TargetSetNode].Kind, NodeKind::TargetSet);
  EXPECT_FALSE(Graph[S1.TargetSetNode].IsExecutable);
  EXPECT_EQ(Graph.callers(S1.TargetSetNode),
            ArrayRef<NodeId>({CallerM1, CallerM2}));
  EXPECT_EQ(Graph.callers(Global),
            ArrayRef<NodeId>({Graph.unknownCalleeNode(), S1.TargetSetNode,
                              S3.TargetSetNode}));

  DenseSet<const FunctionNode *> Reachable;
  const CallGraph *G = &Graph;
  for (const FunctionNode *N : depth_first(G))
    Reachable.insert(N);
  EXPECT_TRUE(Reachable.contains(&Graph[Global]));
  EXPECT_TRUE(Reachable.contains(&Graph[LocalM3]));
  EXPECT_FALSE(Reachable.contains(&Graph[CallerM1]));
}

TEST(CallGraphTest, LocalEvidenceScopedToSourceModule) {
  CallGraphBuilder Builder;

  NodeId CallerM1 = addCaller(Builder, /*Module=*/1, 0xCCCC);
  NodeId CallerM3 = addCaller(Builder, /*Module=*/3, 0xCCCC);
  NodeId Target = addTarget(Builder, /*Module=*/2, 0xCCCC,
                            /*IsExternal=*/false);
  Builder.addAddressTakenFact(Target, /*SourceModule=*/3,
                              /*IsGlobalEscape=*/false);

  CallGraph Graph = Builder.build();

  EXPECT_TRUE(Graph.indirectCallSites(CallerM1)[0].Targets.empty());
  EXPECT_EQ(Graph.indirectCallSites(CallerM3)[0].Targets,
            ArrayRef<NodeId>(Target));
}

TEST(CallGraphTest, ReachabilityFromRootsIsSound) {
  CallGraphBuilder Builder;

  FunctionNode MainNode;
  MainNode.HasRecord = true;
  MainNode.Module = 1;
  NodeId Main = Builder.addFunction(MainNode);

  FunctionNode AsmNode;
  AsmNode.Module = 2;
  NodeId AsmHelper = Builder.addFunction(AsmNode);
  Builder.getFunction(Main).DirectCallees.push_back(AsmHelper);

  FunctionNode HiddenNode;
  HiddenNode.HasRecord = true;
  HiddenNode.Module = 3;
  NodeId Hidden = Builder.addFunction(HiddenNode);
  Builder.addAddressTakenFact(Hidden, /*SourceModule=*/2,
                              /*IsGlobalEscape=*/true);

  FunctionNode DeadNode;
  DeadNode.HasRecord = true;
  DeadNode.Module = 3;
  NodeId Dead = Builder.addFunction(DeadNode);

  Builder.addRoot(Main);
  CallGraph Graph = Builder.build();

  DenseSet<const FunctionNode *> Reachable;
  const CallGraph *G = &Graph;
  for (const FunctionNode *N : depth_first(G))
    Reachable.insert(N);

  EXPECT_TRUE(Reachable.contains(&Graph[Main]));
  EXPECT_TRUE(Reachable.contains(&Graph[AsmHelper]));
  EXPECT_TRUE(Reachable.contains(&Graph[Hidden]));
  EXPECT_FALSE(Reachable.contains(&Graph[Dead]));
}

TEST(CallGraphTest, NodeWithSeveralTypeIdsMatchesEach) {
  CallGraphBuilder Builder;

  NodeId CallerA = addCaller(Builder, /*Module=*/1, 0x10);
  NodeId CallerB = addCaller(Builder, /*Module=*/1, 0x20);
  FunctionNode Multi;
  Multi.HasRecord = true;
  Multi.Module = 2;
  Multi.IsIndirectTarget = true;
  Multi.TypeIds = {0x10, 0x20};
  NodeId M = Builder.addFunction(Multi);
  Builder.addAddressTakenFact(M, /*SourceModule=*/2, /*IsGlobalEscape=*/true);

  CallGraph Graph = Builder.build();

  EXPECT_EQ(Graph.indirectCallSites(CallerA)[0].Targets, ArrayRef<NodeId>(M));
  EXPECT_EQ(Graph.indirectCallSites(CallerB)[0].Targets, ArrayRef<NodeId>(M));
  EXPECT_EQ(Graph.coverage().NumUntypedAddressTaken, 0u);
}

TEST(CallGraphTest, MergedNodeWithKnownAndUnknownTypeIdIsTypedAndUntyped) {
  CallGraphBuilder Builder;

  // ICF merged two functions at one address: one had TypeId 0x10, the other
  // had an unknown TypeId (0).
  NodeId CallerA = addCaller(Builder, /*Module=*/1, 0x10);
  NodeId CallerB = addCaller(Builder, /*Module=*/1, 0x20);
  FunctionNode Merged;
  Merged.HasRecord = true;
  Merged.Module = 2;
  Merged.IsIndirectTarget = true;
  Merged.HasUnknownTypeId = true;
  Merged.TypeIds = {0x10};
  NodeId M = Builder.addFunction(Merged);
  Builder.addAddressTakenFact(M, /*SourceModule=*/2, /*IsGlobalEscape=*/true);

  CallGraph Graph = Builder.build();

  EXPECT_EQ(Graph.indirectCallSites(CallerA)[0].Targets, ArrayRef<NodeId>(M));
  EXPECT_TRUE(Graph.indirectCallSites(CallerB)[0].Targets.empty());
  EXPECT_TRUE(is_contained(Graph.callees(CallerB), Graph.untypedTargetsNode()));
  EXPECT_EQ(Graph.callees(Graph.untypedTargetsNode()), ArrayRef<NodeId>(M));
  EXPECT_EQ(Graph.coverage().NumUntypedAddressTaken, 1u);
}

TEST(CallGraphTest, PartiallyRecordedNodeIsTypedAndUntyped) {
  CallGraphBuilder Builder;

  NodeId Caller = addCaller(Builder, /*Module=*/1, 0x40);
  FunctionNode Mixed;
  Mixed.HasRecord = false;
  Mixed.Module = 2;
  Mixed.IsIndirectTarget = true;
  Mixed.TypeIds = {0x30};
  NodeId M = Builder.addFunction(Mixed);
  Builder.addAddressTakenFact(M, /*SourceModule=*/2, /*IsGlobalEscape=*/true);

  CallGraph Graph = Builder.build();

  EXPECT_TRUE(Graph.indirectCallSites(Caller)[0].Targets.empty());
  EXPECT_TRUE(is_contained(Graph.callees(Caller), Graph.untypedTargetsNode()));
  EXPECT_EQ(Graph.callees(Graph.untypedTargetsNode()), ArrayRef<NodeId>(M));
  EXPECT_TRUE(is_contained(Graph.callees(M), Graph.unknownCalleeNode()));
}

TEST(CallGraphTest, CoverageSummary) {
  CallGraphBuilder Builder;
  addCaller(Builder, /*Module=*/1, 0x1);
  FunctionNode AsmNode;
  AsmNode.Module = 2;
  Builder.addFunction(AsmNode);
  FunctionNode DataNode;
  DataNode.Module = 2;
  DataNode.IsExecutable = false;
  Builder.addFunction(DataNode);

  CallGraph Graph = Builder.build();
  const CoverageSummary &C = Graph.coverage();

  EXPECT_EQ(C.NumFunctions, 3u);
  EXPECT_EQ(C.NumWithRecord, 1u);
  EXPECT_EQ(C.NumWithoutRecord, 1u);
  EXPECT_EQ(C.NumCallingUnknown, 1u);
}

TEST(CallGraphTest, JumpTargetsAreEdgesInCallees) {
  CallGraphBuilder Builder;

  FunctionNode FooNode;
  FooNode.HasRecord = true;
  FooNode.Module = 1;
  NodeId Foo = Builder.addFunction(FooNode);
  FunctionNode ColdNode;
  ColdNode.Module = 1;
  NodeId FooCold = Builder.addFunction(ColdNode);
  Builder.getFunction(Foo).JumpTargets.push_back(FooCold);
  Builder.addRoot(Foo);

  CallGraph Graph = Builder.build();

  EXPECT_TRUE(Graph.directCallees(Foo).empty());
  EXPECT_TRUE(is_contained(Graph.callees(Foo), FooCold));
  EXPECT_TRUE(is_contained(Graph.callers(FooCold), Foo));
  const CallGraph *G = &Graph;
  EXPECT_TRUE(is_contained(depth_first(G), &Graph[FooCold]));
}

} // namespace
