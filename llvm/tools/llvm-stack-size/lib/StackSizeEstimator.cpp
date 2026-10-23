//===- StackSizeEstimator.cpp - Worst-case stack usage of entry points ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "StackSizeEstimator.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/GlobPattern.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;
using namespace llvm::stacksize;

uint64_t stacksize::defaultCallFrameOverhead(uint16_t Machine) {
  switch (Machine) {
  case ELF::EM_X86_64:
    return 8;
  case ELF::EM_386:
    return 4;
  default:
    return 0;
  }
}

uint64_t stacksize::defaultRedZoneSize(uint16_t Machine) {
  switch (Machine) {
  case ELF::EM_X86_64:
    return 128;
  case ELF::EM_PPC64:
    return 288;
  default:
    return 0;
  }
}

static bool byAddress(const Function *A, const Function *B) {
  return A->Address < B->Address;
}

Expected<std::vector<EntryPoint>>
stacksize::resolveEntryPoints(const StackSizeGraph &G,
                              ArrayRef<std::string> Specs) {
  std::vector<EntryPoint> Entries;
  DenseSet<const Function *> Seen;
  auto Add = [&](Function *F, StringRef Name) {
    if (Seen.insert(F).second)
      Entries.push_back({F, Name.str()});
  };

  if (Specs.empty()) {
    if (Function *F = G.getEntryFunction())
      Add(F, F->getDisplayName());
    for (Function *F : G.getDynamicFunctions())
      Add(F, F->getDisplayName());
    if (Entries.empty())
      return createStringError(
          inconvertibleErrorCode(),
          "no entry points: the file has neither an ELF entry point nor "
          "exported functions; name them with --entry-point");
    return Entries;
  }

  for (StringRef Spec : Specs) {
    if (Spec.find_first_of("*?[") != StringRef::npos) {
      Expected<GlobPattern> PatternOrErr = GlobPattern::create(Spec);
      if (!PatternOrErr)
        return PatternOrErr.takeError();
      SmallVector<std::pair<Function *, StringRef>, 8> Matches;
      for (const auto &Entry : G.getNamedFunctions())
        if (PatternOrErr->match(Entry.getKey()))
          for (Function *F : Entry.getValue())
            Matches.emplace_back(F, Entry.getKey());
      if (Matches.empty())
        return createStringError(inconvertibleErrorCode(),
                                 "no function matches '" + Spec + "'");
      llvm::sort(Matches, [](const auto &A, const auto &B) {
        return std::tie(A.first->Address, A.second) <
               std::tie(B.first->Address, B.second);
      });
      for (const auto &[F, Name] : Matches)
        Add(F, Name);
      continue;
    }

    ArrayRef<Function *> Named = G.findByName(Spec);
    if (!Named.empty()) {
      for (Function *F : Named)
        Add(F, Spec);
      continue;
    }

    uint64_t Address;
    if (Spec.getAsInteger(0, Address))
      return createStringError(inconvertibleErrorCode(),
                               "no function named '" + Spec + "'");
    Function *F = G.find(G.canonicalizeAddress(Address));
    if (!F)
      return createStringError(inconvertibleErrorCode(),
                               "no function at address " + Spec);
    Add(F, F->getDisplayName());
  }
  return Entries;
}

StackSizeEstimator::StackSizeEstimator(const StackSizeGraph &G,
                                       const EstimatorOptions &Opts)
    : G(G), Opts(Opts) {
  // scc_iterator visits every component before the components that call it,
  // which is the order the usage of a path can be summed in.
  bool HasUntypedTargets = !G.getUnknownTypeIndirectTargets().empty();
  SmallVector<unsigned, 8> CycleComponents;
  for (auto It = scc_begin(&G), End = scc_end(&G); It != End; ++It) {
    if (is_contained(*It, G.getRoot()))
      continue;
    Component C;
    C.Members.assign(It->begin(), It->end());
    llvm::sort(C.Members, byAddress);
    unsigned Index = Components.size();
    if (It.hasCycle())
      CycleComponents.push_back(Index);
    for (const Function *F : C.Members) {
      ComponentOf[F] = Index;
      C.LocalUsage = SaturatingAdd(C.LocalUsage, getFrameUsage(*F));
      if (!F->HasRecord || !F->FrameSize ||
          !F->UnresolvedIndirectTypeIds.empty() ||
          (HasUntypedTargets && !F->IndirectCalls.empty()))
        C.ReasonMembers.push_back(F);
    }

    SmallSetVector<unsigned, 4> Successors;
    for (const Function *F : C.Members)
      for (const Function *Callee : F->Callees)
        if (unsigned S = ComponentOf.lookup(Callee); S != Index)
          Successors.insert(S);
    C.Successors.assign(Successors.begin(), Successors.end());
    llvm::sort(C.Successors);

    uint64_t Deepest = 0;
    for (unsigned S : C.Successors) {
      if (!C.NextOnMaxPath || Components[S].MaxUsage > Deepest) {
        Deepest = Components[S].MaxUsage;
        C.NextOnMaxPath = S;
      }
    }
    // A call to unknown indirect targets is a path of its own, which at
    // least pushes the return address.
    if (llvm::any_of(
            C.Members,
            [&](const Function *F) { return callsUnknownTargets(*F); }) &&
        Opts.CallFrameOverhead > Deepest) {
      Deepest = Opts.CallFrameOverhead;
      C.NextOnMaxPath = std::nullopt;
    }
    C.MaxUsage = SaturatingAdd(C.LocalUsage, Deepest);
    Components.push_back(std::move(C));
  }

  // Number the cycles by their first member, now that no component moves.
  llvm::sort(CycleComponents, [&](unsigned A, unsigned B) {
    return byAddress(Components[A].Members.front(),
                     Components[B].Members.front());
  });
  for (unsigned Index : CycleComponents) {
    Components[Index].Cycle = Cycles.size();
    Cycles.push_back(Components[Index].Members);
  }

  for (const Function *F : G.functions())
    for (const Function *Callee : F->Callees)
      if (!Callee->HasRecord)
        CallersOfUninstrumented[Callee].push_back(F);
}

bool StackSizeEstimator::callsUnknownTargets(const Function &F) const {
  return !F.UnresolvedIndirectTypeIds.empty() ||
         (!F.IndirectCalls.empty() &&
          !G.getUnknownTypeIndirectTargets().empty());
}

uint64_t StackSizeEstimator::getFrameUsage(const Function &F) const {
  uint64_t Usage = SaturatingAdd(F.FrameSize.value_or(0), getRedZone(F));
  return SaturatingAdd(Usage, Opts.CallFrameOverhead);
}

bool StackSizeEstimator::isRecursive(const Function &F) const {
  return Components[ComponentOf.lookup(&F)].Cycle.has_value();
}

Estimate StackSizeEstimator::estimate(const EntryPoint &Entry) const {
  Estimate E;
  E.Entry = Entry;
  unsigned StartIndex = ComponentOf.lookup(Entry.F);
  E.MaxStackUsage = Components[StartIndex].MaxUsage;

  for (std::optional<unsigned> Index = StartIndex; Index;
       Index = Components[*Index].NextOnMaxPath) {
    const Component &C = Components[*Index];
    E.MaxStackPath.push_back({C.Members, C.Cycle});
  }

  // Everything reachable from the entry point that makes the estimate a lower
  // bound. The walk is over components, since every member of a component is
  // reachable from every other.
  BitVector Reachable(Components.size());
  SmallVector<unsigned, 64> Worklist;
  auto Visit = [&](unsigned Index) {
    if (!Reachable.test(Index)) {
      Reachable.set(Index);
      Worklist.push_back(Index);
    }
  };
  Visit(StartIndex);
  while (!Worklist.empty())
    for (unsigned S : Components[Worklist.pop_back_val()].Successors)
      Visit(S);

  bool HasUntypedTargets = !G.getUnknownTypeIndirectTargets().empty();
  for (unsigned Index : Reachable.set_bits()) {
    const Component &C = Components[Index];
    if (C.Cycle)
      E.Cycles.push_back(*C.Cycle);
    for (const Function *F : C.ReasonMembers) {
      if (!F->HasRecord) {
        UninstrumentedCallee U;
        U.Callee = F;
        for (const Function *Caller : CallersOfUninstrumented.lookup(F))
          if (Reachable.test(ComponentOf.lookup(Caller)))
            U.Callers.push_back(Caller);
        E.UninstrumentedCallees.push_back(std::move(U));
        continue;
      }
      if (!F->FrameSize)
        E.UnknownFrames.push_back(F);
      if (!F->UnresolvedIndirectTypeIds.empty())
        E.UnresolvedIndirectCallers.push_back(F);
      if (HasUntypedTargets && !F->IndirectCalls.empty())
        E.IndirectCallersWithUntypedTargets.push_back(F);
    }
  }
  llvm::sort(E.Cycles);
  llvm::sort(E.UnknownFrames, byAddress);
  llvm::sort(E.UninstrumentedCallees,
             [](const UninstrumentedCallee &A, const UninstrumentedCallee &B) {
               return byAddress(A.Callee, B.Callee);
             });
  llvm::sort(E.UnresolvedIndirectCallers, byAddress);
  llvm::sort(E.IndirectCallersWithUntypedTargets, byAddress);

  E.IsLowerBound = !E.Cycles.empty() || !E.UnknownFrames.empty() ||
                   !E.UninstrumentedCallees.empty() ||
                   !E.UnresolvedIndirectCallers.empty() ||
                   !E.IndirectCallersWithUntypedTargets.empty();
  return E;
}
