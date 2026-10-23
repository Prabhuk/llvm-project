//===- StackSizeGraph.cpp - Call graph with frame sizes of an ELF file ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "StackSizeGraph.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/DataExtractor.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::stacksize;

std::string Function::getDisplayName() const {
  if (!Names.empty())
    return Names.front();
  if (isExternal())
    return "<external>";
  return "0x" + utohexstr(Address, /*LowerCase=*/true);
}

ArrayRef<Function *> StackSizeGraph::findByName(StringRef Name) const {
  auto It = ByName.find(Name);
  if (It == ByName.end())
    return {};
  return It->second;
}

uint64_t StackSizeGraph::canonicalizeAddress(uint64_t Address) const {
  if (Machine == ELF::EM_ARM)
    return Address & ~uint64_t(1);
  return Address;
}

Function &StackSizeGraph::getOrCreateFunction(uint64_t Address) {
  Function *&F = ByAddress[Address];
  if (!F) {
    Storage.push_back(std::make_unique<Function>());
    F = Storage.back().get();
    F->Address = Address;
  }
  return *F;
}

static bool byAddress(const Function *A, const Function *B) {
  return A->Address < B->Address;
}

void StackSizeGraph::finalize() {
  Functions.clear();
  for (auto &[Address, F] : ByAddress)
    Functions.push_back(F);
  llvm::sort(Functions, byAddress);

  ByName.clear();
  UnknownTypeIndirectTargets.clear();
  for (Function *F : Functions) {
    llvm::sort(F->Names);
    for (const std::string &Name : F->Names)
      ByName[Name].push_back(F);
    if (F->HasUnknownTypeId)
      UnknownTypeIndirectTargets.push_back(F);
    F->Callees.clear();
    // Indirect call sites with a popular type can list thousands of targets,
    // so the dedup must not be quadratic.
    SmallPtrSet<Function *, 16> Seen;
    for (Function *Callee : F->DirectCallees)
      if (Seen.insert(Callee).second)
        F->Callees.push_back(Callee);
    for (const IndirectCallSite &Site : F->IndirectCalls)
      for (Function *Target : Site.Targets)
        if (Seen.insert(Target).second)
          F->Callees.push_back(Target);
  }
  llvm::sort(DynamicFunctions, byAddress);
  DynamicFunctions.erase(llvm::unique(DynamicFunctions),
                         DynamicFunctions.end());

  Root = std::make_unique<Function>();
  Root->Address = UINT64_MAX;
  Root->Names.push_back("<root>");
  Root->HasRecord = true;
  Root->Callees.assign(Functions.begin(), Functions.end());
}

namespace {

/// The records of all .llvm.callgraph sections merged by function address.
/// Identical code folding can leave several records at one address.
struct MergedRecord {
  bool IsIndirectTarget = false;
  bool HasUnknownTypeId = false;
  SmallVector<uint64_t, 1> TypeIds;
  SmallVector<uint64_t, 4> DirectCallees;
  SmallVector<uint64_t, 2> IndirectTypeIds;
};

template <typename T> void appendUnique(SmallVectorImpl<T> &Vec, T Value) {
  if (!is_contained(Vec, Value))
    Vec.push_back(Value);
}

} // namespace

template <class ELFT>
Expected<StackSizeGraph>
StackSizeGraph::createImpl(const ELFObjectFile<ELFT> &Obj,
                           WarningHandler Warn) {
  const ELFFile<ELFT> &EF = Obj.getELFFile();
  const typename ELFT::Ehdr &Header = EF.getHeader();
  if (Header.e_type == ELF::ET_REL)
    return createStringError(
        inconvertibleErrorCode(),
        "relocatable object file; link it first, the call graph addresses are "
        "resolved by the linker");

  StackSizeGraph G;
  G.setMachine(Header.e_machine);

  Expected<typename ELFT::ShdrRange> SectionsOrErr = EF.sections();
  if (!SectionsOrErr)
    return SectionsOrErr.takeError();

  // Symbol names. Every defined function symbol names its address; the
  // dynamic symbol table also provides the exported functions.
  DenseMap<uint64_t, SmallVector<std::string, 1>> NamesByAddress;
  SmallSet<uint64_t, 16> DynamicAddresses;
  for (const typename ELFT::Shdr &Sec : *SectionsOrErr) {
    if (Sec.sh_type != ELF::SHT_SYMTAB && Sec.sh_type != ELF::SHT_DYNSYM)
      continue;
    Expected<typename ELFT::SymRange> SymsOrErr = EF.symbols(&Sec);
    if (!SymsOrErr) {
      Warn("unable to read symbols: " + toString(SymsOrErr.takeError()));
      continue;
    }
    Expected<StringRef> StrTabOrErr = EF.getStringTableForSymtab(Sec);
    if (!StrTabOrErr) {
      Warn("unable to read symbol names: " + toString(StrTabOrErr.takeError()));
      continue;
    }
    for (const typename ELFT::Sym &Sym : SymsOrErr->drop_front()) {
      if (Sym.isUndefined() || (Sym.getType() != ELF::STT_FUNC &&
                                Sym.getType() != ELF::STT_GNU_IFUNC))
        continue;
      Expected<StringRef> NameOrErr = Sym.getName(*StrTabOrErr);
      if (!NameOrErr) {
        Warn("unable to read a symbol name: " +
             toString(NameOrErr.takeError()));
        continue;
      }
      if (NameOrErr->empty())
        continue;
      uint64_t Address = G.canonicalizeAddress(Sym.st_value);
      appendUnique(NamesByAddress[Address], NameOrErr->str());
      if (Sec.sh_type == ELF::SHT_DYNSYM)
        DynamicAddresses.insert(Address);
    }
  }

  // Frame sizes. Several entries at one address, from identical code folding,
  // resolve to the largest frame.
  constexpr uint64_t AddressSize = sizeof(typename ELFT::uint);
  DenseMap<uint64_t, uint64_t> FrameSizes;
  for (const typename ELFT::Shdr &Sec : *SectionsOrErr) {
    Expected<StringRef> NameOrErr = EF.getSectionName(Sec);
    if (!NameOrErr) {
      Warn("unable to read a section name: " + toString(NameOrErr.takeError()));
      continue;
    }
    if (*NameOrErr != ".stack_sizes")
      continue;
    Expected<ArrayRef<uint8_t>> ContentsOrErr = EF.getSectionContents(Sec);
    if (!ContentsOrErr)
      return ContentsOrErr.takeError();
    DataExtractor Data(*ContentsOrErr, EF.isLE());
    DataExtractor::Cursor C(0);
    while (C && C.tell() < ContentsOrErr->size()) {
      uint64_t Address = Data.getUnsigned(C, AddressSize);
      uint64_t Size = Data.getULEB128(C);
      if (!C)
        return createStringError(inconvertibleErrorCode(),
                                 "malformed .stack_sizes section: " +
                                     toString(C.takeError()));
      // The linker resolves the entry of a discarded function to address 0.
      if (Address == 0)
        continue;
      Address = G.canonicalizeAddress(Address);
      uint64_t &Frame = FrameSizes[Address];
      Frame = std::max(Frame, Size);
    }
  }

  // Call graph records.
  DenseMap<uint64_t, MergedRecord> Records;
  bool SawCallGraphSection = false;
  for (const typename ELFT::Shdr &Sec : *SectionsOrErr) {
    if (Sec.sh_type != ELF::SHT_LLVM_CALL_GRAPH)
      continue;
    SawCallGraphSection = true;
    Expected<std::vector<CallGraphRecord>> RecordsOrErr =
        EF.decodeCallGraphSection(Sec);
    if (!RecordsOrErr)
      return RecordsOrErr.takeError();
    for (const CallGraphRecord &Record : *RecordsOrErr) {
      uint64_t Address = G.canonicalizeAddress(Record.Function.Value);
      // The linker resolves the entry of a discarded function to address 0.
      if (Address == 0)
        continue;
      MergedRecord &M = Records[Address];
      if (Record.isIndirectTarget()) {
        M.IsIndirectTarget = true;
        if (Record.FunctionTypeId == 0)
          M.HasUnknownTypeId = true;
        else
          appendUnique(M.TypeIds, Record.FunctionTypeId);
      }
      for (const CallGraphRecord::AddressField &Callee : Record.DirectCallees)
        appendUnique(M.DirectCallees, G.canonicalizeAddress(Callee.Value));
      for (uint64_t TypeId : Record.IndirectCalleeTypeIds)
        appendUnique(M.IndirectTypeIds, TypeId);
    }
  }
  if (!SawCallGraphSection)
    Warn("no SHT_LLVM_CALL_GRAPH section found; compile with "
         "-fexperimental-call-graph-section to get call graph information");

  // Create a function for every address the file knows about, then connect
  // them.
  for (const auto &[Address, Names] : NamesByAddress) {
    Function &F = G.getOrCreateFunction(Address);
    F.Names = Names;
    if (DynamicAddresses.contains(Address))
      G.addDynamicFunction(&F);
  }
  for (const auto &[Address, Size] : FrameSizes)
    G.getOrCreateFunction(Address).FrameSize = Size;
  if (Header.e_entry != 0)
    G.setEntryFunction(
        &G.getOrCreateFunction(G.canonicalizeAddress(Header.e_entry)));
  for (const auto &[Address, M] : Records) {
    Function &F = G.getOrCreateFunction(Address);
    F.HasRecord = true;
    F.IsIndirectTarget = M.IsIndirectTarget;
    F.HasUnknownTypeId = M.HasUnknownTypeId;
    F.TypeIds = M.TypeIds;
    for (uint64_t Callee : M.DirectCallees)
      G.getOrCreateFunction(Callee);
  }

  DenseMap<uint64_t, SmallVector<Function *, 4>> TargetsByTypeId;
  for (const auto &[Address, M] : Records) {
    Function &F = *G.find(Address);
    for (uint64_t TypeId : F.TypeIds)
      TargetsByTypeId[TypeId].push_back(&F);
  }
  for (auto &[TypeId, Targets] : TargetsByTypeId)
    llvm::sort(Targets, byAddress);

  for (const auto &[Address, M] : Records) {
    Function &F = *G.find(Address);
    for (uint64_t Callee : M.DirectCallees)
      F.DirectCallees.push_back(G.find(Callee));
    for (uint64_t TypeId : M.IndirectTypeIds) {
      auto It = TargetsByTypeId.find(TypeId);
      if (It == TargetsByTypeId.end())
        F.UnresolvedIndirectTypeIds.push_back(TypeId);
      else
        F.IndirectCalls.push_back({TypeId, It->second});
    }
  }

  G.finalize();
  return std::move(G);
}

Expected<StackSizeGraph> StackSizeGraph::create(const ELFObjectFileBase &Obj,
                                                WarningHandler Warn) {
  if (const auto *ELF = dyn_cast<ELF32LEObjectFile>(&Obj))
    return createImpl(*ELF, Warn);
  if (const auto *ELF = dyn_cast<ELF32BEObjectFile>(&Obj))
    return createImpl(*ELF, Warn);
  if (const auto *ELF = dyn_cast<ELF64LEObjectFile>(&Obj))
    return createImpl(*ELF, Warn);
  return createImpl(*cast<ELF64BEObjectFile>(&Obj), Warn);
}
