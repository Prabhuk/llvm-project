//===- StackSizeGraph.h - Call graph with frame sizes of an ELF file ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// StackSizeGraph is the call graph of a linked ELF file, reconstructed from
// its .llvm.callgraph and .stack_sizes sections, with the frame size of every
// function attached. Functions are identified by their entry address, since
// symbol names are neither unique nor required; names only decorate output
// and select entry points.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_STACK_SIZE_LIB_STACKSIZEGRAPH_H
#define LLVM_TOOLS_LLVM_STACK_SIZE_LIB_STACKSIZEGRAPH_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
namespace object {
class ELFObjectFileBase;
template <class ELFT> class ELFObjectFile;
} // namespace object

namespace stacksize {

class Function;

/// The resolved targets of the indirect calls of one type that a function
/// makes.
struct IndirectCallSite {
  uint64_t TypeId = 0;
  SmallVector<Function *, 4> Targets;
};

/// A function of the file. The special function at address 0 stands for every
/// callee outside the file, such as a function reached through the PLT.
class Function {
public:
  uint64_t Address = 0;

  /// The names of the function symbols defined at Address, sorted.
  SmallVector<std::string, 1> Names;

  /// The frame size recorded in .stack_sizes, or std::nullopt when there is no
  /// entry, which is the case for functions with dynamically sized frames and
  /// for functions compiled without -fstack-size-section.
  std::optional<uint64_t> FrameSize;

  /// Whether a .llvm.callgraph record describes the function. Without one its
  /// callees are unknown.
  bool HasRecord = false;

  /// Whether the function may be the target of an indirect call.
  bool IsIndirectTarget = false;

  /// Whether the function is an indirect call target of unknown type, which
  /// no indirect call site can rule out.
  bool HasUnknownTypeId = false;

  /// The type IDs under which the function may be called indirectly.
  SmallVector<uint64_t, 1> TypeIds;

  /// Functions called directly, in record order and deduplicated.
  SmallVector<Function *, 4> DirectCallees;

  /// Indirect calls whose type matched at least one function.
  SmallVector<IndirectCallSite, 2> IndirectCalls;

  /// Type IDs of indirect calls that matched no function.
  SmallVector<uint64_t, 2> UnresolvedIndirectTypeIds;

  /// Every function reachable by one call, direct or indirect, deduplicated.
  /// These are the successors for graph algorithms.
  SmallVector<Function *, 4> Callees;

  /// The first name, or the address if the function has none.
  std::string getDisplayName() const;

  bool isExternal() const { return Address == 0; }

  /// Whether the record shows any call, resolved or not.
  bool makesCalls() const {
    return !DirectCallees.empty() || !IndirectCalls.empty() ||
           !UnresolvedIndirectTypeIds.empty();
  }

  /// Whether the function is known to make no calls at all.
  bool isLeaf() const { return HasRecord && !makesCalls(); }
};

/// The call graph of a linked ELF file.
///
/// A graph is either read from a file with create(), or built by hand: add
/// functions with getOrCreateFunction(), fill in their fields and callees, and
/// call finalize().
class StackSizeGraph {
public:
  using WarningHandler = function_ref<void(const Twine &)>;

  /// Builds the graph of \p Obj. Problems that do not prevent analysis, such
  /// as a missing .llvm.callgraph section, are reported to \p Warn. The
  /// functions named in \p ExcludedFunctions are left out along with every
  /// call to them, as if they did not exist.
  static Expected<StackSizeGraph>
  create(const object::ELFObjectFileBase &Obj, WarningHandler Warn,
         ArrayRef<std::string> ExcludedFunctions = {});

  StackSizeGraph() = default;
  StackSizeGraph(StackSizeGraph &&) = default;
  StackSizeGraph &operator=(StackSizeGraph &&) = default;

  /// The function at \p Address, created if it does not exist yet. Pointers
  /// to functions stay valid for the life of the graph.
  Function &getOrCreateFunction(uint64_t Address);

  void setMachine(uint16_t M) { Machine = M; }
  void setEntryFunction(Function *F) { EntryFunction = F; }
  void addDynamicFunction(Function *F) { DynamicFunctions.push_back(F); }

  /// Completes a graph built by hand: derives every function's Callees from
  /// its direct and indirect calls, sorts, indexes the names and creates the
  /// root. No function or edge may be added afterwards.
  void finalize();

  uint16_t getMachine() const { return Machine; }

  /// All functions, sorted by address.
  ArrayRef<Function *> functions() const { return Functions; }

  /// The function at \p Address, or nullptr.
  Function *find(uint64_t Address) const { return ByAddress.lookup(Address); }

  /// The functions named \p Name, sorted by address.
  ArrayRef<Function *> findByName(StringRef Name) const;

  /// Every function that has a name, by name.
  const StringMap<SmallVector<Function *, 1>> &getNamedFunctions() const {
    return ByName;
  }

  /// The function at the ELF entry point, if the header has one.
  Function *getEntryFunction() const { return EntryFunction; }

  /// The functions exported through the dynamic symbol table, sorted by
  /// address.
  ArrayRef<Function *> getDynamicFunctions() const { return DynamicFunctions; }

  /// Indirect call targets whose type is unknown, sorted by address.
  ArrayRef<Function *> getUnknownTypeIndirectTargets() const {
    return UnknownTypeIndirectTargets;
  }

  /// A function outside the file whose callees are all functions, so that
  /// graph algorithms that start from a single entry reach every function.
  Function *getRoot() const { return Root.get(); }

  /// Clears the Thumb bit of \p Address on EM_ARM, where it marks the
  /// instruction set rather than being part of the address.
  uint64_t canonicalizeAddress(uint64_t Address) const;

private:
  template <class ELFT>
  static Expected<StackSizeGraph>
  createImpl(const object::ELFObjectFile<ELFT> &Obj, WarningHandler Warn,
             ArrayRef<std::string> ExcludedFunctions);

  uint16_t Machine = 0;
  std::vector<std::unique_ptr<Function>> Storage;
  std::unique_ptr<Function> Root;
  SmallVector<Function *, 0> Functions;
  DenseMap<uint64_t, Function *> ByAddress;
  StringMap<SmallVector<Function *, 1>> ByName;
  Function *EntryFunction = nullptr;
  SmallVector<Function *, 0> DynamicFunctions;
  SmallVector<Function *, 0> UnknownTypeIndirectTargets;
};

} // namespace stacksize

template <> struct GraphTraits<const stacksize::StackSizeGraph *> {
  using NodeRef = stacksize::Function *;
  using ChildIteratorType = SmallVectorImpl<NodeRef>::const_iterator;
  using nodes_iterator = ArrayRef<NodeRef>::const_iterator;

  static NodeRef getEntryNode(const stacksize::StackSizeGraph *G) {
    return G->getRoot();
  }
  static ChildIteratorType child_begin(NodeRef N) { return N->Callees.begin(); }
  static ChildIteratorType child_end(NodeRef N) { return N->Callees.end(); }
  static nodes_iterator nodes_begin(const stacksize::StackSizeGraph *G) {
    return G->functions().begin();
  }
  static nodes_iterator nodes_end(const stacksize::StackSizeGraph *G) {
    return G->functions().end();
  }
  static unsigned size(const stacksize::StackSizeGraph *G) {
    return G->functions().size();
  }
};

} // namespace llvm

#endif // LLVM_TOOLS_LLVM_STACK_SIZE_LIB_STACKSIZEGRAPH_H
