//===- CallGraphSection.h - LLD Call Graph Section Reconstructor *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares CallGraphSectionReconstructor, a standalone library for
// reconstructing a whole-program directed, weighted call graph from
// compiler-emitted .llvm.callgraph sections for linker code layout (LLD).
//
// It implements the 4-phase ELF-precision reconstruction algorithm:
//   1. ELF Section & Symbol Canonicalization (STT_SECTION / STT_FUNC /
//      SHF_EXECINSTR validation and effective binding resolution).
//   2. Whole-program link-time address-taken and escape verification across
//      SHF_ALLOC data/vtable and non-branch code relocations.
//   3. Dual-index scoped target maps (GlobalTypeIdToTargets vs.
//      LocalTypeIdToTargets[TU]) isolating unescaped STB_LOCAL functions.
//   4. Direct and indirect edge synthesis with fan-out bounding (N_max = 64)
//      and inverse-square weight attenuation (W_base / N^2).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_OBJECT_CALLGRAPHSECTION_H
#define LLVM_OBJECT_CALLGRAPHSECTION_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/DataExtractor.h"
#include <cstdint>
#include <vector>

namespace llvm {
namespace object {

/// Constants for .llvm.callgraph binary format v0.
struct CallGraphSectionFormat {
  static constexpr uint8_t FormatVersion = 0;
  static constexpr uint8_t FlagIsIndirectTarget = 1U << 0;
  static constexpr uint8_t FlagHasDirectCallees = 1U << 1;
  static constexpr uint8_t FlagHasIndirectCallees = 1U << 2;

  static constexpr uint32_t MaxIndirectTargets = 64;
  static constexpr uint64_t DirectCallWeight = 100000;
  static constexpr uint64_t IndirectCallBaseWeight = 50000;
};

/// Represents a function section node registered for call graph reconstruction.
struct CallGraphFunctionNode {
  uint32_t NodeId = 0;
  uint32_t TranslationUnitId = 0;
  uint64_t FunctionTypeId = 0;
  bool IsIndirectTarget = false;
  bool IsExternalLinkage = false;
  bool IsExecutable = true;
  SmallVector<uint32_t, 4> DirectCalleeNodeIds;
  SmallVector<uint64_t, 4> IndirectCalleeTypeIds;
};

/// Represents a synthesized weighted edge between two function nodes.
struct ReconstructedCallGraphEdge {
  uint32_t FromNodeId = 0;
  uint32_t ToNodeId = 0;
  uint64_t Weight = 0;
};

/// Reconstructs a whole-program weighted call graph from .llvm.callgraph
/// metadata for link-time function layout in LLD.
class CallGraphSectionReconstructor {
public:
  CallGraphSectionReconstructor() = default;

  /// Register a function section node and return its assigned NodeId.
  uint32_t AddFunctionNode(CallGraphFunctionNode Node);

  /// Retrieve a mutable reference to an existing function node by NodeId.
  CallGraphFunctionNode &GetFunctionNode(uint32_t NodeId) {
    return Nodes[NodeId];
  }

  /// Record that TargetNodeId has its address taken in code or data within
  /// SourceTranslationUnitId. If IsGlobalEscape is true (e.g., referenced from
  /// an exported STB_GLOBAL data table/vtable or code), TargetNodeId is marked
  /// as globally address-taken across all TUs.
  void AddAddressTakenFact(uint32_t TargetNodeId,
                           uint32_t SourceTranslationUnitId,
                           bool IsGlobalEscape);

  /// Execute the 4-phase ELF-precision call graph reconstruction algorithm.
  void BuildGraph();

  /// Access the synthesized weighted call graph edges.
  ArrayRef<ReconstructedCallGraphEdge> GetEdges() const { return Edges; }

  /// Access the registered function nodes.
  ArrayRef<CallGraphFunctionNode> GetNodes() const { return Nodes; }

  /// Helper to inspect x86-64 machine code bytes immediately preceding a
  /// relocation offset in an executable section to determine if the relocation
  /// is a direct call/jump branch rather than a function pointer address-take.
  static bool IsX86_64DirectBranchInstruction(ArrayRef<uint8_t> SectionData,
                                              uint64_t RelocationOffset);

private:
  std::vector<CallGraphFunctionNode> Nodes;
  DenseSet<uint32_t> GloballyAddressTaken;
  DenseMap<uint32_t, DenseSet<uint32_t>> LocallyAddressTaken;
  std::vector<ReconstructedCallGraphEdge> Edges;
};

} // namespace object
} // namespace llvm

#endif // LLVM_OBJECT_CALLGRAPHSECTION_H
