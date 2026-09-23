//===- CallGraphSectionTest.cpp - Tests for CallGraphSectionReconstructor -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Object/CallGraphSection.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::object;

namespace {

TEST(CallGraphSectionTest, ShadowedLocalSymbolsIsolatedPerTU) {
  CallGraphSectionReconstructor Reconstructor;

  // TU 1: caller_tu1 calls TypeId 0xABCD indirectly; local_helper_tu1 has
  // TypeId 0xABCD and is locally address-taken in TU 1.
  CallGraphFunctionNode CallerTU1;
  CallerTU1.TranslationUnitId = 1;
  CallerTU1.IsExternalLinkage = true;
  CallerTU1.IndirectCalleeTypeIds.push_back(0xABCD);
  uint32_t CallerTU1Id = Reconstructor.AddFunctionNode(CallerTU1);

  CallGraphFunctionNode HelperTU1;
  HelperTU1.TranslationUnitId = 1;
  HelperTU1.IsExternalLinkage = false;
  HelperTU1.IsIndirectTarget = true;
  HelperTU1.FunctionTypeId = 0xABCD;
  uint32_t HelperTU1Id = Reconstructor.AddFunctionNode(HelperTU1);
  Reconstructor.AddAddressTakenFact(HelperTU1Id, /*SourceTU=*/1,
                                    /*IsGlobalEscape=*/false);

  // TU 2: local_helper_tu2 also has TypeId 0xABCD, locally address-taken in
  // TU 2.
  CallGraphFunctionNode HelperTU2;
  HelperTU2.TranslationUnitId = 2;
  HelperTU2.IsExternalLinkage = false;
  HelperTU2.IsIndirectTarget = true;
  HelperTU2.FunctionTypeId = 0xABCD;
  uint32_t HelperTU2Id = Reconstructor.AddFunctionNode(HelperTU2);
  Reconstructor.AddAddressTakenFact(HelperTU2Id, /*SourceTU=*/2,
                                    /*IsGlobalEscape=*/false);

  Reconstructor.BuildGraph();

  ASSERT_EQ(Reconstructor.GetEdges().size(), 1u);
  EXPECT_EQ(Reconstructor.GetEdges()[0].FromNodeId, CallerTU1Id);
  EXPECT_EQ(Reconstructor.GetEdges()[0].ToNodeId, HelperTU1Id);
}

TEST(CallGraphSectionTest, EscapedLocalSymbolPromotedToGlobalMap) {
  CallGraphSectionReconstructor Reconstructor;

  // TU 1: caller invokes TypeId 0x1234 indirectly.
  CallGraphFunctionNode Caller;
  Caller.TranslationUnitId = 1;
  Caller.IsExternalLinkage = true;
  Caller.IndirectCalleeTypeIds.push_back(0x1234);
  uint32_t CallerId = Reconstructor.AddFunctionNode(Caller);

  // TU 2: STB_LOCAL function virt_impl whose address escapes via global vtable.
  CallGraphFunctionNode EscapedLocal;
  EscapedLocal.TranslationUnitId = 2;
  EscapedLocal.IsExternalLinkage = false;
  EscapedLocal.IsIndirectTarget = true;
  EscapedLocal.FunctionTypeId = 0x1234;
  uint32_t EscapedLocalId = Reconstructor.AddFunctionNode(EscapedLocal);
  Reconstructor.AddAddressTakenFact(EscapedLocalId, /*SourceTU=*/2,
                                    /*IsGlobalEscape=*/true);

  Reconstructor.BuildGraph();

  ASSERT_EQ(Reconstructor.GetEdges().size(), 1u);
  EXPECT_EQ(Reconstructor.GetEdges()[0].FromNodeId, CallerId);
  EXPECT_EQ(Reconstructor.GetEdges()[0].ToNodeId, EscapedLocalId);
}

TEST(CallGraphSectionTest, NonAddressTakenGlobalPrunedAtLinkTime) {
  CallGraphSectionReconstructor Reconstructor;

  CallGraphFunctionNode Caller;
  Caller.TranslationUnitId = 1;
  Caller.IsExternalLinkage = true;
  Caller.IndirectCalleeTypeIds.push_back(0x5678);
  uint32_t CallerId = Reconstructor.AddFunctionNode(Caller);

  // GlobalFuncNotTaken has IsIndirectTarget=true (as emitted by AsmPrinter for
  // all STB_GLOBAL functions), but zero address-taken relocations in the
  // binary.
  CallGraphFunctionNode GlobalFuncNotTaken;
  GlobalFuncNotTaken.TranslationUnitId = 2;
  GlobalFuncNotTaken.IsExternalLinkage = true;
  GlobalFuncNotTaken.IsIndirectTarget = true;
  GlobalFuncNotTaken.FunctionTypeId = 0x5678;
  Reconstructor.AddFunctionNode(GlobalFuncNotTaken);

  // GlobalFuncTaken actually has its address taken in .rodata / code.
  CallGraphFunctionNode GlobalFuncTaken;
  GlobalFuncTaken.TranslationUnitId = 3;
  GlobalFuncTaken.IsExternalLinkage = true;
  GlobalFuncTaken.IsIndirectTarget = true;
  GlobalFuncTaken.FunctionTypeId = 0x5678;
  uint32_t TakenId = Reconstructor.AddFunctionNode(GlobalFuncTaken);
  Reconstructor.AddAddressTakenFact(TakenId, /*SourceTU=*/3,
                                    /*IsGlobalEscape=*/true);

  Reconstructor.BuildGraph();

  ASSERT_EQ(Reconstructor.GetEdges().size(), 1u);
  EXPECT_EQ(Reconstructor.GetEdges()[0].FromNodeId, CallerId);
  EXPECT_EQ(Reconstructor.GetEdges()[0].ToNodeId, TakenId);
}

TEST(CallGraphSectionTest, FanOutThresholdCapsGenericCliques) {
  CallGraphSectionReconstructor Reconstructor;

  CallGraphFunctionNode Caller;
  Caller.TranslationUnitId = 1;
  Caller.IsExternalLinkage = true;
  Caller.IndirectCalleeTypeIds.push_back(0xDEAD);
  Reconstructor.AddFunctionNode(Caller);

  for (uint32_t I = 0; I < 65; ++I) {
    CallGraphFunctionNode Target;
    Target.TranslationUnitId = 2;
    Target.IsExternalLinkage = true;
    Target.IsIndirectTarget = true;
    Target.FunctionTypeId = 0xDEAD;
    uint32_t TargetId = Reconstructor.AddFunctionNode(Target);
    Reconstructor.AddAddressTakenFact(TargetId, /*SourceTU=*/2,
                                      /*IsGlobalEscape=*/true);
  }

  Reconstructor.BuildGraph();
  EXPECT_EQ(Reconstructor.GetEdges().size(), 0u);
}

} // namespace
