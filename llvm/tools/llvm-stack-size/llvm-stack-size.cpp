//===- llvm-stack-size.cpp - Stack usage of the entry points of a file ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// llvm-stack-size estimates the worst-case stack usage of the entry points of
// a linked ELF file from its .llvm.callgraph and .stack_sizes sections.
//
//===----------------------------------------------------------------------===//

#include "lib/StackSizeEstimator.h"
#include "lib/StackSizeGraph.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Driver.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::stacksize;

namespace {
using namespace llvm::opt;
enum ID {
  OPT_INVALID = 0,
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Opts.inc"
#undef OPTION
};

#define OPTTABLE_CODE
#include "Opts.inc"

class StackSizeOptTable : public opt::OptTable {
public:
  StackSizeOptTable() : OptTable(optionTables()) {}
};

StringRef ToolName;
bool UseSymbolizerMarkup = false;

void reportError(const Twine &Msg) {
  WithColor::error(errs(), ToolName) << Msg << "\n";
}

void reportWarning(const Twine &Msg) {
  WithColor::warning(errs(), ToolName) << Msg << "\n";
}

std::string describeAddress(const Function &F) {
  std::string Address = "0x" + utohexstr(F.Address, true);
  // Address 0 stands for code outside the file, which no markup can resolve.
  if (UseSymbolizerMarkup && !F.isExternal())
    Address = "{{{pc:" + Address + "}}}";
  return Address;
}

std::string describe(const Function &F) {
  return F.getDisplayName() + " (" + describeAddress(F) + ")";
}

/// Prints the symbolizer markup context of \p Obj: its build ID as a module
/// and its loadable segments as mappings of that module, so that a markup
/// filter can symbolize the {{{pc:...}}} elements that follow.
template <class ELFT>
void printSymbolizerContext(raw_ostream &OS, const ELFObjectFile<ELFT> &Obj) {
  const ELFFile<ELFT> &EF = Obj.getELFFile();
  std::string BuildId;
  for (const typename ELFT::Shdr &Sec : cantFail(EF.sections())) {
    if (Sec.sh_type != ELF::SHT_NOTE)
      continue;
    size_t Align = std::max<size_t>(Sec.sh_addralign, 4);
    Error Err = Error::success();
    for (const typename ELFT::Note &Note : EF.notes(Sec, Err))
      if (Note.getName() == "GNU" && Note.getType() == ELF::NT_GNU_BUILD_ID)
        BuildId = toHex(Note.getDesc(Align), /*LowerCase=*/true);
    consumeError(std::move(Err));
    if (!BuildId.empty())
      break;
  }
  if (BuildId.empty())
    return;
  Expected<typename ELFT::PhdrRange> PhdrsOrErr = EF.program_headers();
  if (!PhdrsOrErr) {
    consumeError(PhdrsOrErr.takeError());
    return;
  }
  OS << "{{{reset}}}\n";
  OS << "{{{module:0:" << sys::path::filename(Obj.getFileName())
     << ":elf:" << BuildId << "}}}\n";
  for (const typename ELFT::Phdr &Phdr : *PhdrsOrErr) {
    if (Phdr.p_type != ELF::PT_LOAD)
      continue;
    std::string Flags;
    if (Phdr.p_flags & ELF::PF_R)
      Flags += 'r';
    if (Phdr.p_flags & ELF::PF_W)
      Flags += 'w';
    if (Phdr.p_flags & ELF::PF_X)
      Flags += 'x';
    OS << "{{{mmap:0x" << utohexstr(Phdr.p_vaddr, true) << ":0x"
       << utohexstr(Phdr.p_memsz, true) << ":load:0:" << Flags << ":0x"
       << utohexstr(Phdr.p_vaddr, true) << "}}}\n";
  }
}

void printSymbolizerContext(raw_ostream &OS, const ELFObjectFileBase &Obj) {
  if (const auto *ELF = dyn_cast<ELF32LEObjectFile>(&Obj))
    return printSymbolizerContext(OS, *ELF);
  if (const auto *ELF = dyn_cast<ELF32BEObjectFile>(&Obj))
    return printSymbolizerContext(OS, *ELF);
  if (const auto *ELF = dyn_cast<ELF64LEObjectFile>(&Obj))
    return printSymbolizerContext(OS, *ELF);
  printSymbolizerContext(OS, *cast<ELF64BEObjectFile>(&Obj));
}

/// The text report lists at most this many items of one kind, then says how
/// many more there are, since a large binary can have thousands of each.
constexpr size_t MaxListed = 10;

/// At most this many functions are named inline on one line.
constexpr size_t MaxNamed = 3;

/// Describes a list of functions, naming at most MaxNamed of them.
std::string describe(ArrayRef<const Function *> Fs) {
  std::string S;
  for (const Function *F : Fs.take_front(MaxNamed)) {
    if (!S.empty())
      S += ", ";
    S += describe(*F);
  }
  if (Fs.size() > MaxNamed)
    S += " and " + utostr(Fs.size() - MaxNamed) + " more";
  return S;
}

/// Prints at most MaxListed of \p Items with \p Print, then a line that says
/// how many more there are, with \p Noun naming what they are.
template <typename T, typename PrintFn>
void printListed(raw_ostream &OS, StringRef Indent, ArrayRef<T> Items,
                 StringRef Noun, PrintFn Print) {
  for (const T &Item : Items.take_front(MaxListed))
    Print(Item);
  if (Items.size() > MaxListed)
    OS << Indent << "and " << Items.size() - MaxListed << " more " << Noun
       << "\n";
}

/// Describes what \p F calls: its direct callees, the targets of its indirect
/// calls, and the type IDs of indirect calls with no known target, naming at
/// most MaxNamed of them.
std::string describeCallees(const Function &F) {
  SmallVector<std::string, 4> Parts;
  for (const Function *Callee : F.DirectCallees)
    Parts.push_back(describe(*Callee));
  for (const IndirectCallSite &Site : F.IndirectCalls)
    for (const Function *Target : Site.Targets)
      Parts.push_back(describe(*Target) + " indirectly");
  for (uint64_t TypeId : F.UnresolvedIndirectTypeIds)
    Parts.push_back("an unknown function of type ID 0x" +
                    utohexstr(TypeId, true));
  std::string S = join(ArrayRef(Parts).take_front(MaxNamed), ", ");
  if (Parts.size() > MaxNamed)
    S += " and " + utostr(Parts.size() - MaxNamed) + " more";
  return S;
}

void printFrame(raw_ostream &OS, const StackSizeEstimator &Estimator,
                const Function &F, StringRef Indent = "    ") {
  OS << Indent << describe(F) << ": ";
  if (F.FrameSize)
    OS << *F.FrameSize << " bytes";
  else
    OS << "unknown frame size";
  if (uint64_t RedZone = Estimator.getRedZone(F))
    OS << " + " << RedZone << " bytes red zone";
  if (uint64_t Overhead = Estimator.getOptions().CallFrameOverhead)
    OS << " + " << Overhead << " bytes call overhead";
  if (!F.HasRecord)
    OS << ", callees unknown";
  OS << "\n";
}

/// Prints the frames of a max stack path. A recursive component on the path
/// lists at most MaxListed of its members, then the rest as a count.
void printPath(raw_ostream &OS, const StackSizeEstimator &Estimator,
               ArrayRef<PathStep> Path, StringRef Indent = "    ") {
  for (const PathStep &Step : Path) {
    for (const Function *F : Step.Functions.take_front(MaxListed))
      printFrame(OS, Estimator, *F, Indent);
    if (Step.Functions.size() <= MaxListed)
      continue;
    uint64_t More = 0;
    for (const Function *F : Step.Functions.drop_front(MaxListed))
      More = SaturatingAdd(More, Estimator.getFrameUsage(*F));
    OS << Indent << "and " << Step.Functions.size() - MaxListed
       << " more functions of this cycle with " << More << " more bytes\n";
  }
}

/// The text report prints an expression that names at most this many
/// unknowns, counting repetitions across its terms, since a longer one is not
/// readable; beyond it the report gives the size of the expression instead.
constexpr size_t MaxPrintedUnknowns = 64;

void printExpression(raw_ostream &OS, const StackSizeEstimator &Estimator,
                     const StackUsageExpression &Expression) {
  size_t Named = 0;
  SmallDenseSet<unsigned, 16> Distinct;
  for (const StackUsageTerm &T : Expression.Terms) {
    Named += T.Unknowns.size();
    Distinct.insert_range(T.Unknowns);
  }
  if (Named <= MaxPrintedUnknowns) {
    OS << Estimator.formatExpression(Expression);
    return;
  }
  OS << Expression.Terms.size()
     << (Expression.Terms.size() == 1 ? " term over " : " terms over ")
     << Distinct.size() << " unknowns, too large to print";
}

void printLowerBoundReasons(raw_ostream &OS,
                            const StackSizeEstimator &Estimator,
                            const Estimate &E, StringRef Indent) {
  printListed(OS, Indent, ArrayRef(E.Cycles), "cycles", [&](unsigned Cycle) {
    OS << Indent << "cycle: " << describe(Estimator.cycles()[Cycle]) << "\n";
  });
  printListed(OS, Indent, ArrayRef(E.UnknownFrames),
              "functions without a frame size", [&](const Function *F) {
                OS << Indent << "unknown frame size: " << describe(*F) << "\n";
              });
  printListed(OS, Indent, ArrayRef(E.UninstrumentedCallees),
              "functions with unknown callees",
              [&](const UninstrumentedCallee &U) {
                OS << Indent << "unknown callees: " << describe(*U.Callee);
                if (!U.Callers.empty())
                  OS << ", called by " << describe(U.Callers);
                OS << "\n";
              });
  printListed(OS, Indent, ArrayRef(E.UnresolvedIndirectCallers),
              "functions with unresolved indirect calls",
              [&](const Function *F) {
                OS << Indent << "unresolved indirect call: " << describe(*F)
                   << ", type ID";
                if (F->UnresolvedIndirectTypeIds.size() > 1)
                  OS << "s";
                for (uint64_t TypeId : F->UnresolvedIndirectTypeIds)
                  OS << " 0x" << utohexstr(TypeId, true);
                OS << "\n";
              });
  printListed(OS, Indent, ArrayRef(E.IndirectCallersWithUntypedTargets),
              "functions whose indirect calls may reach a target of unknown "
              "type",
              [&](const Function *F) {
                OS << Indent
                   << "indirect call may reach a target of unknown type: "
                   << describe(*F) << "\n";
              });
}

void printEstimate(raw_ostream &OS, const StackSizeEstimator &Estimator,
                   const Estimate &E) {
  OS << "Entry point: " << E.Entry.Name << " (" << describeAddress(*E.Entry.F)
     << ")\n";
  OS << "  Stack usage: ";
  if (E.IsLowerBound)
    OS << "at least ";
  OS << E.MaxStackUsage << " bytes";
  if (E.IsLowerBound)
    OS << " (lower bound)";
  OS << "\n";

  if (E.IsLowerBound) {
    OS << "  Stack usage expression";
    if (E.Expression.Coarsened)
      OS << " (coarsened)";
    OS << ": ";
    printExpression(OS, Estimator, E.Expression);
    OS << "\n";
    OS << "  Lower bound because:\n";
    printLowerBoundReasons(OS, Estimator, E, "    ");
  }

  OS << "  Max stack path:\n";
  printPath(OS, Estimator, E.MaxStackPath);
}

/// The JSON report is streamed, one entry point at a time, so that its size
/// in memory does not grow with the number of entry points.

void writeAddresses(json::OStream &J, ArrayRef<const Function *> Fs) {
  J.array([&] {
    for (const Function *F : Fs)
      J.value(F->Address);
  });
}

/// Writes a function as its address, name and frame size, the last null when
/// it is unknown.
void writeFunction(json::OStream &J, const Function &F) {
  J.object([&] {
    J.attribute("address", F.Address);
    J.attribute("name", F.getDisplayName());
    if (F.FrameSize)
      J.attribute("stack", *F.FrameSize);
    else
      J.attribute("stack", nullptr);
  });
}

void writeUnknown(json::OStream &J, const StackSizeEstimator &Estimator,
                  unsigned Index) {
  const Unknown &U = Estimator.unknowns()[Index];
  J.object([&] {
    J.attribute("name", Estimator.unknownNames()[Index]);
    switch (U.K) {
    case Unknown::Kind::Frame:
      J.attribute("kind", "frame");
      J.attribute("address", U.F->Address);
      break;
    case Unknown::Kind::Calls:
      J.attribute("kind", "calls");
      J.attribute("address", U.F->Address);
      break;
    case Unknown::Kind::Cycle:
      J.attribute("kind", "cycle");
      J.attribute("cycle", U.Cycle);
      break;
    case Unknown::Kind::Indirect:
      J.attribute("kind", "indirect");
      J.attribute("type_id", U.TypeId);
      break;
    }
  });
}

void writeEstimate(json::OStream &J, const Estimate &E) {
  J.object([&] {
    J.attribute("entry_function", E.Entry.Name);
    J.attribute("address", E.Entry.F->Address);
    J.attribute("max_stack_usage", E.MaxStackUsage);
    J.attribute("is_lower_bound", E.IsLowerBound);
    J.attributeObject("stack_usage_expression", [&] {
      J.attributeArray("terms", [&] {
        for (const StackUsageTerm &T : E.Expression.Terms)
          J.object([&] {
            J.attribute("constant", T.Constant);
            J.attributeArray("unknowns", [&] {
              for (unsigned Id : T.Unknowns)
                J.value(Id);
            });
          });
      });
      J.attribute("coarsened", E.Expression.Coarsened);
    });
    J.attributeArray("max_stack_path", [&] {
      for (const PathStep &Step : E.MaxStackPath) {
        if (Step.Cycle)
          J.object([&] { J.attribute("cycle", *Step.Cycle); });
        else
          writeFunction(J, *Step.Functions.front());
      }
    });
    J.attributeArray("cycles", [&] {
      for (unsigned Cycle : E.Cycles)
        J.value(Cycle);
    });
    J.attributeBegin("unknown_stack_sizes");
    writeAddresses(J, E.UnknownFrames);
    J.attributeEnd();
    J.attributeArray("uninstrumented_callees", [&] {
      for (const UninstrumentedCallee &U : E.UninstrumentedCallees)
        J.object([&] {
          J.attribute("address", U.Callee->Address);
          J.attribute("name", U.Callee->getDisplayName());
          J.attributeBegin("callers");
          writeAddresses(J, U.Callers);
          J.attributeEnd();
        });
    });
    J.attributeArray("unresolved_indirect_calls", [&] {
      for (const Function *F : E.UnresolvedIndirectCallers)
        for (uint64_t TypeId : F->UnresolvedIndirectTypeIds)
          J.object([&] {
            J.attribute("caller", F->Address);
            J.attribute("type_id", TypeId);
          });
    });
    J.attributeBegin("indirect_calls_with_unknown_type_targets");
    writeAddresses(J, E.IndirectCallersWithUntypedTargets);
    J.attributeEnd();
  });
}

/// Writes the report, calling \p Observe on every estimate as it is written.
void writeReport(json::OStream &J, StringRef File, const StackSizeGraph &G,
                 const StackSizeEstimator &Estimator,
                 ArrayRef<EntryPoint> Entries,
                 function_ref<void(const Estimate &)> Observe) {
  J.object([&] {
    J.attribute("file", File);
    J.attribute("call_frame_overhead",
                Estimator.getOptions().CallFrameOverhead);
    J.attribute("red_zone_size", Estimator.getOptions().RedZoneSize);
    J.attributeBegin("unknown_type_indirect_targets");
    writeAddresses(J, G.getUnknownTypeIndirectTargets());
    J.attributeEnd();
    J.attributeArray("cycles", [&] {
      for (ArrayRef<const Function *> Cycle : Estimator.cycles())
        J.array([&] {
          for (const Function *F : Cycle)
            writeFunction(J, *F);
        });
    });
    J.attributeArray("unknowns", [&] {
      for (unsigned I = 0, N = Estimator.unknowns().size(); I != N; ++I)
        writeUnknown(J, Estimator, I);
    });
    J.attributeArray("entry_functions", [&] {
      for (const EntryPoint &Entry : Entries) {
        Estimate E = Estimator.estimate(Entry);
        writeEstimate(J, E);
        Observe(E);
      }
    });
  });
}

} // namespace

int llvm_stack_size_main(int Argc, char **Argv, const llvm::ToolContext &) {
  ToolName = Argv[0];
  BumpPtrAllocator A;
  StringSaver Saver(A);
  StackSizeOptTable Tbl;
  bool HadArgError = false;
  opt::InputArgList Args =
      Tbl.parseArgs(Argc, Argv, OPT_UNKNOWN, Saver, [&](StringRef Msg) {
        reportError(Msg);
        HadArgError = true;
      });
  if (HadArgError)
    return 1;

  if (Args.hasArg(OPT_help)) {
    Tbl.printHelp(outs(), (Twine(ToolName) + " [options] <file>").str().c_str(),
                  "Estimate the worst-case stack usage of the entry points of "
                  "a linked ELF file");
    outs() << "\nPass @FILE as argument to read options from FILE.\n";
    return 0;
  }
  if (Args.hasArg(OPT_version)) {
    cl::PrintVersionMessage();
    return 0;
  }

  std::vector<std::string> Inputs = Args.getAllArgValues(OPT_INPUT);
  if (Inputs.size() != 1) {
    reportError(Inputs.empty() ? "no input file" : "expected one input file");
    return 1;
  }
  StringRef InputPath = Inputs.front();

  auto ParseBytes = [&](ID Opt, std::optional<uint64_t> &Out) {
    const opt::Arg *Arg = Args.getLastArg(Opt);
    if (!Arg)
      return true;
    uint64_t Value;
    if (StringRef(Arg->getValue()).getAsInteger(0, Value)) {
      reportError("invalid byte count for " + Arg->getSpelling() + ": '" +
                  Arg->getValue() + "'");
      return false;
    }
    Out = Value;
    return true;
  };
  std::optional<uint64_t> CallFrameOverhead, RedZoneSize, MaxStackUsage;
  if (!ParseBytes(OPT_call_frame_overhead_EQ, CallFrameOverhead) ||
      !ParseBytes(OPT_red_zone_EQ, RedZoneSize) ||
      !ParseBytes(OPT_max_stack_usage_EQ, MaxStackUsage))
    return 1;

  // The functions that --limit and --leaf name are analyzed in addition to
  // the entry points, since a policy for a function needs its estimate.
  std::vector<std::pair<std::string, uint64_t>> Limits;
  for (StringRef Spec : Args.getAllArgValues(OPT_limit_EQ)) {
    auto [Name, Bytes] = Spec.rsplit('=');
    uint64_t Limit;
    if (Name.empty() || Bytes.getAsInteger(0, Limit)) {
      reportError("invalid --limit: '" + Spec +
                  "', expected <function>=<bytes>");
      return 1;
    }
    Limits.emplace_back(Name.str(), Limit);
  }
  std::vector<std::string> Leaves = Args.getAllArgValues(OPT_leaf_EQ);
  std::vector<std::string> PolicySpecs;
  for (const auto &[Name, Limit] : Limits)
    PolicySpecs.push_back(Name);
  llvm::append_range(PolicySpecs, Leaves);

  Expected<OwningBinary<Binary>> BinaryOrErr = createBinary(InputPath);
  if (!BinaryOrErr) {
    reportError("'" + InputPath + "': " + toString(BinaryOrErr.takeError()));
    return 1;
  }
  const auto *Obj = dyn_cast<ELFObjectFileBase>(BinaryOrErr->getBinary());
  if (!Obj) {
    reportError("'" + InputPath + "': not an ELF object file");
    return 1;
  }

  Expected<StackSizeGraph> GraphOrErr = StackSizeGraph::create(
      *Obj,
      [&](const Twine &Msg) { reportWarning("'" + InputPath + "': " + Msg); },
      Args.getAllArgValues(OPT_exclude_EQ));
  if (!GraphOrErr) {
    reportError("'" + InputPath + "': " + toString(GraphOrErr.takeError()));
    return 1;
  }
  const StackSizeGraph &G = *GraphOrErr;

  EstimatorOptions Opts;
  Opts.CallFrameOverhead =
      CallFrameOverhead.value_or(defaultCallFrameOverhead(G.getMachine()));
  Opts.RedZoneSize = RedZoneSize.value_or(defaultRedZoneSize(G.getMachine()));

  Expected<std::vector<EntryPoint>> EntriesOrErr =
      resolveEntryPoints(G, Args.getAllArgValues(OPT_entry_point_EQ));
  if (!EntriesOrErr) {
    reportError(toString(EntriesOrErr.takeError()));
    return 1;
  }
  if (!PolicySpecs.empty()) {
    Expected<std::vector<EntryPoint>> PolicyEntriesOrErr =
        resolveEntryPoints(G, PolicySpecs);
    if (!PolicyEntriesOrErr) {
      reportError(toString(PolicyEntriesOrErr.takeError()));
      return 1;
    }
    for (EntryPoint &Entry : *PolicyEntriesOrErr)
      if (llvm::none_of(*EntriesOrErr,
                        [&](const EntryPoint &E) { return E.F == Entry.F; }))
        EntriesOrErr->push_back(std::move(Entry));
  }

  // The tightest limit named for a function wins, over --max-stack-usage and
  // over limits named for its other names.
  DenseMap<const Function *, uint64_t> LimitOf;
  for (const auto &[Name, Limit] : Limits)
    for (const EntryPoint &Entry : cantFail(resolveEntryPoints(G, {Name}))) {
      auto [It, Inserted] = LimitOf.try_emplace(Entry.F, Limit);
      if (!Inserted)
        It->second = std::min(It->second, Limit);
    }
  DenseSet<const Function *> MustBeLeaf;
  for (const std::string &Name : Leaves)
    for (const EntryPoint &Entry : cantFail(resolveEntryPoints(G, {Name})))
      MustBeLeaf.insert(Entry.F);

  std::optional<ToolOutputFile> Out;
  raw_ostream *OS = &outs();
  if (const opt::Arg *Arg = Args.getLastArg(OPT_output_EQ)) {
    std::error_code EC;
    Out.emplace(Arg->getValue(), EC, sys::fs::OF_Text);
    if (EC) {
      reportError("unable to open '" + Twine(Arg->getValue()) +
                  "': " + EC.message());
      return 1;
    }
    OS = &Out->os();
  }

  UseSymbolizerMarkup = Args.hasArg(OPT_symbolizer_markup);
  StackSizeEstimator Estimator(G, Opts);

  // The policy is checked as each estimate is produced, and the errors are
  // reported after the report, so that neither is kept whole in memory.
  // A limit is a guarantee, so an estimate that is only a lower bound fails
  // it whatever its value.
  std::vector<std::string> Errors;
  auto Verify = [&](const Estimate &E) {
    const Function &F = *E.Entry.F;
    std::optional<uint64_t> Limit = MaxStackUsage;
    if (auto It = LimitOf.find(&F); It != LimitOf.end())
      Limit = It->second;
    if (Limit) {
      if (E.IsLowerBound) {
        std::string Msg;
        raw_string_ostream MS(Msg);
        MS << describe(F) << " may use more than " << *Limit
           << " bytes of stack, its estimate of " << E.MaxStackUsage
           << " bytes is only a lower bound (";
        printExpression(MS, Estimator, E.Expression);
        if (E.Expression.Coarsened)
          MS << ", coarsened";
        MS << ") because:\n";
        printLowerBoundReasons(MS, Estimator, E, "  ");
        Errors.push_back(std::move(Msg));
      } else if (E.MaxStackUsage > *Limit) {
        std::string Msg;
        raw_string_ostream MS(Msg);
        MS << describe(F) << " uses " << E.MaxStackUsage
           << " bytes of stack, more than the limit of " << *Limit
           << " bytes:\n";
        printPath(MS, Estimator, E.MaxStackPath, "  ");
        Errors.push_back(std::move(Msg));
      }
    }
    if (MustBeLeaf.contains(&F)) {
      if (!F.HasRecord)
        Errors.push_back(describe(F) + " must be a leaf function but has no "
                                       "call graph information");
      else if (F.makesCalls())
        Errors.push_back(describe(F) + " must be a leaf function but calls " +
                         describeCallees(F));
    }
  };

  if (Args.hasArg(OPT_json)) {
    json::OStream J(*OS, /*IndentSize=*/2);
    writeReport(J, InputPath, G, Estimator, *EntriesOrErr, Verify);
    *OS << "\n";
  } else {
    if (UseSymbolizerMarkup)
      printSymbolizerContext(*OS, *Obj);
    if (Opts.CallFrameOverhead || Opts.RedZoneSize)
      *OS << "Call frame overhead: " << Opts.CallFrameOverhead
          << " bytes, red zone: " << Opts.RedZoneSize << " bytes\n";
    if (!G.getUnknownTypeIndirectTargets().empty()) {
      *OS << "Indirect call targets of unknown type:\n";
      printListed(
          *OS, "  ", G.getUnknownTypeIndirectTargets(), "functions",
          [&](const Function *F) { *OS << "  " << describe(*F) << "\n"; });
    }
    for (const EntryPoint &Entry : *EntriesOrErr) {
      Estimate E = Estimator.estimate(Entry);
      *OS << "\n";
      printEstimate(*OS, Estimator, E);
      Verify(E);
    }
  }
  if (Out)
    Out->keep();

  if (Errors.empty())
    return 0;
  if (UseSymbolizerMarkup)
    printSymbolizerContext(errs(), *Obj);
  for (const std::string &Msg : Errors)
    reportError(StringRef(Msg).rtrim('\n'));
  return 1;
}
