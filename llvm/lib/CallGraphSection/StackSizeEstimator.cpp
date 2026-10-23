//===- StackSizeEstimator.cpp - Whole-program stack usage estimation ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/StackSizeEstimator.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/GlobPattern.h"
#include <algorithm>
#include <deque>
#include <map>

using namespace llvm;
using namespace llvm::callgraph;

namespace {

static std::string formatHexAddress(uint64_t Addr) {
  return "0x" + utohexstr(Addr, /*LowerCase=*/true);
}

/// Return true if \p Sub is a subset of \p Super.
static bool isSubset(ArrayRef<std::string> Sub, ArrayRef<std::string> Super) {
  for (const std::string &Item : Sub) {
    if (!llvm::is_contained(Super, Item))
      return false;
  }
  return true;
}

/// Append \p Items to \p Dest without duplicates, preserving first-seen order.
static void appendUnique(SmallVectorImpl<std::string> &Dest,
                         ArrayRef<std::string> Items) {
  for (const std::string &Item : Items) {
    if (!llvm::is_contained(Dest, Item))
      Dest.push_back(Item);
  }
}

/// Prune dominated symbolic stack terms: a term (c1, U1) is dominated by
/// (c2, U2) when c2 >= c1 and U1 is a subset of U2.
static SmallVector<StackUsageTerm, 2>
pruneStackTerms(ArrayRef<StackUsageTerm> RawTerms) {
  SmallVector<StackUsageTerm, 4> Unique;
  for (const StackUsageTerm &T : RawTerms) {
    if (!llvm::is_contained(Unique, T))
      Unique.push_back(T);
  }

  SmallVector<StackUsageTerm, 2> Kept;
  for (size_t I = 0; I < Unique.size(); ++I) {
    bool Dominated = false;
    for (size_t J = 0; J < Unique.size(); ++J) {
      if (I == J)
        continue;
      const StackUsageTerm &A = Unique[I];
      const StackUsageTerm &B = Unique[J];
      if (B.ConstBytes >= A.ConstBytes && isSubset(A.Unknowns, B.Unknowns)) {
        if (B.ConstBytes > A.ConstBytes ||
            B.Unknowns.size() > A.Unknowns.size() || J < I) {
          Dominated = true;
          break;
        }
      }
    }
    if (!Dominated)
      Kept.push_back(Unique[I]);
  }

  llvm::sort(Kept, [](const StackUsageTerm &A, const StackUsageTerm &B) {
    if (A.ConstBytes != B.ConstBytes)
      return A.ConstBytes > B.ConstBytes;
    if (A.Unknowns.size() != B.Unknowns.size())
      return A.Unknowns.size() < B.Unknowns.size();
    return A.Unknowns < B.Unknowns;
  });
  return Kept;
}

/// Collect the deduplicated outgoing instrumented callee addresses of \p F.
static SmallVector<uint64_t, 8> getInstrumentedCallees(const ELFCallGraph &CG,
                                                       const ELFFunctionInfo &F,
                                                       bool &HasSelfLoop) {
  SmallVector<uint64_t, 8> Callees;
  DenseSet<uint64_t> Seen;
  HasSelfLoop = false;

  auto AddCallee = [&](uint64_t TargetAddr) {
    if (TargetAddr == F.Address)
      HasSelfLoop = true;
    if (CG.findFunction(TargetAddr) && Seen.insert(TargetAddr).second)
      Callees.push_back(TargetAddr);
  };

  for (uint64_t Callee : F.DirectCallees)
    AddCallee(Callee);
  for (const ELFIndirectCallGroup &Site : F.IndirectCalls)
    for (uint64_t Target : Site.TargetAddresses)
      AddCallee(Target);

  return Callees;
}

/// Build the SCC condensation DAG via an iterative Tarjan's algorithm and
/// compute per-SCC stack usage and symbolic terms in reverse topological order.
static void buildSCCsAndDP(const ELFCallGraph &CG,
                           StackEstimateReport &Report) {
  ArrayRef<uint64_t> FuncAddrs = CG.getFunctionAddresses();
  const size_t N = FuncAddrs.size();
  if (N == 0)
    return;

  DenseMap<uint64_t, unsigned> AddrToIdx;
  AddrToIdx.reserve(N);
  for (unsigned I = 0; I < N; ++I)
    AddrToIdx[FuncAddrs[I]] = I;

  SmallVector<SmallVector<unsigned, 4>, 0> Adj(N);
  SmallVector<bool, 0> SelfLoop(N, false);
  for (unsigned I = 0; I < N; ++I) {
    const ELFFunctionInfo *F = CG.findFunction(FuncAddrs[I]);
    assert(F && "Function address must exist in ELFCallGraph");
    bool HasSelf = false;
    SmallVector<uint64_t, 8> Callees = getInstrumentedCallees(CG, *F, HasSelf);
    SelfLoop[I] = HasSelf;
    for (uint64_t CalleeAddr : Callees) {
      auto It = AddrToIdx.find(CalleeAddr);
      if (It != AddrToIdx.end())
        Adj[I].push_back(It->second);
    }
  }

  // Iterative Tarjan's strongly connected components algorithm.
  constexpr unsigned Unvisited = ~0u;
  SmallVector<unsigned, 0> Index(N, Unvisited);
  SmallVector<unsigned, 0> Lowlink(N, Unvisited);
  SmallVector<bool, 0> OnStack(N, false);
  SmallVector<unsigned, 0> TarjanStack;
  unsigned NextIndex = 0;

  struct WorkFrame {
    unsigned U;
    unsigned EdgeIdx;
  };
  SmallVector<WorkFrame, 0> WorkStack;
  SmallVector<SmallVector<uint64_t, 1>, 0> ReverseTopologicalSCCs;

  for (unsigned Start = 0; Start < N; ++Start) {
    if (Index[Start] != Unvisited)
      continue;

    Index[Start] = Lowlink[Start] = NextIndex++;
    TarjanStack.push_back(Start);
    OnStack[Start] = true;
    WorkStack.push_back({Start, 0});

    while (!WorkStack.empty()) {
      WorkFrame &Top = WorkStack.back();
      unsigned U = Top.U;
      if (Top.EdgeIdx < Adj[U].size()) {
        unsigned V = Adj[U][Top.EdgeIdx++];
        if (Index[V] == Unvisited) {
          Index[V] = Lowlink[V] = NextIndex++;
          TarjanStack.push_back(V);
          OnStack[V] = true;
          WorkStack.push_back({V, 0});
        } else if (OnStack[V]) {
          Lowlink[U] = std::min(Lowlink[U], Index[V]);
        }
      } else {
        if (Lowlink[U] == Index[U]) {
          SmallVector<uint64_t, 1> Members;
          while (true) {
            unsigned W = TarjanStack.pop_back_val();
            OnStack[W] = false;
            Members.push_back(FuncAddrs[W]);
            if (W == U)
              break;
          }
          // Sort members by address for deterministic representation.
          llvm::sort(Members);
          ReverseTopologicalSCCs.push_back(std::move(Members));
        }
        WorkStack.pop_back();
        if (!WorkStack.empty()) {
          unsigned Parent = WorkStack.back().U;
          Lowlink[Parent] = std::min(Lowlink[Parent], Lowlink[U]);
        }
      }
    }
  }

  // Tarjan's algorithm emits SCCs in reverse topological order (sinks first,
  // sources last). Reverse so that SCC 0..M-1 is in topological order (callers
  // before callees).
  const unsigned NumSCCs = ReverseTopologicalSCCs.size();
  Report.SCCs.resize(NumSCCs);
  for (unsigned I = 0; I < NumSCCs; ++I) {
    SCCNode &Node = Report.SCCs[I];
    Node.Id = I;
    Node.MemberAddresses = std::move(ReverseTopologicalSCCs[NumSCCs - 1 - I]);
    for (uint64_t Addr : Node.MemberAddresses)
      Report.AddressToSCC[Addr] = I;
  }

  // Populate SCC edges, cycle status, and local stack usage.
  for (unsigned I = 0; I < NumSCCs; ++I) {
    SCCNode &Node = Report.SCCs[I];
    if (Node.MemberAddresses.size() > 1) {
      Node.IsCycle = true;
    } else if (Node.MemberAddresses.size() == 1) {
      unsigned FuncIdx = AddrToIdx.lookup(Node.MemberAddresses[0]);
      Node.IsCycle = SelfLoop[FuncIdx];
    }

    DenseSet<unsigned> SeenSucc;
    uint64_t LocalSum = 0;
    for (uint64_t Addr : Node.MemberAddresses) {
      const ELFFunctionInfo *F = CG.findFunction(Addr);
      assert(F && "SCC member must exist in ELFCallGraph");
      LocalSum += F->StackSize.value_or(0) + CG.getCallFrameOverhead();

      unsigned FuncIdx = AddrToIdx.lookup(Addr);
      for (unsigned CalleeFuncIdx : Adj[FuncIdx]) {
        uint64_t CalleeAddr = FuncAddrs[CalleeFuncIdx];
        unsigned SuccSCC = Report.AddressToSCC.lookup(CalleeAddr);
        if (SuccSCC != I && SeenSucc.insert(SuccSCC).second)
          Node.SuccessorSCCs.push_back(SuccSCC);
      }
    }
    llvm::sort(Node.SuccessorSCCs);
    Node.LocalStackUsage = LocalSum;
  }

  // Bottom-up dynamic programming in reverse topological order.
  for (unsigned Rev = 0; Rev < NumSCCs; ++Rev) {
    unsigned I = NumSCCs - 1 - Rev;
    SCCNode &Node = Report.SCCs[I];

    uint64_t BestSuccUsage = 0;
    std::optional<unsigned> BestSucc;
    for (unsigned SuccId : Node.SuccessorSCCs) {
      const SCCNode &Succ = Report.SCCs[SuccId];
      if (!BestSucc.has_value() || Succ.MaxStackUsage > BestSuccUsage) {
        BestSuccUsage = Succ.MaxStackUsage;
        BestSucc = SuccId;
      }
    }
    Node.MaxStackUsage = Node.LocalStackUsage + BestSuccUsage;
    Node.NextSCCOnMaxPath = BestSucc;

    // Build local symbolic unknowns for this SCC.
    SmallVector<std::string, 2> LocalUnknowns;
    if (Node.IsCycle) {
      SmallVector<std::string, 4> CycleNames;
      for (uint64_t Addr : Node.MemberAddresses)
        CycleNames.push_back(CG.findFunction(Addr)->PrimaryName);
      llvm::sort(CycleNames);
      LocalUnknowns.push_back("cycle(" + llvm::join(CycleNames, " -> ") + ")");
    }

    for (uint64_t Addr : Node.MemberAddresses) {
      const ELFFunctionInfo *F = CG.findFunction(Addr);
      if (!F->StackSize.has_value())
        appendUnique(LocalUnknowns, {"frame(" + F->PrimaryName + ")"});
      for (uint64_t UninstAddr : F->UninstrumentedCallees) {
        std::string TargetName = CG.getUninstrumentedCalleeName(UninstAddr);
        appendUnique(LocalUnknowns, {"calls(" + TargetName + ")"});
      }
      if (!F->UnresolvedIndirectTypeIds.empty())
        appendUnique(LocalUnknowns, {"indirect(" + F->PrimaryName + ")"});
    }

    if (Node.SuccessorSCCs.empty()) {
      Node.StackTerms.push_back({Node.LocalStackUsage, LocalUnknowns});
    } else {
      SmallVector<StackUsageTerm, 4> CandidateTerms;
      for (unsigned SuccId : Node.SuccessorSCCs) {
        const SCCNode &Succ = Report.SCCs[SuccId];
        for (const StackUsageTerm &SuccTerm : Succ.StackTerms) {
          StackUsageTerm Combined;
          Combined.ConstBytes = Node.LocalStackUsage + SuccTerm.ConstBytes;
          Combined.Unknowns = LocalUnknowns;
          appendUnique(Combined.Unknowns, SuccTerm.Unknowns);
          CandidateTerms.push_back(std::move(Combined));
        }
      }
      Node.StackTerms = pruneStackTerms(CandidateTerms);
    }
  }
}

struct ResolvedEntrySpec {
  std::string Name;
  uint64_t Address = 0;
  bool HasCallGraph = true;
};

static Expected<SmallVector<ResolvedEntrySpec, 8>>
resolveEntryPoints(const ELFCallGraph &CG, const StackEstimateOptions &Opts) {
  SmallVector<ResolvedEntrySpec, 8> Result;
  DenseSet<uint64_t> SeenAddresses;

  auto AddByAddress = [&](StringRef Name, uint64_t Addr) {
    const ELFFunctionInfo *F = CG.findFunction(Addr);
    ArrayRef<std::string> DisplayNames = CG.getDisplayNames(Addr);
    StringRef EffectiveName =
        !Name.empty()
            ? Name
            : (F ? StringRef(F->PrimaryName)
                 : (!DisplayNames.empty() ? StringRef(DisplayNames.front())
                                          : StringRef()));
    std::string ResolvedName =
        EffectiveName.empty() ? formatHexAddress(Addr) : EffectiveName.str();
    Result.push_back({std::move(ResolvedName), Addr, F != nullptr});
  };

  if (Opts.EntryPoints.empty()) {
    if (std::optional<uint64_t> Entry = CG.getElfEntryPoint()) {
      if (SeenAddresses.insert(*Entry).second)
        AddByAddress("", *Entry);
    }
    for (uint64_t DynAddr : CG.getDynamicFunctionAddresses()) {
      if (SeenAddresses.insert(DynAddr).second)
        AddByAddress("", DynAddr);
    }
    if (Result.empty())
      return createStringError(
          inconvertibleErrorCode(),
          "no entry points specified and no ELF entry point or dynamic "
          "function symbols found");
    return Result;
  }

  // Invert CG.getDefinedFunctionSymbols() (address -> names) into
  // name -> sorted addresses.
  StringMap<SmallVector<uint64_t, 1>> SymToAddrs;
  for (const auto &KV : CG.getDefinedFunctionSymbols()) {
    uint64_t Addr = KV.first;
    for (const std::string &SymName : KV.second) {
      auto &Addrs = SymToAddrs[SymName];
      if (!llvm::is_contained(Addrs, Addr))
        Addrs.push_back(Addr);
    }
  }
  for (auto &KV : SymToAddrs)
    llvm::sort(KV.second);

  for (const std::string &Spec : Opts.EntryPoints) {
    StringRef S(Spec);
    if (S.find_first_of("*?[") != StringRef::npos) {
      Expected<GlobPattern> PatOrErr = GlobPattern::create(S);
      if (!PatOrErr)
        return PatOrErr.takeError();
      SmallVector<std::pair<std::string, uint64_t>, 8> Matched;
      for (const auto &KV : SymToAddrs) {
        if (PatOrErr->match(KV.first()) && KV.second.size() == 1)
          Matched.emplace_back(KV.first().str(), KV.second[0]);
      }
      llvm::sort(Matched, [](const auto &A, const auto &B) {
        if (A.second != B.second)
          return A.second < B.second;
        return A.first < B.first;
      });
      if (Matched.empty())
        return createStringError(inconvertibleErrorCode(),
                                 "entry point pattern matched no functions: " +
                                     Spec);
      for (const auto &[Name, Addr] : Matched)
        AddByAddress(Name, Addr);
      continue;
    }

    auto SymIt = SymToAddrs.find(S);
    if (SymIt != SymToAddrs.end()) {
      if (SymIt->second.size() > 1)
        return createStringError(
            inconvertibleErrorCode(),
            "ambiguous entry function name '" + Spec +
                "' maps to multiple addresses; specify an address");
      AddByAddress(Spec, SymIt->second[0]);
      continue;
    }

    uint64_t ParsedAddr = 0;
    if (!S.getAsInteger(0, ParsedAddr)) {
      if (CG.getEMachine() == /*EM_ARM=*/40)
        ParsedAddr &= ~uint64_t(1);
      if (CG.findFunction(ParsedAddr) ||
          CG.getDefinedFunctionSymbols().contains(ParsedAddr)) {
        AddByAddress("", ParsedAddr);
        continue;
      }
      return createStringError(inconvertibleErrorCode(),
                               "entry function address not found: " + Spec);
    }

    return createStringError(inconvertibleErrorCode(),
                             "entry function not found: " + Spec);
  }

  return Result;
}

static EntryPointEstimate
computeEntryPointEstimate(const ELFCallGraph &CG,
                          const StackEstimateReport &Report,
                          const ResolvedEntrySpec &Spec) {
  EntryPointEstimate Est;
  Est.Name = Spec.Name;
  Est.Address = Spec.Address;
  Est.HasCallGraph = Spec.HasCallGraph;

  if (!Spec.HasCallGraph) {
    Est.MaxStackUsage = 0;
    Est.StackUsageExpr = "calls(" + Spec.Name + ")";
    Est.IsLowerBound = true;
    Est.LowerBoundReasons.push_back(
        "No call graph information for entry point " + Spec.Name + " (" +
        formatHexAddress(Spec.Address) + ")");
    return Est;
  }

  unsigned StartSCC = Report.AddressToSCC.lookup(Spec.Address);
  const SCCNode &StartNode = Report.SCCs[StartSCC];
  Est.MaxStackUsage = StartNode.MaxStackUsage;
  Est.StackUsageExpr = formatStackUsageExpr(StartNode.StackTerms);

  // Reconstruct the worst-case stack call path across SCCs.
  std::optional<unsigned> CurrSCC = StartSCC;
  while (CurrSCC.has_value()) {
    const SCCNode &Node = Report.SCCs[*CurrSCC];
    for (uint64_t Addr : Node.MemberAddresses) {
      const ELFFunctionInfo *F = CG.findFunction(Addr);
      StringRef FrameName = (Addr == Spec.Address && !Spec.Name.empty())
                                ? StringRef(Spec.Name)
                                : StringRef(F->PrimaryName);
      Est.MaxStackPath.push_back({Addr, FrameName.str(), F->StackSize});
    }
    CurrSCC = Node.NextSCCOnMaxPath;
  }

  // BFS over reachable SCCs from StartSCC to gather lower-bound causes.
  DenseSet<unsigned> VisitedSCCs;
  std::deque<unsigned> Queue;
  VisitedSCCs.insert(StartSCC);
  Queue.push_back(StartSCC);

  std::map<uint64_t, SmallVector<StackFrameEntry, 2>> UninstToCallers;

  while (!Queue.empty()) {
    unsigned SCCId = Queue.front();
    Queue.pop_front();
    const SCCNode &Node = Report.SCCs[SCCId];

    if (Node.IsCycle) {
      SmallVector<StackFrameEntry, 2> CycleFrames;
      for (uint64_t Addr : Node.MemberAddresses) {
        const ELFFunctionInfo *F = CG.findFunction(Addr);
        CycleFrames.push_back({Addr, F->PrimaryName, F->StackSize});
      }
      Est.Cycles.push_back(std::move(CycleFrames));
    }

    for (uint64_t Addr : Node.MemberAddresses) {
      const ELFFunctionInfo *F = CG.findFunction(Addr);
      if (!F->StackSize.has_value())
        Est.UnknownStackSizes.push_back({Addr, F->PrimaryName, std::nullopt});

      for (uint64_t UninstAddr : F->UninstrumentedCallees) {
        UninstToCallers[UninstAddr].push_back(
            {Addr, F->PrimaryName, F->StackSize});
      }

      if (!F->UnresolvedIndirectTypeIds.empty()) {
        UnresolvedIndirectCallInfo Info;
        Info.CallerAddress = Addr;
        Info.CallerName = F->PrimaryName;
        Info.TypeIds.assign(F->UnresolvedIndirectTypeIds.begin(),
                            F->UnresolvedIndirectTypeIds.end());
        Est.UnresolvedIndirectCalls.push_back(std::move(Info));
      }
    }

    for (unsigned SuccSCC : Node.SuccessorSCCs) {
      if (VisitedSCCs.insert(SuccSCC).second)
        Queue.push_back(SuccSCC);
    }
  }

  for (auto &[TargetAddr, Callers] : UninstToCallers) {
    UninstrumentedCalleeInfo Info;
    Info.TargetAddress = TargetAddr;
    Info.TargetName = CG.getUninstrumentedCalleeName(TargetAddr);
    Info.Callers = std::move(Callers);
    Est.UninstrumentedCallees.push_back(std::move(Info));
  }

  // Populate human-readable lower-bound reasons.
  for (const auto &Cycle : Est.Cycles) {
    SmallVector<std::string, 4> Names;
    for (const StackFrameEntry &Frame : Cycle)
      Names.push_back(Frame.Name);
    Est.LowerBoundReasons.push_back("Cycle detected: " +
                                    llvm::join(Names, " -> "));
  }
  for (const StackFrameEntry &Unk : Est.UnknownStackSizes) {
    Est.LowerBoundReasons.push_back(
        "Unknown stack size for function: " + Unk.Name + " (" +
        formatHexAddress(Unk.Address) + ")");
  }
  for (const UninstrumentedCalleeInfo &Uninst : Est.UninstrumentedCallees) {
    SmallVector<std::string, 4> CallerNames;
    for (const StackFrameEntry &Caller : Uninst.Callers)
      CallerNames.push_back(Caller.Name);
    Est.LowerBoundReasons.push_back(
        "Calls uninstrumented function: " + Uninst.TargetName + " (" +
        formatHexAddress(Uninst.TargetAddress) + ") (called by " +
        llvm::join(CallerNames, ", ") + ")");
  }
  for (const UnresolvedIndirectCallInfo &Ind : Est.UnresolvedIndirectCalls) {
    SmallVector<std::string, 4> HexTypeIds;
    for (uint64_t TId : Ind.TypeIds)
      HexTypeIds.push_back(formatHexAddress(TId));
    Est.LowerBoundReasons.push_back(
        "Unresolved indirect call in function: " + Ind.CallerName + " (" +
        formatHexAddress(Ind.CallerAddress) +
        ") (type IDs: " + llvm::join(HexTypeIds, ", ") + ")");
  }

  Est.IsLowerBound = !Est.LowerBoundReasons.empty();
  return Est;
}

} // namespace

std::string
llvm::callgraph::formatStackUsageExpr(ArrayRef<StackUsageTerm> Terms) {
  if (Terms.empty())
    return "0";

  auto FormatOne = [](const StackUsageTerm &T) -> std::string {
    if (T.Unknowns.empty())
      return std::to_string(T.ConstBytes);
    std::string SymPart = llvm::join(T.Unknowns, " + ");
    if (T.ConstBytes > 0)
      return std::to_string(T.ConstBytes) + " + " + SymPart;
    return SymPart;
  };

  if (Terms.size() == 1)
    return FormatOne(Terms[0]);

  SmallVector<std::string, 4> Formatted;
  for (const StackUsageTerm &T : Terms)
    Formatted.push_back(FormatOne(T));
  return "max(" + llvm::join(Formatted, ", ") + ")";
}

Expected<StackEstimateReport>
llvm::callgraph::estimateStackSizes(const ELFCallGraph &CG,
                                    const StackEstimateOptions &Opts,
                                    StringRef BinaryPath) {
  StackEstimateReport Report;
  Report.BinaryPath = BinaryPath.str();
  Report.CallFrameOverhead = CG.getCallFrameOverhead();
  Report.RedZoneSize = CG.getRedZoneSize();
  Report.SymbolizerMarkupContext.assign(CG.getSymbolizerMarkupContext().begin(),
                                        CG.getSymbolizerMarkupContext().end());

  for (uint64_t Addr : CG.getUnknownTypeIndirectTargets()) {
    const ELFFunctionInfo *F = CG.findFunction(Addr);
    std::string Name = F ? F->PrimaryName : formatHexAddress(Addr);
    std::optional<uint64_t> Stack =
        F ? F->StackSize : std::optional<uint64_t>();
    Report.UnknownTypeIndirectTargets.push_back({Addr, std::move(Name), Stack});
  }

  buildSCCsAndDP(CG, Report);

  Expected<SmallVector<ResolvedEntrySpec, 8>> ResolvedOrErr =
      resolveEntryPoints(CG, Opts);
  if (!ResolvedOrErr)
    return ResolvedOrErr.takeError();

  for (const ResolvedEntrySpec &Spec : *ResolvedOrErr)
    Report.EntryPoints.push_back(computeEntryPointEstimate(CG, Report, Spec));

  return Report;
}

json::Value llvm::callgraph::toJSON(const StackEstimateReport &Report) {
  json::Object Root;
  Root["binary"] = Report.BinaryPath;
  Root["call_frame_overhead"] = Report.CallFrameOverhead;
  Root["red_zone_size"] = Report.RedZoneSize;

  json::Array UnknownTyped;
  for (const StackFrameEntry &Target : Report.UnknownTypeIndirectTargets) {
    json::Object Obj;
    Obj["address"] = formatHexAddress(Target.Address);
    Obj["name"] = Target.Name;
    UnknownTyped.push_back(std::move(Obj));
  }
  Root["unknown_type_indirect_targets"] = std::move(UnknownTyped);

  json::Array Entries;
  for (const EntryPointEstimate &EP : Report.EntryPoints) {
    json::Object EPObj;
    EPObj["entry_point"] = EP.Name;
    EPObj["address"] = formatHexAddress(EP.Address);
    EPObj["has_call_graph"] = EP.HasCallGraph;
    EPObj["max_stack_usage_bytes"] = EP.MaxStackUsage;
    EPObj["stack_usage_expr"] = EP.StackUsageExpr;
    EPObj["is_lower_bound"] = EP.IsLowerBound;

    json::Array PathArr;
    for (const StackFrameEntry &Frame : EP.MaxStackPath) {
      json::Object FObj;
      FObj["address"] = formatHexAddress(Frame.Address);
      FObj["name"] = Frame.Name;
      if (Frame.StackSize.has_value())
        FObj["stack_size"] = *Frame.StackSize;
      else
        FObj["stack_size"] = nullptr;
      PathArr.push_back(std::move(FObj));
    }
    EPObj["max_stack_path"] = std::move(PathArr);

    json::Array CyclesArr;
    for (const auto &Cycle : EP.Cycles) {
      json::Array CArr;
      for (const StackFrameEntry &Frame : Cycle) {
        json::Object FObj;
        FObj["address"] = formatHexAddress(Frame.Address);
        FObj["name"] = Frame.Name;
        CArr.push_back(std::move(FObj));
      }
      CyclesArr.push_back(std::move(CArr));
    }
    EPObj["cycles"] = std::move(CyclesArr);

    json::Array UnknownSizesArr;
    for (const StackFrameEntry &Unk : EP.UnknownStackSizes) {
      json::Object UObj;
      UObj["address"] = formatHexAddress(Unk.Address);
      UObj["name"] = Unk.Name;
      UnknownSizesArr.push_back(std::move(UObj));
    }
    EPObj["unknown_stack_sizes"] = std::move(UnknownSizesArr);

    json::Array UninstArr;
    for (const UninstrumentedCalleeInfo &Uninst : EP.UninstrumentedCallees) {
      json::Object UObj;
      UObj["address"] = formatHexAddress(Uninst.TargetAddress);
      UObj["name"] = Uninst.TargetName;
      json::Array CallersArr;
      for (const StackFrameEntry &Caller : Uninst.Callers) {
        json::Object CObj;
        CObj["address"] = formatHexAddress(Caller.Address);
        CObj["name"] = Caller.Name;
        CallersArr.push_back(std::move(CObj));
      }
      UObj["callers"] = std::move(CallersArr);
      UninstArr.push_back(std::move(UObj));
    }
    EPObj["uninstrumented_callees"] = std::move(UninstArr);

    json::Array UnresolvedArr;
    for (const UnresolvedIndirectCallInfo &Ind : EP.UnresolvedIndirectCalls) {
      json::Object IObj;
      IObj["caller_address"] = formatHexAddress(Ind.CallerAddress);
      IObj["caller_name"] = Ind.CallerName;
      json::Array TypeIdsArr;
      for (uint64_t TId : Ind.TypeIds)
        TypeIdsArr.push_back(formatHexAddress(TId));
      IObj["type_ids"] = std::move(TypeIdsArr);
      UnresolvedArr.push_back(std::move(IObj));
    }
    EPObj["unresolved_indirect_calls"] = std::move(UnresolvedArr);

    json::Array ReasonsArr;
    for (const std::string &Reason : EP.LowerBoundReasons)
      ReasonsArr.push_back(Reason);
    EPObj["lower_bound_reasons"] = std::move(ReasonsArr);

    Entries.push_back(std::move(EPObj));
  }
  Root["entry_points"] = std::move(Entries);

  return json::Value(std::move(Root));
}

void llvm::callgraph::formatTextReport(const StackEstimateReport &Report,
                                       raw_ostream &OS) {
  for (const std::string &MarkupLine : Report.SymbolizerMarkupContext)
    OS << MarkupLine << "\n";

  if (!Report.BinaryPath.empty())
    OS << "Binary: " << Report.BinaryPath << "\n";
  if (Report.CallFrameOverhead > 0)
    OS << "Call Frame Overhead: " << Report.CallFrameOverhead
       << " bytes per call\n";
  if (Report.RedZoneSize > 0)
    OS << "Leaf Red Zone Size: " << Report.RedZoneSize << " bytes\n";

  if (!Report.UnknownTypeIndirectTargets.empty()) {
    OS << "Indirect targets with unknown type ID (0):\n";
    for (const StackFrameEntry &Target : Report.UnknownTypeIndirectTargets) {
      OS << "  " << Target.Name << " ("
         << formatSymbolizerMarkupPC(Target.Address) << ")\n";
    }
  }

  for (const EntryPointEstimate &EP : Report.EntryPoints) {
    OS << "\nEntry Point: " << EP.Name << " (" << formatHexAddress(EP.Address)
       << ")\n";
    OS << "  Stack Usage: " << EP.StackUsageExpr;
    if (EP.StackUsageExpr == std::to_string(EP.MaxStackUsage))
      OS << " bytes";
    else
      OS << " (known constant: " << EP.MaxStackUsage << " bytes)";
    if (EP.IsLowerBound)
      OS << " [LOWER BOUND]";
    OS << "\n";

    if (!EP.MaxStackPath.empty()) {
      OS << "  Max Stack Path:\n";
      for (const StackFrameEntry &Frame : EP.MaxStackPath) {
        OS << "    " << Frame.Name << " ("
           << formatSymbolizerMarkupPC(Frame.Address) << "): ";
        if (Frame.StackSize.has_value())
          OS << *Frame.StackSize << " bytes";
        else
          OS << "unknown";
        if (Report.CallFrameOverhead > 0)
          OS << " (+ " << Report.CallFrameOverhead << " bytes call overhead)";
        OS << "\n";
      }
    }

    if (!EP.LowerBoundReasons.empty()) {
      OS << "  Lower Bound Reasons:\n";
      for (const std::string &Reason : EP.LowerBoundReasons)
        OS << "    - " << Reason << "\n";
    }
    if (!EP.UnresolvedIndirectCalls.empty()) {
      OS << "  Unresolved Indirect Call Sites:\n";
      for (const UnresolvedIndirectCallInfo &Ind : EP.UnresolvedIndirectCalls) {
        OS << "    " << Ind.CallerName << " ("
           << formatSymbolizerMarkupPC(Ind.CallerAddress) << "): ";
        SmallVector<std::string, 4> HexIds;
        for (uint64_t TId : Ind.TypeIds)
          HexIds.push_back(formatHexAddress(TId));
        OS << llvm::join(HexIds, ", ") << "\n";
      }
    }
  }
}
