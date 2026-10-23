//===- StackSizeEstimatorTest.cpp - Tests for StackSizeEstimator ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../../../tools/llvm-stack-size/lib/StackSizeEstimator.h"
#include "../../../tools/llvm-stack-size/lib/StackSizeGraph.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::stacksize;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

namespace {

/// Builds graphs by hand, with every function instrumented and sized unless
/// a test says otherwise.
class GraphBuilder {
public:
  Function &add(uint64_t Address, StringRef Name,
                std::optional<uint64_t> Frame = 0) {
    Function &F = G.getOrCreateFunction(Address);
    F.Names.push_back(Name.str());
    F.FrameSize = Frame;
    F.HasRecord = true;
    return F;
  }

  Function &addUninstrumented(uint64_t Address, StringRef Name,
                              std::optional<uint64_t> Frame = std::nullopt) {
    Function &F = G.getOrCreateFunction(Address);
    if (!Name.empty())
      F.Names.push_back(Name.str());
    F.FrameSize = Frame;
    return F;
  }

  static void call(Function &Caller, Function &Callee) {
    Caller.DirectCallees.push_back(&Callee);
  }

  static void callIndirect(Function &Caller, uint64_t TypeId,
                           std::initializer_list<Function *> Targets) {
    IndirectCallSite Site;
    Site.TypeId = TypeId;
    Site.Targets.assign(Targets.begin(), Targets.end());
    Caller.IndirectCalls.push_back(std::move(Site));
  }

  StackSizeGraph finish() {
    G.finalize();
    return std::move(G);
  }

private:
  StackSizeGraph G;
};

std::vector<std::string> names(ArrayRef<const Function *> Fs) {
  std::vector<std::string> Names;
  for (const Function *F : Fs)
    Names.push_back(F->getDisplayName());
  return Names;
}

/// The functions of a max stack path, flattened.
std::vector<std::string> names(ArrayRef<PathStep> Path) {
  std::vector<std::string> Names;
  for (const PathStep &Step : Path)
    llvm::append_range(Names, names(Step.Functions));
  return Names;
}

Estimate estimateFrom(const StackSizeGraph &G, const Function &Entry,
                      EstimatorOptions Opts = {}) {
  StackSizeEstimator Estimator(G, Opts);
  return Estimator.estimate(
      {const_cast<Function *>(&Entry), Entry.getDisplayName()});
}

std::string expressionOf(const StackSizeGraph &G, const Function &Entry,
                         EstimatorOptions Opts = {}) {
  StackSizeEstimator Estimator(G, Opts);
  return Estimator.formatExpression(
      Estimator
          .estimate({const_cast<Function *>(&Entry), Entry.getDisplayName()})
          .Expression);
}

TEST(StackSizeEstimatorTest, DiamondTakesTheDeepestPath) {
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Function &Left = B.add(0x2000, "left", 64);
  Function &Right = B.add(0x3000, "right", 32);
  Function &Sink = B.add(0x4000, "sink", 8);
  B.call(Main, Left);
  B.call(Main, Right);
  B.call(Left, Sink);
  B.call(Right, Sink);
  StackSizeGraph G = B.finish();

  Estimate E = estimateFrom(G, Main);
  EXPECT_EQ(E.MaxStackUsage, 88u);
  EXPECT_FALSE(E.IsLowerBound);
  EXPECT_THAT(names(E.MaxStackPath), ElementsAre("main", "left", "sink"));

  Estimate R = estimateFrom(G, Right);
  EXPECT_EQ(R.MaxStackUsage, 40u);
  EXPECT_THAT(names(R.MaxStackPath), ElementsAre("right", "sink"));
}

TEST(StackSizeEstimatorTest, CallFrameOverheadAndRedZone) {
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Function &Leaf = B.add(0x2000, "leaf", 32);
  B.call(Main, Leaf);
  StackSizeGraph G = B.finish();

  EstimatorOptions Opts;
  Opts.CallFrameOverhead = 8;
  Opts.RedZoneSize = 128;
  StackSizeEstimator Estimator(G, Opts);
  // Only the leaf gets the red zone; every frame gets the overhead.
  EXPECT_EQ(Estimator.getRedZone(Main), 0u);
  EXPECT_EQ(Estimator.getRedZone(Leaf), 128u);
  EXPECT_EQ(Estimator.getFrameUsage(Main), 24u);
  EXPECT_EQ(Estimator.getFrameUsage(Leaf), 168u);
  EXPECT_EQ(Estimator.estimate({&Main, "main"}).MaxStackUsage, 192u);
}

TEST(StackSizeEstimatorTest, RecursionIsALowerBound) {
  // main calls itself and a, a and b call each other, b calls sink.
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Function &A = B.add(0x2000, "a", 32);
  Function &Bf = B.add(0x3000, "b", 64);
  Function &Sink = B.add(0x4000, "sink", 8);
  B.call(Main, Main);
  B.call(Main, A);
  B.call(A, Bf);
  B.call(Bf, A);
  B.call(Bf, Sink);
  StackSizeGraph G = B.finish();

  StackSizeEstimator Estimator(G, {});
  EXPECT_TRUE(Estimator.isRecursive(Main));
  EXPECT_TRUE(Estimator.isRecursive(A));
  EXPECT_FALSE(Estimator.isRecursive(Sink));

  Estimate E = Estimator.estimate({&Main, "main"});
  EXPECT_TRUE(E.IsLowerBound);
  EXPECT_EQ(E.MaxStackUsage, 120u);
  EXPECT_THAT(names(E.MaxStackPath), ElementsAre("main", "a", "b", "sink"));
  // The path steps through two cycles and a single function.
  ASSERT_EQ(E.MaxStackPath.size(), 3u);
  EXPECT_EQ(E.MaxStackPath[0].Cycle, 0u);
  EXPECT_EQ(E.MaxStackPath[1].Cycle, 1u);
  EXPECT_EQ(E.MaxStackPath[2].Cycle, std::nullopt);
  ASSERT_EQ(Estimator.cycles().size(), 2u);
  EXPECT_THAT(E.Cycles, ElementsAre(0u, 1u));
  EXPECT_THAT(names(Estimator.cycles()[0]), ElementsAre("main"));
  EXPECT_THAT(names(Estimator.cycles()[1]), ElementsAre("a", "b"));

  Estimate S = Estimator.estimate({&Sink, "sink"});
  EXPECT_FALSE(S.IsLowerBound);
  EXPECT_THAT(S.Cycles, IsEmpty());
}

TEST(StackSizeEstimatorTest, UnknownFrameIsALowerBound) {
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Function &Vla = B.add(0x2000, "vla", std::nullopt);
  B.call(Main, Vla);
  StackSizeGraph G = B.finish();

  Estimate E = estimateFrom(G, Main);
  EXPECT_TRUE(E.IsLowerBound);
  EXPECT_EQ(E.MaxStackUsage, 16u);
  EXPECT_THAT(names(E.UnknownFrames), ElementsAre("vla"));
  EXPECT_THAT(names(E.MaxStackPath), ElementsAre("main", "vla"));
}

TEST(StackSizeEstimatorTest, UninstrumentedCalleesAreALowerBound) {
  // An external callee with nothing known, and an assembly callee whose frame
  // is known but whose callees are not. Both are reported with their callers.
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Function &Helper = B.add(0x1100, "helper", 8);
  Function &External = B.addUninstrumented(0, "");
  Function &Asm = B.addUninstrumented(0x2000, "asm_fn", 4096);
  B.call(Main, External);
  B.call(Main, Helper);
  B.call(Helper, External);
  B.call(Helper, Asm);
  StackSizeGraph G = B.finish();

  EXPECT_EQ(External.getDisplayName(), "<external>");
  Estimate E = estimateFrom(G, Main);
  EXPECT_TRUE(E.IsLowerBound);
  EXPECT_EQ(E.MaxStackUsage, 16u + 8u + 4096u);
  EXPECT_THAT(names(E.MaxStackPath), ElementsAre("main", "helper", "asm_fn"));
  ASSERT_EQ(E.UninstrumentedCallees.size(), 2u);
  EXPECT_EQ(E.UninstrumentedCallees[0].Callee, &External);
  EXPECT_THAT(names(E.UninstrumentedCallees[0].Callers),
              ElementsAre("main", "helper"));
  EXPECT_EQ(E.UninstrumentedCallees[1].Callee, &Asm);
  EXPECT_THAT(names(E.UninstrumentedCallees[1].Callers), ElementsAre("helper"));

  // An entry point without a record is its own uninstrumented callee.
  Estimate A = estimateFrom(G, Asm);
  EXPECT_TRUE(A.IsLowerBound);
  EXPECT_EQ(A.MaxStackUsage, 4096u);
  ASSERT_EQ(A.UninstrumentedCallees.size(), 1u);
  EXPECT_EQ(A.UninstrumentedCallees[0].Callee, &Asm);
  EXPECT_THAT(A.UninstrumentedCallees[0].Callers, IsEmpty());
}

TEST(StackSizeEstimatorTest, IndirectCalls) {
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Function &T1 = B.add(0x2000, "t1", 32);
  Function &T2 = B.add(0x3000, "t2", 64);
  Function &Unresolved = B.add(0x4000, "unresolved", 16);
  T1.IsIndirectTarget = T2.IsIndirectTarget = true;
  T1.TypeIds.push_back(0xaa);
  T2.TypeIds.push_back(0xaa);
  B.callIndirect(Main, 0xaa, {&T1, &T2});
  Unresolved.UnresolvedIndirectTypeIds.push_back(0xbb);
  StackSizeGraph G = B.finish();

  // Resolved indirect calls are exact.
  Estimate E = estimateFrom(G, Main);
  EXPECT_FALSE(E.IsLowerBound);
  EXPECT_EQ(E.MaxStackUsage, 80u);
  EXPECT_THAT(names(E.MaxStackPath), ElementsAre("main", "t2"));

  Estimate U = estimateFrom(G, Unresolved);
  EXPECT_TRUE(U.IsLowerBound);
  EXPECT_THAT(names(U.UnresolvedIndirectCallers), ElementsAre("unresolved"));
}

TEST(StackSizeEstimatorTest, UntypedTargetsMakeIndirectCallsALowerBound) {
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Function &Typed = B.add(0x2000, "typed", 32);
  Function &Untyped = B.add(0x3000, "untyped", 4096);
  Function &Direct = B.add(0x4000, "direct", 8);
  Typed.IsIndirectTarget = true;
  Typed.TypeIds.push_back(0xaa);
  Untyped.IsIndirectTarget = true;
  Untyped.HasUnknownTypeId = true;
  B.callIndirect(Main, 0xaa, {&Typed});
  B.call(Direct, Typed);
  StackSizeGraph G = B.finish();

  EXPECT_THAT(names(G.getUnknownTypeIndirectTargets()), ElementsAre("untyped"));

  // The indirect call might reach the untyped target as well.
  Estimate E = estimateFrom(G, Main);
  EXPECT_TRUE(E.IsLowerBound);
  EXPECT_EQ(E.MaxStackUsage, 48u);
  EXPECT_THAT(names(E.IndirectCallersWithUntypedTargets), ElementsAre("main"));

  // A function that reaches no indirect call is unaffected.
  Estimate D = estimateFrom(G, Direct);
  EXPECT_FALSE(D.IsLowerBound);
}

TEST(StackSizeEstimatorTest, UnknownIndirectTargetsCountTheReturnAddress) {
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Main.UnresolvedIndirectTypeIds.push_back(0xbb);
  StackSizeGraph G = B.finish();

  EstimatorOptions Opts;
  Opts.CallFrameOverhead = 8;
  Estimate E = estimateFrom(G, Main, Opts);
  EXPECT_TRUE(E.IsLowerBound);
  EXPECT_EQ(E.MaxStackUsage, 32u);
  EXPECT_THAT(names(E.MaxStackPath), ElementsAre("main"));
  EXPECT_EQ(expressionOf(G, Main, Opts), "32 + indirect(type 0xbb)");
}

TEST(StackSizeEstimatorTest, ExpressionsKeepUnknownsAsTerms) {
  // main calls leaf and an external function; helper calls external through
  // two paths; rec calls itself and vla, which has no frame size.
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Function &Leaf = B.add(0x2000, "leaf", 64);
  Function &External = B.addUninstrumented(0, "");
  Function &Helper = B.add(0x1100, "helper", 16);
  Function &A = B.add(0x2100, "a", 64);
  Function &Bf = B.add(0x2200, "b", 32);
  Function &Rec = B.add(0x1200, "rec", 8);
  Function &Vla = B.add(0x2300, "vla", std::nullopt);
  B.call(Main, Leaf);
  B.call(Main, External);
  B.call(Helper, A);
  B.call(Helper, Bf);
  B.call(A, External);
  B.call(Bf, External);
  B.call(Rec, Rec);
  B.call(Rec, Vla);
  StackSizeGraph G = B.finish();

  // An exact estimate is a number.
  EXPECT_EQ(expressionOf(G, Leaf), "64");
  // Incomparable paths both stay.
  EXPECT_EQ(expressionOf(G, Main), "max(80, 16 + calls(<external>))");
  // A path dominated by another with the same unknowns is dropped.
  EXPECT_EQ(expressionOf(G, Helper), "80 + calls(<external>)");
  // Unknowns are sorted and never repeated.
  EXPECT_EQ(expressionOf(G, Rec), "8 + cycle(rec) + frame(vla)");

  // The terms refer to the unknowns of the estimator.
  StackSizeEstimator Estimator(G, {});
  Estimate R = Estimator.estimate({&Rec, "rec"});
  ASSERT_EQ(R.Expression.Terms.size(), 1u);
  EXPECT_EQ(R.Expression.Terms[0].Constant, 8u);
  ASSERT_EQ(R.Expression.Terms[0].Unknowns.size(), 2u);
  std::vector<Unknown::Kind> Kinds;
  for (unsigned Id : R.Expression.Terms[0].Unknowns) {
    const Unknown &U = Estimator.unknowns()[Id];
    Kinds.push_back(U.K);
    if (U.K == Unknown::Kind::Frame)
      EXPECT_EQ(U.F, &Vla);
    else
      EXPECT_THAT(names(Estimator.cycles()[U.Cycle]), ElementsAre("rec"));
  }
  EXPECT_THAT(Kinds, testing::UnorderedElementsAre(Unknown::Kind::Frame,
                                                   Unknown::Kind::Cycle));
}

TEST(StackSizeEstimatorTest, UnknownsAreNamedByWhatIsUnknown) {
  // Two functions make indirect calls of the same unresolved type, and two
  // make calls that may reach the untyped target, so each of those is one
  // unknown. A cycle of four names three members.
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Function &P = B.add(0x2000, "p", 32);
  Function &Q = B.add(0x3000, "q", 32);
  Function &R = B.add(0x4000, "r", 8);
  Function &S = B.add(0x5000, "s", 8);
  Function &Typed = B.add(0x6000, "typed", 0);
  Function &Untyped = B.add(0x7000, "untyped", 0);
  Typed.IsIndirectTarget = true;
  Typed.TypeIds.push_back(0xaa);
  Untyped.IsIndirectTarget = true;
  Untyped.HasUnknownTypeId = true;
  B.call(Main, P);
  B.call(Main, Q);
  B.call(Main, R);
  B.call(Main, S);
  P.UnresolvedIndirectTypeIds.push_back(0xbb);
  Q.UnresolvedIndirectTypeIds.push_back(0xbb);
  B.callIndirect(R, 0xaa, {&Typed});
  B.callIndirect(S, 0xaa, {&Typed});
  StackSizeGraph G = B.finish();

  EstimatorOptions Opts;
  Opts.CallFrameOverhead = 8;
  EXPECT_EQ(expressionOf(G, Main, Opts),
            "max(72 + indirect(type 0xbb), 48 + indirect(unknown type))");

  GraphBuilder C;
  Function &A1 = C.add(0x1000, "a1", 8);
  Function &A2 = C.add(0x2000, "a2", 8);
  Function &A3 = C.add(0x3000, "a3", 8);
  Function &A4 = C.add(0x4000, "a4", 8);
  C.call(A1, A2);
  C.call(A2, A3);
  C.call(A3, A4);
  C.call(A4, A1);
  StackSizeGraph G2 = C.finish();
  EXPECT_EQ(expressionOf(G2, A1), "32 + cycle(a1, a2, a3 and 1 more)");
}

TEST(StackSizeEstimatorTest, IndirectCallsWithUnknownTargetsAreABranch) {
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Function &Leaf = B.add(0x2000, "leaf", 64);
  B.call(Main, Leaf);
  Main.UnresolvedIndirectTypeIds.push_back(0xbb);
  StackSizeGraph G = B.finish();

  EstimatorOptions Opts;
  Opts.CallFrameOverhead = 8;
  EXPECT_EQ(expressionOf(G, Main, Opts), "max(96, 32 + indirect(type 0xbb))");
}

TEST(StackSizeEstimatorTest, ExpressionsBeyondTheTermLimitAreCoarsened) {
  // main calls 40 functions of distinct frame sizes, each of which calls an
  // uninstrumented function of its own, so main has 40 incomparable paths.
  // The 32 with the largest constants stay and the other 8 merge into one
  // term that bounds them. A caller of main inherits the coarsening.
  GraphBuilder B;
  Function &Caller = B.add(0x100, "caller", 8);
  Function &Main = B.add(0x1000, "main", 16);
  B.call(Caller, Main);
  for (unsigned I = 1; I <= 40; ++I) {
    Function &F = B.add(0x2000 + 0x10 * I, "f" + utostr(I), 8 * I);
    Function &X = B.addUninstrumented(0x3000 + 0x10 * I, "x" + utostr(I));
    B.call(Main, F);
    B.call(F, X);
  }
  StackSizeGraph G = B.finish();

  StackSizeEstimator Estimator(G, {});
  Estimate E = Estimator.estimate({&Main, "main"});
  EXPECT_EQ(E.MaxStackUsage, 16u + 8 * 40);
  EXPECT_TRUE(E.Expression.Coarsened);
  ASSERT_EQ(E.Expression.Terms.size(), 33u);
  // The first term is still the stack usage, with the unknown of f40.
  EXPECT_EQ(E.Expression.Terms.front().Constant, E.MaxStackUsage);
  EXPECT_EQ(E.Expression.Terms.front().Unknowns.size(), 1u);
  // The last term bounds f1 to f8: the constant of f8 and all their unknowns.
  const StackUsageTerm &Bound = E.Expression.Terms.back();
  EXPECT_EQ(Bound.Constant, 16u + 8 * 8);
  ASSERT_EQ(Bound.Unknowns.size(), 8u);
  std::vector<std::string> BoundNames;
  for (unsigned Id : Bound.Unknowns)
    BoundNames.push_back(Estimator.unknownNames()[Id]);
  EXPECT_THAT(BoundNames,
              testing::UnorderedElementsAre(
                  "calls(x1)", "calls(x2)", "calls(x3)", "calls(x4)",
                  "calls(x5)", "calls(x6)", "calls(x7)", "calls(x8)"));
  // Every other term is one exact path.
  for (const StackUsageTerm &T : ArrayRef(E.Expression.Terms).drop_back())
    EXPECT_EQ(T.Unknowns.size(), 1u);

  Estimate C = Estimator.estimate({&Caller, "caller"});
  EXPECT_TRUE(C.Expression.Coarsened);
  EXPECT_EQ(C.Expression.Terms.size(), 33u);
  EXPECT_EQ(C.Expression.Terms.front().Constant, 8u + E.MaxStackUsage);

  // A function with few paths is exact.
  Function &F40 = *G.findByName("f40").front();
  EXPECT_FALSE(Estimator.estimate({&F40, "f40"}).Expression.Coarsened);
}

TEST(StackSizeEstimatorTest, FormatStackUsageExpression) {
  std::string Names[] = {"calls(f)", "frame(g)"};
  EXPECT_EQ(formatStackUsageExpression({{48, {}}}, Names), "48");
  EXPECT_EQ(formatStackUsageExpression({{0, {}}}, Names), "0");
  EXPECT_EQ(formatStackUsageExpression({{0, {0}}}, Names), "calls(f)");
  EXPECT_EQ(formatStackUsageExpression({{64, {}}, {32, {0, 1}}}, Names),
            "max(64, 32 + calls(f) + frame(g))");
}

TEST(StackSizeEstimatorTest, FrameSumsSaturate) {
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Function &Huge = B.add(0x2000, "huge", UINT64_MAX);
  B.call(Main, Huge);
  StackSizeGraph G = B.finish();

  EstimatorOptions Opts;
  Opts.CallFrameOverhead = 8;
  EXPECT_EQ(estimateFrom(G, Main, Opts).MaxStackUsage, UINT64_MAX);
}

TEST(StackSizeEstimatorTest, ResolveEntryPoints) {
  GraphBuilder B;
  Function &Main = B.add(0x1000, "main", 16);
  Main.Names.push_back("main_alias");
  Function &Dup1 = B.add(0x2000, "dup", 32);
  Function &Dup2 = B.add(0x3000, "dup", 64);
  Function &Exported = B.add(0x4000, "exported", 8);
  StackSizeGraph G = B.finish();

  // Without a specification, the ELF entry point and the exported functions.
  EXPECT_THAT_EXPECTED(
      resolveEntryPoints(G, {}),
      FailedWithMessage(testing::HasSubstr("no entry points")));
  G.setEntryFunction(&Main);
  G.addDynamicFunction(&Exported);
  G.addDynamicFunction(&Main);
  {
    auto Entries = resolveEntryPoints(G, {});
    ASSERT_THAT_EXPECTED(Entries, Succeeded());
    ASSERT_EQ(Entries->size(), 2u);
    EXPECT_EQ((*Entries)[0].F, &Main);
    EXPECT_EQ((*Entries)[0].Name, "main");
    EXPECT_EQ((*Entries)[1].F, &Exported);
  }

  // A name selects every function defined under it, a glob too.
  {
    auto Entries = resolveEntryPoints(G, {"dup"});
    ASSERT_THAT_EXPECTED(Entries, Succeeded());
    ASSERT_EQ(Entries->size(), 2u);
    EXPECT_EQ((*Entries)[0].F, &Dup1);
    EXPECT_EQ((*Entries)[1].F, &Dup2);
  }
  {
    auto Entries = resolveEntryPoints(G, {"d*", "ex*"});
    ASSERT_THAT_EXPECTED(Entries, Succeeded());
    ASSERT_EQ(Entries->size(), 3u);
    EXPECT_EQ((*Entries)[2].F, &Exported);
  }

  // Aliases and addresses select a function once, under the first name.
  {
    auto Entries = resolveEntryPoints(G, {"main_alias", "main", "0x1000"});
    ASSERT_THAT_EXPECTED(Entries, Succeeded());
    ASSERT_EQ(Entries->size(), 1u);
    EXPECT_EQ((*Entries)[0].Name, "main_alias");
  }

  EXPECT_THAT_EXPECTED(resolveEntryPoints(G, {"missing"}),
                       FailedWithMessage("no function named 'missing'"));
  EXPECT_THAT_EXPECTED(resolveEntryPoints(G, {"miss*"}),
                       FailedWithMessage("no function matches 'miss*'"));
  EXPECT_THAT_EXPECTED(resolveEntryPoints(G, {"0x5000"}),
                       FailedWithMessage("no function at address 0x5000"));
}

} // namespace
