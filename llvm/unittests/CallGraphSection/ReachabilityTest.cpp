//===- ReachabilityTest.cpp - Tests for call graph reachability -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/Reachability.h"
#include "llvm/CallGraphSection/LayoutWeights.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::callgraph;

namespace {

/// A function described by a record in \p Module.
static NodeId addRecorded(CallGraphBuilder &B, ModuleId Module) {
  FunctionNode Node;
  Node.HasRecord = true;
  Node.Module = Module;
  Node.IsExternal = true;
  return B.addFunction(std::move(Node));
}

/// A function described by a record that may be called indirectly as
/// \p TypeId.
static NodeId addTypedTarget(CallGraphBuilder &B, ModuleId Module,
                             uint64_t TypeId) {
  FunctionNode Node;
  Node.HasRecord = true;
  Node.Module = Module;
  Node.IsExternal = true;
  Node.IsIndirectTarget = true;
  Node.TypeIds.push_back(TypeId);
  return B.addFunction(std::move(Node));
}

/// A function without a record (hand-written assembly, foreign object).
static NodeId addUnrecorded(CallGraphBuilder &B, ModuleId Module) {
  FunctionNode Node;
  Node.Module = Module;
  Node.IsExternal = true;
  return B.addFunction(std::move(Node));
}

TEST(ReachabilityTest, EmptyGraph) {
  CallGraph Graph;
  EXPECT_EQ(computeReachable(Graph).size(), 0u);
}

TEST(ReachabilityTest, DirectAndTypedIndirectEdges) {
  CallGraphBuilder Builder;

  // main -> a ; a calls type 0x11 indirectly.
  // t1 (0x11, address-taken): reachable through a's call site.
  // t2 (0x22, address-taken): no call site of its type anywhere.
  // dead: called only by t2.
  NodeId Main = addRecorded(Builder, 1);
  NodeId A = addRecorded(Builder, 1);
  Builder.getFunction(A).IndirectCalleeTypeIds.push_back(0x11);
  Builder.getFunction(Main).DirectCallees.push_back(A);
  NodeId T1 = addTypedTarget(Builder, 1, 0x11);
  NodeId T2 = addTypedTarget(Builder, 1, 0x22);
  NodeId Dead = addRecorded(Builder, 1);
  Builder.getFunction(T2).DirectCallees.push_back(Dead);
  Builder.addAddressTakenFact(T1, 1, /*IsGlobalEscape=*/true);
  Builder.addAddressTakenFact(T2, 1, /*IsGlobalEscape=*/true);
  Builder.addRoot(Main);

  CallGraph Graph = Builder.build();
  BitVector R = computeReachable(Graph);

  EXPECT_TRUE(R.test(Main));
  EXPECT_TRUE(R.test(A));
  EXPECT_TRUE(R.test(T1));
  EXPECT_FALSE(R.test(T2));
  EXPECT_FALSE(R.test(Dead));
  EXPECT_TRUE(R.test(Graph.externalCallingNode()));
  // Nothing without a record and nothing calling unknown code is reachable.
  EXPECT_FALSE(R.test(Graph.unknownCalleeNode()));
}

TEST(ReachabilityTest, UnrecordedCodeReachesEveryAddressTakenFunction) {
  CallGraphBuilder Builder;

  // main -> asm_dispatch (no record). asm_dispatch may call any function
  // pointer, so t2 is reachable even though no call site has its type.
  NodeId Main = addRecorded(Builder, 1);
  NodeId Asm = addUnrecorded(Builder, 2);
  Builder.getFunction(Main).DirectCallees.push_back(Asm);
  NodeId T2 = addTypedTarget(Builder, 1, 0x22);
  NodeId Dead = addRecorded(Builder, 1);
  Builder.getFunction(T2).DirectCallees.push_back(Dead);
  Builder.addAddressTakenFact(T2, 1, /*IsGlobalEscape=*/true);
  Builder.addRoot(Main);

  CallGraph Graph = Builder.build();
  BitVector R = computeReachable(Graph);

  EXPECT_TRUE(R.test(Asm));
  EXPECT_TRUE(R.test(Graph.unknownCalleeNode()));
  EXPECT_TRUE(R.test(T2));
  EXPECT_TRUE(R.test(Dead));
}

TEST(ReachabilityTest, CallsUnknownReachesEveryAddressTakenFunction) {
  CallGraphBuilder Builder;

  // main calls into a shared library, which may invoke t2 through a pointer
  // it was handed.
  NodeId Main = addRecorded(Builder, 1);
  Builder.getFunction(Main).CallsUnknown = true;
  NodeId T2 = addTypedTarget(Builder, 1, 0x22);
  Builder.addAddressTakenFact(T2, 1, /*IsGlobalEscape=*/true);
  Builder.addRoot(Main);

  CallGraph Graph = Builder.build();
  EXPECT_TRUE(computeReachable(Graph).test(T2));
}

TEST(ReachabilityTest, UnreachableCallerDoesNotLeakUnknownEdge) {
  CallGraphBuilder Builder;

  // The unknown-callee edge of a function without a record only matters if
  // that function may execute.
  NodeId Main = addRecorded(Builder, 1);
  NodeId DeadAsm = addUnrecorded(Builder, 2);
  NodeId T2 = addTypedTarget(Builder, 1, 0x22);
  Builder.addAddressTakenFact(T2, 1, /*IsGlobalEscape=*/true);
  Builder.addRoot(Main);

  CallGraph Graph = Builder.build();
  BitVector R = computeReachable(Graph);

  EXPECT_FALSE(R.test(DeadAsm));
  EXPECT_FALSE(R.test(Graph.unknownCalleeNode()));
  EXPECT_FALSE(R.test(T2));
}

TEST(ReachabilityTest, UntypedAddressTakenAreRootsByDefault) {
  CallGraphBuilder Builder;

  // part is address-taken code without a record (for example a basic block
  // section entered through a jump table). No call reaches it, but nothing
  // excludes it either.
  NodeId Main = addRecorded(Builder, 1);
  NodeId Part = addUnrecorded(Builder, 1);
  NodeId Callee = addRecorded(Builder, 1);
  Builder.getFunction(Part).DirectCallees.push_back(Callee);
  Builder.addAddressTakenFact(Part, 1, /*IsGlobalEscape=*/false);
  // Recorded, address-taken, but not claimed as an indirect target.
  NodeId Unclaimed = addRecorded(Builder, 1);
  Builder.addAddressTakenFact(Unclaimed, 1, /*IsGlobalEscape=*/true);
  Builder.addRoot(Main);

  CallGraph Graph = Builder.build();

  BitVector R = computeReachable(Graph);
  EXPECT_TRUE(R.test(Part));
  EXPECT_TRUE(R.test(Callee));
  EXPECT_TRUE(R.test(Unclaimed));
  // Part has no record, so reaching it makes unknown code reachable.
  EXPECT_TRUE(R.test(Graph.unknownCalleeNode()));

  ReachabilityOptions Strict;
  Strict.UntypedAddressTakenAreRoots = false;
  BitVector S = computeReachable(Graph, Strict);
  EXPECT_FALSE(S.test(Part));
  EXPECT_FALSE(S.test(Callee));
  EXPECT_FALSE(S.test(Unclaimed));
}

TEST(ReachabilityTest, UnknownCodeRoot) {
  CallGraphBuilder Builder;

  // A shared library: exported get_handler returns &handler. The program
  // that loads the library may call handler through that pointer.
  NodeId GetHandler = addRecorded(Builder, 1);
  NodeId Handler = addTypedTarget(Builder, 1, 0x33);
  Builder.addAddressTakenFact(Handler, 1, /*IsGlobalEscape=*/true);
  Builder.addRoot(GetHandler);

  {
    CallGraphBuilder Copy = Builder;
    CallGraph Graph = Copy.build();
    EXPECT_FALSE(computeReachable(Graph).test(Handler));
  }

  Builder.addUnknownCodeRoot();
  CallGraph Graph = Builder.build();
  BitVector R = computeReachable(Graph);
  EXPECT_TRUE(R.test(Graph.unknownCalleeNode()));
  EXPECT_TRUE(R.test(Handler));
  EXPECT_TRUE(Graph.callers(Graph.unknownCalleeNode()).size() == 1 &&
              Graph.callers(Graph.unknownCalleeNode())[0] ==
                  Graph.externalCallingNode());
}

TEST(ReachabilityTest, CyclesTerminate) {
  CallGraphBuilder Builder;

  NodeId A = addRecorded(Builder, 1);
  NodeId B = addRecorded(Builder, 1);
  NodeId C = addRecorded(Builder, 1);
  Builder.getFunction(A).DirectCallees.push_back(B);
  Builder.getFunction(B).DirectCallees.push_back(A);
  Builder.getFunction(B).DirectCallees.push_back(B);
  // C calls into the cycle but nothing calls C.
  Builder.getFunction(C).DirectCallees.push_back(A);
  Builder.addRoot(A);

  CallGraph Graph = Builder.build();
  BitVector R = computeReachable(Graph);
  EXPECT_TRUE(R.test(A));
  EXPECT_TRUE(R.test(B));
  EXPECT_FALSE(R.test(C));
}

TEST(ReachabilityTest, AdditionalRootsAreFollowed) {
  CallGraphBuilder Builder;

  // A profile observed handler running (e.g. entered by the kernel), so
  // handler and its callees are reachable although no root reaches them.
  NodeId Main = addRecorded(Builder, 1);
  NodeId Handler = addTypedTarget(Builder, 1, 0x44);
  NodeId Helper = addRecorded(Builder, 1);
  Builder.getFunction(Handler).DirectCallees.push_back(Helper);
  Builder.addAddressTakenFact(Handler, 1, /*IsGlobalEscape=*/true);
  Builder.addRoot(Main);

  CallGraph Graph = Builder.build();
  EXPECT_FALSE(computeReachable(Graph).test(Helper));

  ReachabilityOptions Options;
  NodeId Extra[] = {Handler, InvalidNodeId};
  Options.AdditionalRoots = Extra;
  BitVector R = computeReachable(Graph, Options);
  EXPECT_TRUE(R.test(Handler));
  EXPECT_TRUE(R.test(Helper));
}

TEST(ReachabilityTest, JumpTargetsAreReachableButNotLaidOut) {
  CallGraphBuilder Builder;

  // foo branches to foo.cold, the split-off part of itself. foo's record
  // lists calls only, so the branch arrives as a jump target.
  NodeId Main = addRecorded(Builder, 1);
  NodeId Foo = addRecorded(Builder, 1);
  NodeId FooCold = addUnrecorded(Builder, 1);
  Builder.getFunction(Main).DirectCallees.push_back(Foo);
  Builder.getFunction(Foo).JumpTargets.push_back(FooCold);
  Builder.addRoot(Main);

  CallGraph Graph = Builder.build();
  EXPECT_TRUE(computeReachable(Graph).test(FooCold));
  EXPECT_TRUE(Graph.directCallees(Foo).empty());
  for (const WeightedEdge &E : computeLayoutEdges(Graph))
    EXPECT_NE(E.To, FooCold);
}

} // namespace
