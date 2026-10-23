//===- ELFCallGraph.h - PC-keyed call graph from ELF objects ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Extracts a PC-keyed call graph and per-function stack frame sizes directly
// from a linked ELF object file's `.llvm.callgraph` (SHT_LLVM_CALL_GRAPH) and
// `.stack_sizes` sections.
//
// Design invariants:
//   - Functions are identified strictly by their linked virtual address
//     (with bit 0 cleared on EM_ARM for Thumb code), never by symbol name.
//   - Symbol tables (.dynsym and .symtab) are used only to discover default
//     exported entry points, match user-supplied symbol name filters, and
//     attach human-readable display names to addresses.
//   - Multiple .stack_sizes entries at the same address (from Identical Code
//     Folding) resolve to the maximum frame size at that address.
//   - A function in .llvm.callgraph with no .stack_sizes entry has an unknown
//     frame size (std::nullopt), never 0.
//   - Relocatable object files (ET_REL) are rejected with an Error because
//     their address fields hold relocation placeholders rather than final
//     virtual addresses.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CALLGRAPHSECTION_ELFCALLGRAPH_H
#define LLVM_CALLGRAPHSECTION_ELFCALLGRAPH_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CallGraphSection/CallGraph.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
namespace callgraph {

/// Resolved targets for one indirect call signature invoked by a function.
struct ELFIndirectCallGroup {
  uint64_t TypeId = 0;
  SmallVector<uint64_t, 4> TargetAddresses;
};

/// Per-function call graph and stack frame metadata keyed by virtual address.
struct ELFFunctionInfo {
  /// Linked virtual address of the function (Thumb bit cleared on EM_ARM).
  uint64_t Address = 0;

  /// NodeId in the underlying CallGraph.
  NodeId Id = InvalidNodeId;

  /// Primary human-readable name for reports/diagnostics.
  std::string PrimaryName;

  /// All defined symbol names at this function's address, sorted.
  SmallVector<std::string, 2> Names;

  /// Stack frame size in bytes (including any leaf red-zone allowance), or
  /// std::nullopt if the function has no `.stack_sizes` entry.
  std::optional<uint64_t> StackSize;

  /// True if the `.llvm.callgraph` record(s) at this address mark the function
  /// as a potential indirect call target.
  bool IsIndirectTarget = false;

  /// True if any `.llvm.callgraph` record at this address marks the function
  /// as an indirect target with an unknown type ID (0).
  bool HasUnknownTypeId = false;

  /// Non-zero function type IDs associated with this address.
  SmallVector<uint64_t, 1> TypeIds;

  /// Direct callees that have `.llvm.callgraph` records (in encounter order,
  /// deduplicated).
  SmallVector<uint64_t, 4> DirectCallees;

  /// Resolved indirect call target groups (one per resolved signature).
  SmallVector<ELFIndirectCallGroup, 2> IndirectCalls;

  /// Direct callee addresses that have no `.llvm.callgraph` record (including
  /// address 0 for unresolved external / PLT calls).
  SmallVector<uint64_t, 2> UninstrumentedCallees;

  /// Indirect call type IDs invoked by this function that matched no target in
  /// `.llvm.callgraph` (including type ID 0).
  SmallVector<uint64_t, 2> UnresolvedIndirectTypeIds;
};

/// Options controlling ELF call graph extraction.
struct ELFCallGraphOptions {
  /// Function symbol names to exclude from the call graph. Any function whose
  /// symbol names intersect this list is omitted from vertices, indirect target
  /// sets, and call edges (calls to it are ignored rather than flagged as
  /// uninstrumented).
  SmallVector<std::string, 0> ExcludedFunctions;

  /// Optional override for the per-call frame overhead in bytes. When
  /// std::nullopt, the architecture default from the ELF header is used
  /// (8 on EM_X86_64, 4 on EM_386, 0 otherwise).
  std::optional<uint64_t> CallFrameOverhead;
};

/// Returns the default number of bytes pushed onto the stack by a call
/// instruction on \p EMachine (8 on EM_X86_64, 4 on EM_386, 0 otherwise).
uint64_t defaultCallFrameOverhead(uint16_t EMachine);

/// Returns the ABI red-zone size in bytes below the stack pointer that a leaf
/// function may use without adjusting the stack pointer (128 on EM_X86_64,
/// 0 otherwise).
uint64_t defaultRedZoneSize(uint16_t EMachine);

/// Formats a code address as a symbolizer markup PC element, e.g.
/// `{{{pc:0x1000}}}`.
std::string formatSymbolizerMarkupPC(uint64_t Address);

/// Whole-binary PC-keyed call graph and stack size data extracted from a
/// linked ELF object file.
class ELFCallGraph {
public:
  /// Extract the call graph and stack sizes from a linked ELF object file.
  /// Returns an Error if \p Obj is a relocatable file (`ET_REL`) or if any
  /// `.llvm.callgraph` or `.stack_sizes` section is malformed.
  static Expected<ELFCallGraph>
  create(const object::ELFObjectFileBase &Obj,
         const ELFCallGraphOptions &Opts = ELFCallGraphOptions());

  uint16_t getEMachine() const { return EMachine; }
  uint64_t getCallFrameOverhead() const { return CallFrameOverhead; }
  uint64_t getRedZoneSize() const { return RedZoneSize; }

  /// Non-zero ELF header entry point (`e_entry`), if present.
  std::optional<uint64_t> getElfEntryPoint() const { return ElfEntryPoint; }

  /// Addresses of defined `STT_FUNC` symbols in `.dynsym`, sorted.
  ArrayRef<uint64_t> getDynamicFunctionAddresses() const {
    return DynamicFunctionAddresses;
  }

  /// Defined `STT_FUNC` symbol names in `.dynsym` by address.
  const DenseMap<uint64_t, SmallVector<std::string, 2>> &
  getDynamicFunctionSymbols() const {
    return DynamicFunctionSymbols;
  }

  /// Defined `STT_FUNC` symbol names across `.dynsym` and `.symtab` by address.
  const DenseMap<uint64_t, SmallVector<std::string, 2>> &
  getDefinedFunctionSymbols() const {
    return DefinedFunctionSymbols;
  }

  /// Human-readable display names for an address (empty if none).
  ArrayRef<std::string> getDisplayNames(uint64_t Address) const;

  /// Primary display name for an uninstrumented callee address (`"<external>"`
  /// for address 0 if unnamed, or `"0x..."`).
  std::string getUninstrumentedCalleeName(uint64_t Address) const;

  /// Function addresses in `.llvm.callgraph` in section order.
  ArrayRef<uint64_t> getFunctionAddresses() const { return FunctionAddresses; }

  /// Lookup function info by virtual address, or nullptr if not in
  /// `.llvm.callgraph`.
  const ELFFunctionInfo *findFunction(uint64_t Address) const {
    auto It = Functions.find(Address);
    return It != Functions.end() ? &It->second : nullptr;
  }

  const DenseMap<uint64_t, ELFFunctionInfo> &getFunctions() const {
    return Functions;
  }

  /// Non-excluded indirect call targets in `.llvm.callgraph` whose type ID is
  /// unknown (0), in section order.
  ArrayRef<uint64_t> getUnknownTypeIndirectTargets() const {
    return UnknownTypeIndirectTargets;
  }

  /// Raw `.stack_sizes` map (address -> max frame size in bytes, before leaf
  /// red-zone adjustment).
  const DenseMap<uint64_t, uint64_t> &getRawStackSizes() const {
    return RawStackSizes;
  }

  /// Underlying reconstructed CallGraph.
  const CallGraph &getGraph() const { return Graph; }

  /// Symbolizer markup context lines (`{{{reset}}}`, `{{{module:...}}}`,
  /// `{{{mmap:...}}}`) extracted from `NT_GNU_BUILD_ID` and `PT_LOAD` headers.
  ArrayRef<std::string> getSymbolizerMarkupContext() const {
    return SymbolizerMarkupContext;
  }

private:
  uint16_t EMachine = 0;
  uint64_t CallFrameOverhead = 0;
  uint64_t RedZoneSize = 0;
  std::optional<uint64_t> ElfEntryPoint;
  SmallVector<uint64_t, 0> DynamicFunctionAddresses;
  DenseMap<uint64_t, SmallVector<std::string, 2>> DynamicFunctionSymbols;
  DenseMap<uint64_t, SmallVector<std::string, 2>> DefinedFunctionSymbols;
  DenseMap<uint64_t, uint64_t> RawStackSizes;
  SmallVector<uint64_t, 0> FunctionAddresses;
  DenseMap<uint64_t, ELFFunctionInfo> Functions;
  SmallVector<uint64_t, 0> UnknownTypeIndirectTargets;
  SmallVector<std::string, 0> SymbolizerMarkupContext;
  CallGraph Graph;

  template <class ELFT>
  static Expected<ELFCallGraph>
  createImpl(const object::ELFObjectFile<ELFT> &ObjF,
             const ELFCallGraphOptions &Opts);
};

} // namespace callgraph
} // namespace llvm

#endif // LLVM_CALLGRAPHSECTION_ELFCALLGRAPH_H
