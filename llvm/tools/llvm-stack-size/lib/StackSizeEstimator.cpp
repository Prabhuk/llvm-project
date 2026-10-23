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
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/GlobPattern.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>

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

  // The expressions name the cycles, so they come after the numbering. The
  // component order still puts every component before its callers.
  for (Component &C : Components)
    computeTerms(C);

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

std::string
stacksize::formatStackUsageExpression(ArrayRef<StackUsageTerm> Terms,
                                      ArrayRef<std::string> Names) {
  // The text orders the unknowns of a term, and the terms of one constant,
  // by name, so that it does not depend on the numbering of the unknowns.
  struct NamedTerm {
    uint64_t Constant;
    SmallVector<StringRef, 4> Unknowns;
  };
  SmallVector<NamedTerm, 4> Named;
  for (const StackUsageTerm &T : Terms) {
    NamedTerm N{T.Constant, {}};
    for (unsigned Id : T.Unknowns)
      N.Unknowns.push_back(Names[Id]);
    llvm::sort(N.Unknowns);
    Named.push_back(std::move(N));
  }
  llvm::stable_sort(Named, [](const NamedTerm &A, const NamedTerm &B) {
    if (A.Constant != B.Constant)
      return A.Constant > B.Constant;
    return A.Unknowns < B.Unknowns;
  });

  SmallVector<std::string, 4> Formatted;
  for (const NamedTerm &T : Named) {
    SmallVector<StringRef, 4> Parts;
    std::string Constant = utostr(T.Constant);
    if (T.Constant != 0 || T.Unknowns.empty())
      Parts.push_back(Constant);
    Parts.append(T.Unknowns);
    Formatted.push_back(join(Parts, " + "));
  }
  if (Formatted.size() == 1)
    return Formatted.front();
  return "max(" + join(Formatted, ", ") + ")";
}

/// At most this many members are named in cycle(...).
static constexpr size_t MaxNamedCycleMembers = 3;

unsigned StackSizeEstimator::internUnknown(const Unknown &U) {
  uint64_t Key;
  switch (U.K) {
  case Unknown::Kind::Frame:
  case Unknown::Kind::Calls:
    Key = U.F->Address;
    break;
  case Unknown::Kind::Cycle:
    Key = U.Cycle;
    break;
  case Unknown::Kind::Indirect:
    Key = U.TypeId;
    break;
  }
  auto [It, Inserted] =
      UnknownIds.try_emplace({unsigned(U.K), Key}, Unknowns.size());
  if (!Inserted)
    return It->second;

  std::string Name;
  switch (U.K) {
  case Unknown::Kind::Frame:
    Name = "frame(" + U.F->getDisplayName() + ")";
    break;
  case Unknown::Kind::Calls:
    Name = "calls(" + U.F->getDisplayName() + ")";
    break;
  case Unknown::Kind::Cycle: {
    SmallVector<std::string, 4> Members;
    for (const Function *F : Cycles[U.Cycle])
      Members.push_back(F->getDisplayName());
    llvm::sort(Members);
    Name = "cycle(" +
           join(ArrayRef(Members).take_front(MaxNamedCycleMembers), ", ");
    if (Members.size() > MaxNamedCycleMembers)
      Name += " and " + utostr(Members.size() - MaxNamedCycleMembers) + " more";
    Name += ")";
    break;
  }
  case Unknown::Kind::Indirect:
    Name = U.TypeId == 0 ? "indirect(unknown type)"
                         : "indirect(type 0x" + utohexstr(U.TypeId, true) + ")";
    break;
  }
  Unknowns.push_back(U);
  UnknownNames.push_back(std::move(Name));
  return Unknowns.size() - 1;
}

/// Returns the sorted union of two sorted, unique lists.
static SmallVector<unsigned, 2> unite(ArrayRef<unsigned> A,
                                      ArrayRef<unsigned> B) {
  SmallVector<unsigned, 2> Union;
  Union.reserve(A.size() + B.size());
  std::set_union(A.begin(), A.end(), B.begin(), B.end(),
                 std::back_inserter(Union));
  return Union;
}

/// The terms kept per component. The incomparable paths of a large binary
/// can run to tens of thousands per function, and an expression that long
/// tells a reader nothing, so past this many terms the rest are merged into
/// one that bounds them.
static constexpr size_t MaxTermsPerComponent = 32;

void StackSizeEstimator::computeTerms(Component &C) {
  // The unknowns of the component itself apply to every path through it.
  SmallVector<unsigned, 2> Local;
  if (C.Cycle)
    Local.push_back(internUnknown({Unknown::Kind::Cycle, nullptr, *C.Cycle}));
  for (const Function *F : C.Members) {
    if (!F->HasRecord)
      Local.push_back(internUnknown({Unknown::Kind::Calls, F}));
    else if (!F->FrameSize)
      Local.push_back(internUnknown({Unknown::Kind::Frame, F}));
  }
  llvm::sort(Local);
  Local.erase(llvm::unique(Local), Local.end());

  // A path either ends in this component, continues into a component it
  // calls, or continues into the unknown targets of an indirect call. The
  // unknown targets are named by their type, so that every call of a type
  // reaches the same unknown.
  SmallVector<StackUsageTerm, 4> Candidates;
  auto AddCandidate = [&](uint64_t Constant, ArrayRef<unsigned> Unknowns) {
    StackUsageTerm T;
    T.Constant = SaturatingAdd(C.LocalUsage, Constant);
    T.Unknowns = unite(Local, Unknowns);
    Candidates.push_back(std::move(T));
  };
  AddCandidate(0, {});
  for (unsigned S : C.Successors)
    for (const StackUsageTerm &T : Components[S].Expression.Terms)
      AddCandidate(T.Constant, T.Unknowns);
  bool HasUntypedTargets = !G.getUnknownTypeIndirectTargets().empty();
  for (const Function *F : C.Members) {
    for (uint64_t TypeId : F->UnresolvedIndirectTypeIds)
      AddCandidate(
          Opts.CallFrameOverhead,
          internUnknown({Unknown::Kind::Indirect, nullptr, 0, TypeId}));
    if (HasUntypedTargets && !F->IndirectCalls.empty())
      AddCandidate(Opts.CallFrameOverhead,
                   internUnknown({Unknown::Kind::Indirect, nullptr, 0, 0}));
  }

  // Keep the largest constant for each set of unknowns.
  DenseMap<hash_code, unsigned> ByUnknowns;
  SmallVector<StackUsageTerm, 4> Unique;
  for (StackUsageTerm &T : Candidates) {
    hash_code H = hash_combine_range(T.Unknowns.begin(), T.Unknowns.end());
    auto [It, Inserted] = ByUnknowns.try_emplace(H, Unique.size());
    if (Inserted || Unique[It->second].Unknowns != T.Unknowns) {
      // A new set, or a hash collision between different sets, which the
      // dominance pass below still tells apart.
      Unique.push_back(std::move(T));
      continue;
    }
    StackUsageTerm &Same = Unique[It->second];
    Same.Constant = std::max(Same.Constant, T.Constant);
  }

  // Drop every term that another term dominates: for non-negative unknowns,
  // (c1, U1) is at least (c2, U2) whenever c1 >= c2 and U2 is a subset of U1.
  // In order of decreasing constant, and of decreasing size among equal
  // constants, a dominating term always comes first, and dominance is
  // transitive, so a term only needs to be checked against the terms kept
  // before it.
  llvm::stable_sort(Unique,
                    [](const StackUsageTerm &A, const StackUsageTerm &B) {
                      if (A.Constant != B.Constant)
                        return A.Constant > B.Constant;
                      return A.Unknowns.size() > B.Unknowns.size();
                    });
  auto Dominates = [](const StackUsageTerm &A, const StackUsageTerm &B) {
    return A.Constant >= B.Constant &&
           std::includes(A.Unknowns.begin(), A.Unknowns.end(),
                         B.Unknowns.begin(), B.Unknowns.end());
  };
  SmallVectorImpl<StackUsageTerm> &Terms = C.Expression.Terms;
  for (unsigned S : C.Successors)
    C.Expression.Coarsened |= Components[S].Expression.Coarsened;
  std::optional<StackUsageTerm> Bound;
  for (StackUsageTerm &T : Unique) {
    if (llvm::any_of(Terms, [&](const StackUsageTerm &Kept) {
          return Dominates(Kept, T);
        }))
      continue;
    if (Terms.size() < MaxTermsPerComponent) {
      Terms.push_back(std::move(T));
      continue;
    }
    // Past the limit, the remaining terms, whose constants are no larger,
    // are merged into one term that dominates them all: the largest of their
    // constants, which is the first one's, with all of their unknowns.
    if (!Bound)
      Bound = std::move(T);
    else
      Bound->Unknowns = unite(Bound->Unknowns, T.Unknowns);
  }
  if (Bound) {
    C.Expression.Coarsened = true;
    Terms.push_back(std::move(*Bound));
  }
  llvm::sort(Terms, [&](const StackUsageTerm &A, const StackUsageTerm &B) {
    if (A.Constant != B.Constant)
      return A.Constant > B.Constant;
    return A.Unknowns < B.Unknowns;
  });
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
  E.Expression = Components[StartIndex].Expression;

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
