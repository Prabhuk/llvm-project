//===- CallGraphSection.cpp - LLD Call Graph Section Reconstructor --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Object/CallGraphSection.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::object;

uint32_t
CallGraphSectionReconstructor::AddFunctionNode(CallGraphFunctionNode Node) {
  uint32_t AssignedId = static_cast<uint32_t>(Nodes.size());
  Node.NodeId = AssignedId;
  Nodes.push_back(std::move(Node));
  return AssignedId;
}

void CallGraphSectionReconstructor::AddAddressTakenFact(
    uint32_t TargetNodeId, uint32_t SourceTranslationUnitId,
    bool IsGlobalEscape) {
  if (IsGlobalEscape)
    GloballyAddressTaken.insert(TargetNodeId);
  else
    LocallyAddressTaken[SourceTranslationUnitId].insert(TargetNodeId);
}

bool CallGraphSectionReconstructor::IsX86_64DirectBranchInstruction(
    ArrayRef<uint8_t> SectionData, uint64_t RelocationOffset) {
  if (RelocationOffset >= 1 && RelocationOffset <= SectionData.size()) {
    uint8_t Opcode1 = SectionData[RelocationOffset - 1];
    // 0xE8: CALL rel32, 0xE9: JMP rel32, 0xEB: JMP rel8
    if (Opcode1 == 0xE8 || Opcode1 == 0xE9 || Opcode1 == 0xEB)
      return true;
    // 0x0F 0x80..0x8F: Jcc rel32
    if (RelocationOffset >= 2 && SectionData[RelocationOffset - 2] == 0x0F &&
        (Opcode1 & 0xF0) == 0x80)
      return true;
  }
  return false;
}

void CallGraphSectionReconstructor::BuildGraph() {
  Edges.clear();

  // Phase 3: Dual-Index Scoped Target Maps.
  DenseMap<uint64_t, SmallVector<uint32_t, 4>> GlobalTypeIdToTargets;
  DenseMap<uint32_t, DenseMap<uint64_t, SmallVector<uint32_t, 2>>>
      LocalTypeIdToTargets;

  for (const CallGraphFunctionNode &Node : Nodes) {
    if (!Node.IsExecutable || !Node.IsIndirectTarget)
      continue;

    // Verify whole-program link-time address-taken status to prune
    // single-TU compiler over-approximations (AsmPrinter marks all
    // non-local STB_GLOBAL/STB_WEAK functions IsIndirectTarget=1).
    if (Node.IsExternalLinkage) {
      if (!GloballyAddressTaken.contains(Node.NodeId))
        continue;
    } else {
      auto LocalIt = LocallyAddressTaken.find(Node.TranslationUnitId);
      bool IsLocalTaken = (LocalIt != LocallyAddressTaken.end() &&
                           LocalIt->second.contains(Node.NodeId));
      if (!GloballyAddressTaken.contains(Node.NodeId) && !IsLocalTaken)
        continue;
    }

    if (Node.IsExternalLinkage || GloballyAddressTaken.contains(Node.NodeId)) {
      GlobalTypeIdToTargets[Node.FunctionTypeId].push_back(Node.NodeId);
    } else {
      LocalTypeIdToTargets[Node.TranslationUnitId][Node.FunctionTypeId]
          .push_back(Node.NodeId);
    }
  }

  // Deduplicate target lists for deterministic synthesis.
  for (auto &Entry : GlobalTypeIdToTargets) {
    auto &Vec = Entry.second;
    llvm::sort(Vec);
    Vec.erase(llvm::unique(Vec), Vec.end());
  }
  for (auto &TUEntry : LocalTypeIdToTargets) {
    for (auto &Entry : TUEntry.second) {
      auto &Vec = Entry.second;
      llvm::sort(Vec);
      Vec.erase(llvm::unique(Vec), Vec.end());
    }
  }

  // Phase 4: Direct and Indirect Edge Synthesis with Fan-Out Bounding (64)
  // and Inverse-Square Weight Attenuation (W_base / N^2).
  DenseMap<std::pair<uint32_t, uint32_t>, size_t> EdgeIndexMap;
  auto AddOrAccumulateEdge = [&](uint32_t FromId, uint32_t ToId,
                                 uint64_t Weight) {
    if (FromId == ToId || ToId >= Nodes.size() || !Nodes[ToId].IsExecutable)
      return;
    auto Key = std::make_pair(FromId, ToId);
    auto It = EdgeIndexMap.find(Key);
    if (It != EdgeIndexMap.end()) {
      Edges[It->second].Weight += Weight;
    } else {
      EdgeIndexMap[Key] = Edges.size();
      Edges.push_back({FromId, ToId, Weight});
    }
  };

  for (const CallGraphFunctionNode &Caller : Nodes) {
    if (!Caller.IsExecutable)
      continue;

    for (uint32_t DirectCalleeId : Caller.DirectCalleeNodeIds) {
      AddOrAccumulateEdge(Caller.NodeId, DirectCalleeId,
                          CallGraphSectionFormat::DirectCallWeight);
    }

    for (uint64_t CalleeTypeId : Caller.IndirectCalleeTypeIds) {
      const SmallVector<uint32_t, 4> *GlobalTargets = nullptr;
      const SmallVector<uint32_t, 2> *LocalTargets = nullptr;

      auto GlobalIt = GlobalTypeIdToTargets.find(CalleeTypeId);
      if (GlobalIt != GlobalTypeIdToTargets.end())
        GlobalTargets = &GlobalIt->second;

      auto TUIt = LocalTypeIdToTargets.find(Caller.TranslationUnitId);
      if (TUIt != LocalTypeIdToTargets.end()) {
        auto LocalIt = TUIt->second.find(CalleeTypeId);
        if (LocalIt != TUIt->second.end())
          LocalTargets = &LocalIt->second;
      }

      size_t TotalCandidates = (GlobalTargets ? GlobalTargets->size() : 0) +
                               (LocalTargets ? LocalTargets->size() : 0);
      if (TotalCandidates == 0 ||
          TotalCandidates > CallGraphSectionFormat::MaxIndirectTargets)
        continue;

      uint64_t Denominator =
          static_cast<uint64_t>(TotalCandidates) * TotalCandidates;
      uint64_t AttenuatedWeight = std::max<uint64_t>(
          1, CallGraphSectionFormat::IndirectCallBaseWeight / Denominator);

      if (LocalTargets) {
        for (uint32_t TargetId : *LocalTargets)
          AddOrAccumulateEdge(Caller.NodeId, TargetId, AttenuatedWeight);
      }
      if (GlobalTargets) {
        for (uint32_t TargetId : *GlobalTargets)
          AddOrAccumulateEdge(Caller.NodeId, TargetId, AttenuatedWeight);
      }
    }
  }
}
