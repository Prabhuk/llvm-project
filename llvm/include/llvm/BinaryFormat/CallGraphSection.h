//===- CallGraphSection.h - Call graph section constants --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the on-disk constants of the `.llvm.callgraph`
// (SHT_LLVM_CALL_GRAPH) section emitted under `-fcall-graph-section`.
//
// These constants are the single source of truth shared by the producer
// (AsmPrinter) and every consumer (llvm/CallGraphSection, LLD, BOLT,
// binary analysis tools). Do not duplicate them.
//
// Record layout (format version 0), repeated until the end of the section:
//
//   uint8   FormatVersion
//   uint8   Flags                  (see `Flags` below)
//   addr    FunctionEntryPC        (target address size; relocated)
//   uint64  FunctionTypeId         (0 if unknown or not an indirect target)
//   [ if Flags & HasDirectCallees ]
//     uleb128 NumDirectCallees
//     addr    DirectCalleePC[NumDirectCallees]   (relocated)
//   [ if Flags & HasIndirectCallees ]
//     uleb128 NumIndirectCalleeTypeIds
//     uint64  IndirectCalleeTypeId[NumIndirectCalleeTypeIds]
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BINARYFORMAT_CALLGRAPHSECTION_H
#define LLVM_BINARYFORMAT_CALLGRAPHSECTION_H

#include "llvm/ADT/BitmaskEnum.h"
#include <cstdint>

namespace llvm {
namespace callgraph {

/// Format version of the `.llvm.callgraph` section.
enum FormatVersion : uint8_t {
  V_0 = 0,
};

/// The most recent format version produced and understood by LLVM.
inline constexpr uint8_t LatestFormatVersion = V_0;

// ELF call graph section entry Flag field supported values.
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();
enum Flags : uint8_t {
  None = 0,
  /// The function may be the target of an indirect call.
  IsIndirectTarget = 1u << 0,
  /// The record carries a direct callee array.
  HasDirectCallees = 1u << 1,
  /// The record carries an indirect callee type ID array.
  HasIndirectCallees = 1u << 2,
  LLVM_MARK_AS_BITMASK_ENUM(/*LargestValue=*/HasIndirectCallees)
};

} // namespace callgraph
} // namespace llvm

#endif // LLVM_BINARYFORMAT_CALLGRAPHSECTION_H
