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
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Driver.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
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

void reportError(const Twine &Msg) {
  WithColor::error(errs(), ToolName) << Msg << "\n";
}

void reportWarning(const Twine &Msg) {
  WithColor::warning(errs(), ToolName) << Msg << "\n";
}

std::string describe(const Function &F) {
  return F.getDisplayName() + " (0x" + utohexstr(F.Address, true) + ")";
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

void printFrame(raw_ostream &OS, const StackSizeEstimator &Estimator,
                const Function &F) {
  OS << "    " << describe(F) << ": ";
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
               ArrayRef<PathStep> Path) {
  for (const PathStep &Step : Path) {
    for (const Function *F : Step.Functions.take_front(MaxListed))
      printFrame(OS, Estimator, *F);
    if (Step.Functions.size() <= MaxListed)
      continue;
    uint64_t More = 0;
    for (const Function *F : Step.Functions.drop_front(MaxListed))
      More = SaturatingAdd(More, Estimator.getFrameUsage(*F));
    OS << "    and " << Step.Functions.size() - MaxListed
       << " more functions of this cycle with " << More << " more bytes\n";
  }
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
  OS << "Entry point: " << E.Entry.Name << " (0x"
     << utohexstr(E.Entry.F->Address, true) << ")\n";
  OS << "  Stack usage: ";
  if (E.IsLowerBound)
    OS << "at least ";
  OS << E.MaxStackUsage << " bytes";
  if (E.IsLowerBound)
    OS << " (lower bound)";
  OS << "\n";

  if (E.IsLowerBound) {
    OS << "  Lower bound because:\n";
    printLowerBoundReasons(OS, Estimator, E, "    ");
  }

  OS << "  Max stack path:\n";
  printPath(OS, Estimator, E.MaxStackPath);
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
  std::optional<uint64_t> CallFrameOverhead, RedZoneSize;
  if (!ParseBytes(OPT_call_frame_overhead_EQ, CallFrameOverhead) ||
      !ParseBytes(OPT_red_zone_EQ, RedZoneSize))
    return 1;

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

  Expected<StackSizeGraph> GraphOrErr =
      StackSizeGraph::create(*Obj, [&](const Twine &Msg) {
        reportWarning("'" + InputPath + "': " + Msg);
      });
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

  StackSizeEstimator Estimator(G, Opts);
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
    *OS << "\n";
    printEstimate(*OS, Estimator, Estimator.estimate(Entry));
  }

  if (Out)
    Out->keep();
  return 0;
}
