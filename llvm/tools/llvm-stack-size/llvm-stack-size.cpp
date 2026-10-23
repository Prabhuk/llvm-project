//===- llvm-stack-size.cpp - Whole-program ELF stack usage estimator ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reconstructs a whole-program call graph and per-function stack frame sizes
// from `.llvm.callgraph` and `.stack_sizes` sections in a linked ELF binary,
// condenses strongly connected components, and estimates worst-case stack usage
// and lower-bound conditions for entry points.
//
//===----------------------------------------------------------------------===//

#include "llvm/CallGraphSection/ELFCallGraph.h"
#include "llvm/CallGraphSection/StackSizeEstimator.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Driver.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::callgraph;
using namespace llvm::object;

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
  StackSizeOptTable() : OptTable(optionTables()) {
    setGroupedShortOptions(true);
  }
};
} // namespace

static void reportError(StringRef ToolName, const Twine &Msg) {
  WithColor::error(errs(), ToolName) << Msg << "\n";
}

static void reportError(StringRef ToolName, Error Err) {
  handleAllErrors(std::move(Err), [&](const ErrorInfoBase &EI) {
    reportError(ToolName, EI.message());
  });
}

int llvm_stack_size_main(int argc, char **argv, const llvm::ToolContext &) {
  StringRef ToolName = argv[0];
  BumpPtrAllocator A;
  StringSaver Saver(A);
  StackSizeOptTable Tbl;
  bool HasArgError = false;
  opt::InputArgList Args =
      Tbl.parseArgs(argc, argv, OPT_UNKNOWN, Saver, [&](StringRef Msg) {
        reportError(ToolName, Msg);
        HasArgError = true;
      });
  if (HasArgError)
    return 1;

  if (Args.hasArg(OPT_help)) {
    Tbl.printHelp(outs(),
                  (Twine(ToolName) + " [options] <input-elf>").str().c_str(),
                  "LLVM whole-program ELF stack usage estimator");
    return 0;
  }
  if (Args.hasArg(OPT_version)) {
    outs() << ToolName << '\n';
    cl::PrintVersionMessage();
    return 0;
  }

  std::vector<std::string> PositionalInputs = Args.getAllArgValues(OPT_INPUT);
  StringRef ElfOpt = Args.getLastArgValue(OPT_elf_EQ);
  StringRef InputPath;
  if (!ElfOpt.empty() && !PositionalInputs.empty()) {
    reportError(ToolName,
                "cannot specify both --elf and a positional input file");
    return 1;
  }
  if (!ElfOpt.empty()) {
    InputPath = ElfOpt;
  } else if (PositionalInputs.size() == 1) {
    InputPath = PositionalInputs[0];
  } else if (PositionalInputs.empty()) {
    reportError(ToolName, "no input ELF file specified");
    return 1;
  } else {
    reportError(ToolName, "expected a single input ELF file");
    return 1;
  }

  StringRef OutputPath = Args.getLastArgValue(OPT_output_EQ);
  StringRef DepfilePath = Args.getLastArgValue(OPT_depfile_EQ);
  if (!DepfilePath.empty() && OutputPath.empty()) {
    reportError(ToolName, "--depfile requires --output");
    return 1;
  }

  ELFCallGraphOptions CGOpts;
  for (const std::string &Excl : Args.getAllArgValues(OPT_exclude_EQ))
    CGOpts.ExcludedFunctions.push_back(Excl);

  if (const opt::Arg *OverheadArg =
          Args.getLastArg(OPT_call_frame_overhead_EQ)) {
    uint64_t Val = 0;
    if (StringRef(OverheadArg->getValue()).getAsInteger(0, Val)) {
      reportError(ToolName, Twine("invalid --call-frame-overhead value: ") +
                                OverheadArg->getValue());
      return 1;
    }
    CGOpts.CallFrameOverhead = Val;
  }

  Expected<OwningBinary<Binary>> BinOrErr = createBinary(InputPath);
  if (!BinOrErr) {
    reportError(ToolName, BinOrErr.takeError());
    return 1;
  }
  const auto *ELFObj = dyn_cast<ELFObjectFileBase>(BinOrErr->getBinary());
  if (!ELFObj) {
    reportError(ToolName,
                Twine("'") + InputPath + "' is not an ELF object file");
    return 1;
  }

  Expected<ELFCallGraph> CGOrErr = ELFCallGraph::create(*ELFObj, CGOpts);
  if (!CGOrErr) {
    reportError(ToolName, CGOrErr.takeError());
    return 1;
  }

  StackEstimateOptions EstOpts;
  EstOpts.EntryPoints = Args.getAllArgValues(OPT_entry_point_EQ);

  Expected<StackEstimateReport> ReportOrErr =
      estimateStackSizes(*CGOrErr, EstOpts, InputPath);
  if (!ReportOrErr) {
    reportError(ToolName, ReportOrErr.takeError());
    return 1;
  }

  std::unique_ptr<ToolOutputFile> OutFile;
  raw_ostream *OS = &outs();
  if (!OutputPath.empty()) {
    std::error_code EC;
    OutFile = std::make_unique<ToolOutputFile>(OutputPath, EC,
                                               sys::fs::OF_TextWithCRLF);
    if (EC) {
      reportError(ToolName, Twine("unable to open output file '") + OutputPath +
                                "': " + EC.message());
      return 1;
    }
    OS = &OutFile->os();
  }

  if (Args.hasArg(OPT_json)) {
    json::Value J = toJSON(*ReportOrErr);
    *OS << formatv("{0:2}\n", J);
  } else {
    formatTextReport(*ReportOrErr, *OS);
  }

  if (OutFile)
    OutFile->keep();

  if (!DepfilePath.empty()) {
    std::error_code EC;
    ToolOutputFile DepFile(DepfilePath, EC, sys::fs::OF_TextWithCRLF);
    if (EC) {
      reportError(ToolName, Twine("unable to open depfile '") + DepfilePath +
                                "': " + EC.message());
      return 1;
    }
    DepFile.os() << OutputPath << ": " << InputPath << "\n";
    DepFile.keep();
  }

  return 0;
}
