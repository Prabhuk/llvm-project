//===- CallGraphSection.h - Call graph section constants --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the on-disk constants of the `.llvm.callgraph`
// (SHT_LLVM_CALL_GRAPH) section, emitted by clang under
// -fexperimental-call-graph-section and by llc under -call-graph-section.
// They are shared by the producer (AsmPrinter) and every consumer.
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
//     [ if Flags & HasIndirectCalleeFrequencies ]
//       uleb128 IndirectCalleeFrequency[NumIndirectCalleeTypeIds]
//
// An indirect callee frequency is the estimated number of calls through the
// corresponding type ID per call of the function, in units of
// 1/IndirectCalleeFrequencyScale, summed over the function's call sites.
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
  /// The indirect callee type ID array is followed by a frequency array.
  HasIndirectCalleeFrequencies = 1u << 3,
  LLVM_MARK_AS_BITMASK_ENUM(/*LargestValue=*/HasIndirectCalleeFrequencies)
};

/// Bitmask of all defined `Flags` bits in format version 0.
inline constexpr Flags ValidFlags = IsIndirectTarget | HasDirectCallees |
                                    HasIndirectCallees |
                                    HasIndirectCalleeFrequencies;

/// Frequency of a call that executes once per call of its caller.
inline constexpr uint64_t IndirectCalleeFrequencyScale = 1024;

} // namespace callgraph
} // namespace llvm

#endif // LLVM_BINARYFORMAT_CALLGRAPHSECTION_H
