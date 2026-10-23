//===- ELFCallGraph.cpp - PC-keyed call graph from ELF objects ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/ELFCallGraph.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CallGraphSection/RecordReader.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::callgraph;
using namespace llvm::object;

uint64_t llvm::callgraph::defaultCallFrameOverhead(uint16_t EMachine) {
  switch (EMachine) {
  case ELF::EM_X86_64:
    return 8;
  case ELF::EM_386:
    return 4;
  default:
    return 0;
  }
}

uint64_t llvm::callgraph::defaultRedZoneSize(uint16_t EMachine) {
  switch (EMachine) {
  case ELF::EM_X86_64:
    return 128;
  default:
    return 0;
  }
}

std::string llvm::callgraph::formatSymbolizerMarkupPC(uint64_t Address) {
  return "{{{pc:0x" + utohexstr(Address, /*LowerCase=*/true) + "}}}";
}

ArrayRef<std::string> ELFCallGraph::getDisplayNames(uint64_t Address) const {
  auto It = DefinedFunctionSymbols.find(Address);
  if (It != DefinedFunctionSymbols.end())
    return It->second;
  return {};
}

std::string ELFCallGraph::getUninstrumentedCalleeName(uint64_t Address) const {
  ArrayRef<std::string> Names = getDisplayNames(Address);
  if (!Names.empty())
    return Names.front();
  if (Address == 0)
    return "<external>";
  return "0x" + utohexstr(Address, /*LowerCase=*/true);
}

namespace {

uint64_t clearThumbBit(uint64_t Address, uint16_t EMachine) {
  if (EMachine == ELF::EM_ARM)
    return Address & ~uint64_t(1);
  return Address;
}

template <typename T> void appendUnique(SmallVectorImpl<T> &Vec, const T &Val) {
  if (!is_contained(Vec, Val))
    Vec.push_back(Val);
}

template <class ELFT>
Error readSymbolTable(
    const ELFFile<ELFT> &EF, const typename ELFT::Shdr &Sec, uint16_t EMachine,
    DenseMap<uint64_t, SmallVector<std::string, 2>> &OutSymbols) {
  Expected<typename ELFT::SymRange> SymsOrErr = EF.symbols(&Sec);
  if (!SymsOrErr)
    return SymsOrErr.takeError();
  Expected<StringRef> StrTabOrErr = EF.getStringTableForSymtab(Sec);
  if (!StrTabOrErr)
    return StrTabOrErr.takeError();
  StringRef StrTab = *StrTabOrErr;

  for (const typename ELFT::Sym &Sym : *SymsOrErr) {
    if (Sym.getType() != ELF::STT_FUNC || Sym.st_shndx == ELF::SHN_UNDEF)
      continue;
    Expected<StringRef> NameOrErr = Sym.getName(StrTab);
    if (!NameOrErr) {
      consumeError(NameOrErr.takeError());
      continue;
    }
    if (NameOrErr->empty())
      continue;
    uint64_t Addr = clearThumbBit(Sym.st_value, EMachine);
    appendUnique(OutSymbols[Addr], NameOrErr->str());
  }
  return Error::success();
}

template <class ELFT>
void extractSymbolizerMarkup(const ELFFile<ELFT> &EF, StringRef FileName,
                             SmallVectorImpl<std::string> &OutLines) {
  std::string BuildId;
  if (Expected<typename ELFT::ShdrRange> SecsOrErr = EF.sections()) {
    for (const typename ELFT::Shdr &Sec : *SecsOrErr) {
      if (Sec.sh_type != ELF::SHT_NOTE)
        continue;
      Error Err = Error::success();
      for (const typename ELFT::Note &Note : EF.notes(Sec, Err)) {
        if (Note.getName() == "GNU" && Note.getType() == ELF::NT_GNU_BUILD_ID) {
          ArrayRef<uint8_t> Desc = Note.getDesc(Sec.sh_addralign);
          BuildId = toHex(Desc, /*LowerCase=*/true);
          break;
        }
      }
      consumeError(std::move(Err));
      if (!BuildId.empty())
        break;
    }
  } else {
    consumeError(SecsOrErr.takeError());
  }

  if (BuildId.empty())
    return;

  Expected<typename ELFT::PhdrRange> PhdrsOrErr = EF.program_headers();
  if (!PhdrsOrErr) {
    consumeError(PhdrsOrErr.takeError());
    return;
  }

  SmallVector<const typename ELFT::Phdr *, 4> LoadSegments;
  for (const typename ELFT::Phdr &Phdr : *PhdrsOrErr)
    if (Phdr.p_type == ELF::PT_LOAD)
      LoadSegments.push_back(&Phdr);

  if (LoadSegments.empty())
    return;

  StringRef BaseName = sys::path::filename(FileName);
  OutLines.push_back("{{{reset}}}");
  OutLines.push_back("{{{module:0:" + BaseName.str() + ":elf:" + BuildId +
                     "}}}");
  for (const typename ELFT::Phdr *Seg : LoadSegments) {
    std::string Perms;
    if (Seg->p_flags & ELF::PF_R)
      Perms.push_back('r');
    if (Seg->p_flags & ELF::PF_W)
      Perms.push_back('w');
    if (Seg->p_flags & ELF::PF_X)
      Perms.push_back('x');
    std::string VAddrHex = utohexstr(Seg->p_vaddr, /*LowerCase=*/true);
    std::string MemSizeHex = utohexstr(Seg->p_memsz, /*LowerCase=*/true);
    OutLines.push_back("{{{mmap:0x" + VAddrHex + ":0x" + MemSizeHex +
                       ":load:0:" + Perms + ":0x" + VAddrHex + "}}}");
  }
}

} // namespace

template <class ELFT>
Expected<ELFCallGraph>
ELFCallGraph::createImpl(const ELFObjectFile<ELFT> &ObjF,
                         const ELFCallGraphOptions &Opts) {
  const ELFFile<ELFT> &EF = ObjF.getELFFile();
  const typename ELFT::Ehdr &Header = EF.getHeader();

  if (Header.e_type == ELF::ET_REL)
    return createStringError(
        inconvertibleErrorCode(),
        "call graph input is for a relocatable object file; link it first");

  ELFCallGraph Result;
  Result.EMachine = Header.e_machine;
  Result.CallFrameOverhead = Opts.CallFrameOverhead.value_or(
      defaultCallFrameOverhead(Result.EMachine));
  Result.RedZoneSize = defaultRedZoneSize(Result.EMachine);

  if (Header.e_entry != 0)
    Result.ElfEntryPoint = clearThumbBit(Header.e_entry, Result.EMachine);

  Expected<typename ELFT::ShdrRange> SectionsOrErr = EF.sections();
  if (!SectionsOrErr)
    return SectionsOrErr.takeError();

  // Step 1: Read symbol tables (.dynsym and .symtab).
  for (const typename ELFT::Shdr &Sec : *SectionsOrErr) {
    if (Sec.sh_type == ELF::SHT_DYNSYM) {
      if (Error E = readSymbolTable(EF, Sec, Result.EMachine,
                                    Result.DynamicFunctionSymbols))
        return std::move(E);
      if (Error E = readSymbolTable(EF, Sec, Result.EMachine,
                                    Result.DefinedFunctionSymbols))
        return std::move(E);
    } else if (Sec.sh_type == ELF::SHT_SYMTAB) {
      if (Error E = readSymbolTable(EF, Sec, Result.EMachine,
                                    Result.DefinedFunctionSymbols))
        return std::move(E);
    }
  }

  for (auto &Entry : Result.DynamicFunctionSymbols) {
    llvm::sort(Entry.second);
    Result.DynamicFunctionAddresses.push_back(Entry.first);
  }
  llvm::sort(Result.DynamicFunctionAddresses);

  for (auto &Entry : Result.DefinedFunctionSymbols)
    llvm::sort(Entry.second);

  // Step 2: Read .stack_sizes sections.
  constexpr uint8_t AddrSize = sizeof(typename ELFT::uint);
  for (const typename ELFT::Shdr &Sec : *SectionsOrErr) {
    Expected<StringRef> NameOrErr = EF.getSectionName(Sec);
    if (!NameOrErr) {
      consumeError(NameOrErr.takeError());
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
      uint64_t RawAddr = Data.getUnsigned(C, AddrSize);
      uint64_t StackSize = Data.getULEB128(C);
      if (!C)
        return createStringError(inconvertibleErrorCode(),
                                 "truncated .stack_sizes section: " +
                                     toString(C.takeError()));
      uint64_t Addr = clearThumbBit(RawAddr, Result.EMachine);
      auto [It, Inserted] = Result.RawStackSizes.try_emplace(Addr, StackSize);
      if (!Inserted)
        It->second = std::max(It->second, StackSize);
    }
  }

  // Step 3: Read SHT_LLVM_CALL_GRAPH (.llvm.callgraph) sections and merge
  // records by function address.
  struct RawMergedFunc {
    uint64_t Address = 0;
    bool IsIndirectTarget = false;
    bool HasUnknownTypeId = false;
    SmallVector<uint64_t, 1> TypeIds;
    SmallVector<uint64_t, 4> DirectCallees;
    SmallVector<uint64_t, 2> IndirectTypeIds;
  };
  SmallVector<RawMergedFunc, 0> RawFuncs;
  DenseMap<uint64_t, size_t> RawFuncIndex;

  for (const typename ELFT::Shdr &Sec : *SectionsOrErr) {
    if (Sec.sh_type != ELF::SHT_LLVM_CALL_GRAPH)
      continue;
    Expected<ArrayRef<uint8_t>> ContentsOrErr = EF.getSectionContents(Sec);
    if (!ContentsOrErr)
      return ContentsOrErr.takeError();

    RecordReader Reader(*ContentsOrErr, EF.isLE(), AddrSize);
    FunctionRecord Rec;
    while (!Reader.atEnd()) {
      if (Error E = Reader.readRecord(Rec))
        return std::move(E);
      uint64_t Addr = clearThumbBit(Rec.FunctionAddress, Result.EMachine);
      auto [It, Inserted] = RawFuncIndex.try_emplace(Addr, RawFuncs.size());
      if (Inserted) {
        RawMergedFunc M;
        M.Address = Addr;
        RawFuncs.push_back(std::move(M));
      }
      RawMergedFunc &M = RawFuncs[It->second];
      if (Rec.isIndirectTarget()) {
        M.IsIndirectTarget = true;
        if (Rec.FunctionTypeId == 0)
          M.HasUnknownTypeId = true;
        else
          appendUnique(M.TypeIds, Rec.FunctionTypeId);
      } else if (Rec.FunctionTypeId != 0) {
        appendUnique(M.TypeIds, Rec.FunctionTypeId);
      }
      for (uint64_t CalleeRawAddr : Rec.DirectCalleeAddresses)
        appendUnique(M.DirectCallees,
                     clearThumbBit(CalleeRawAddr, Result.EMachine));
      for (uint64_t TypeId : Rec.IndirectCalleeTypeIds)
        appendUnique(M.IndirectTypeIds, TypeId);
    }
  }

  // Step 4: Filter excluded functions and register vertices.
  StringSet<> ExcludedNames;
  for (StringRef Name : Opts.ExcludedFunctions)
    ExcludedNames.insert(Name);

  DenseSet<uint64_t> ExcludedAddresses;
  DenseMap<uint64_t, SmallVector<uint64_t, 4>> TypeIdToTargets;
  CallGraphBuilder Builder;

  for (const RawMergedFunc &Raw : RawFuncs) {
    ArrayRef<std::string> Names = Result.getDisplayNames(Raw.Address);
    bool IsExcluded = llvm::any_of(
        Names, [&](const std::string &N) { return ExcludedNames.contains(N); });
    if (IsExcluded) {
      ExcludedAddresses.insert(Raw.Address);
      continue;
    }

    ELFFunctionInfo Info;
    Info.Address = Raw.Address;
    Info.Names.assign(Names.begin(), Names.end());
    Info.PrimaryName = !Names.empty()
                           ? Names.front()
                           : ("<unknown_@0x" +
                              utohexstr(Raw.Address, /*LowerCase=*/true) + ">");
    Info.IsIndirectTarget = Raw.IsIndirectTarget;
    Info.HasUnknownTypeId = Raw.HasUnknownTypeId;
    Info.TypeIds = Raw.TypeIds;

    auto StackIt = Result.RawStackSizes.find(Raw.Address);
    if (StackIt != Result.RawStackSizes.end()) {
      uint64_t FrameSize = StackIt->second;
      bool IsLeaf = Raw.DirectCallees.empty() && Raw.IndirectTypeIds.empty();
      if (IsLeaf)
        FrameSize += Result.RedZoneSize;
      Info.StackSize = FrameSize;
    }

    FunctionNode CGNode;
    CGNode.HasRecord = true;
    CGNode.Module = 1;
    CGNode.IsIndirectTarget = Raw.IsIndirectTarget;
    CGNode.HasUnknownTypeId = Raw.HasUnknownTypeId;
    CGNode.TypeIds = Raw.TypeIds;
    CGNode.IndirectCalleeTypeIds = Raw.IndirectTypeIds;
    NodeId Id = Builder.addFunction(std::move(CGNode));
    Info.Id = Id;

    if (Raw.IsIndirectTarget || !Raw.TypeIds.empty())
      Builder.addAddressTakenFact(Id, /*SourceModule=*/1,
                                  /*IsGlobalEscape=*/true);

    for (uint64_t TypeId : Raw.TypeIds)
      if (TypeId != 0)
        appendUnique(TypeIdToTargets[TypeId], Raw.Address);

    if (Raw.IsIndirectTarget && Raw.HasUnknownTypeId)
      Result.UnknownTypeIndirectTargets.push_back(Raw.Address);

    Result.FunctionAddresses.push_back(Raw.Address);
    Result.Functions.try_emplace(Raw.Address, std::move(Info));
  }

  // Also check if any symbol in the symbol tables (not in .llvm.callgraph) is
  // named in ExcludedFunctions so calls to it are ignored as excluded rather
  // than flagged as uninstrumented.
  for (const auto &SymEntry : Result.DefinedFunctionSymbols) {
    bool IsExcluded = llvm::any_of(SymEntry.second, [&](const std::string &N) {
      return ExcludedNames.contains(N);
    });
    if (IsExcluded)
      ExcludedAddresses.insert(SymEntry.first);
  }

  // Step 5: Resolve direct and indirect call edges.
  for (const RawMergedFunc &Raw : RawFuncs) {
    if (ExcludedAddresses.contains(Raw.Address))
      continue;
    ELFFunctionInfo &Info = Result.Functions[Raw.Address];
    FunctionNode &CGNode = Builder.getFunction(Info.Id);

    for (uint64_t CalleeAddr : Raw.DirectCallees) {
      if (ExcludedAddresses.contains(CalleeAddr))
        continue;
      auto CalleeIt = Result.Functions.find(CalleeAddr);
      if (CalleeIt != Result.Functions.end()) {
        appendUnique(Info.DirectCallees, CalleeAddr);
        CGNode.DirectCallees.push_back(CalleeIt->second.Id);
      } else {
        appendUnique(Info.UninstrumentedCallees, CalleeAddr);
        CGNode.CallsUnknown = true;
      }
    }

    for (uint64_t TypeId : Raw.IndirectTypeIds) {
      auto TargetIt = TypeIdToTargets.find(TypeId);
      if (TargetIt != TypeIdToTargets.end() && !TargetIt->second.empty()) {
        ELFIndirectCallGroup Group;
        Group.TypeId = TypeId;
        for (uint64_t TargetAddr : TargetIt->second)
          if (Result.Functions.contains(TargetAddr))
            Group.TargetAddresses.push_back(TargetAddr);
        if (!Group.TargetAddresses.empty()) {
          Info.IndirectCalls.push_back(std::move(Group));
          continue;
        }
      }
      appendUnique(Info.UnresolvedIndirectTypeIds, TypeId);
    }
  }

  Result.Graph = Builder.build();
  extractSymbolizerMarkup(EF, ObjF.getFileName(),
                          Result.SymbolizerMarkupContext);
  return std::move(Result);
}

Expected<ELFCallGraph> ELFCallGraph::create(const ELFObjectFileBase &Obj,
                                            const ELFCallGraphOptions &Opts) {
  if (const auto *ELF = dyn_cast<ELF32LEObjectFile>(&Obj))
    return createImpl(*ELF, Opts);
  if (const auto *ELF = dyn_cast<ELF32BEObjectFile>(&Obj))
    return createImpl(*ELF, Opts);
  if (const auto *ELF = dyn_cast<ELF64LEObjectFile>(&Obj))
    return createImpl(*ELF, Opts);
  if (const auto *ELF = dyn_cast<ELF64BEObjectFile>(&Obj))
    return createImpl(*ELF, Opts);
  return createStringError(inconvertibleErrorCode(),
                           "unsupported ELF object format");
}
