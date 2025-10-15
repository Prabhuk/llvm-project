==== Opts.td ====
include "llvm/Option/OptParser.td"

class F<string letter, string help> : Flag<["-"], letter>, HelpText<help>;
class FF<string name, string help> : Flag<["--"], name>, HelpText<help>;

multiclass Eq<string name, string help> {
  def NAME #_EQ : Joined<["--"], name #"=">, HelpText<help>;
  def : Separate<["--"], name>, Alias<!cast<Joined>(NAME #_EQ)>;
}

def help : FF<"help", "Display this help">;
def : F<"h", "Alias for --help">, Alias<help>;
def version : FF<"version", "Display the version">;
def : F<"V", "Alias for --version">, Alias<version>;
def mergeSubcommand : Subcommand<"merge">;
defm output : Eq<"output", "Output file">, Subcommand<mergeSubcommand>;
defm  : Eq<"", "Profile kind:">, Subcommand<mergeSubcommand>;
defm max_debug_info_correlation_warnings : Eq<"max-debug-info-correlation-warnings", "The maximum number of warnings to emit when correlating profile from debug info (0 = no limit)">, Subcommand<mergeSubcommand>;
defm profiled_binary : Eq<"profiled-binary", "Path to binary from which the profile was collected.">, Subcommand<mergeSubcommand>;
defm debug_info : Eq<"debug-info", "For show, read and extract profile metadata from debug info and show the functions it found. For merge, use the provided debug info to correlate the raw profile.">, Subcommand<mergeSubcommand>;
defm binary_file : Eq<"binary-file", "For merge, use the provided unstripped binary to correlate the raw profile.">, Subcommand<mergeSubcommand>;
def debuginfod : FF<"debuginfod", "Enable debuginfod">, Subcommand<mergeSubcommand>;
defm function : Eq<"function", "Only functions matching the filter are shown in the output. For overlapping CSSPGO, this takes a function name with calling context.">, Subcommand<mergeSubcommand>;
defm  : Eq<"", "<filename...>">, Subcommand<mergeSubcommand>;
defm weighted_input : Eq<"weighted-input", "<weight>,<filename>">, Subcommand<mergeSubcommand>;
defm  : Eq<"", "Format of output profile">, Subcommand<mergeSubcommand>;
defm input_files : Eq<"input-files", "Path to file containing newline-separated [<weight>,]<filename> entries">, Subcommand<mergeSubcommand>;
def dump_input_file_list : FF<"dump-input-file-list", "Dump the list of input files and their weights, then exit">, Subcommand<mergeSubcommand>;
defm remapping_file : Eq<"remapping-file", "Symbol remapping file">, Subcommand<mergeSubcommand>;
def use_md5 : FF<"use-md5", "Choose to use MD5 to represent string in name table (only meaningful for -extbinary)">, Subcommand<mergeSubcommand>;
def compress_all_sections : FF<"compress-all-sections", "Compress all sections when writing the profile (only meaningful for -extbinary)">, Subcommand<mergeSubcommand>;
def sample_merge_cold_context : FF<"sample-merge-cold-context", "Merge context sample profiles whose count is below cold threshold">, Subcommand<mergeSubcommand>;
def sample_trim_cold_context : FF<"sample-trim-cold-context", "Trim context sample profiles whose count is below cold threshold">, Subcommand<mergeSubcommand>;
defm sample_frame_depth_for_cold_context : Eq<"sample-frame-depth-for-cold-context", "Keep the last K frames while merging cold profile. 1 means the context-less base profile">, Subcommand<mergeSubcommand>;
defm output_size_limit : Eq<"output-size-limit", "Trim cold functions until profile size is below specified limit in bytes. This uses a heursitic and functions may be excessively trimmed">, Subcommand<mergeSubcommand>;
def gen_partial_profile : FF<"gen-partial-profile", "Generate a partial profile (only meaningful for -extbinary)">, Subcommand<mergeSubcommand>;
def split_layout : FF<"split-layout", "Split the profile to two sections with one containing sample profiles with inlined functions and the other without (only meaningful for -extbinary)">, Subcommand<mergeSubcommand>;
defm supplement_instr_with_sample : Eq<"supplement-instr-with-sample", "Supplement an instr profile with sample profile, to correct the profile unrepresentativeness issue. The sample profile is the input of the flag. Output will be in instr format (The flag only works with -instr)">, Subcommand<mergeSubcommand>;
defm zero_counter_threshold : Eq<"zero-counter-threshold", "For the function which is cold in instr profile but hot in sample profile, if the ratio of the number of zero counters divided by the total number of counters is above the threshold, the profile of the function will be regarded as being harmful for performance and will be dropped.">, Subcommand<mergeSubcommand>;
defm suppl_min_size_threshold : Eq<"suppl-min-size-threshold", "If the size of a function is smaller than the threshold, assume it can be inlined by PGO early inliner and it won't be adjusted based on sample profile.">, Subcommand<mergeSubcommand>;
defm instr_prof_cold_threshold : Eq<"instr-prof-cold-threshold", "User specified cold threshold for instr profile which will override the cold threshold got from profile summary. ">, Subcommand<mergeSubcommand>;
defm temporal_profile_trace_reservoir_size : Eq<"temporal-profile-trace-reservoir-size", "The maximum number of stored temporal profile traces (default: 100)">, Subcommand<mergeSubcommand>;
defm temporal_profile_max_trace_length : Eq<"temporal-profile-max-trace-length", "The maximum length of a single temporal profile trace (default: 10000)">, Subcommand<mergeSubcommand>;
defm no_function : Eq<"no-function", "Exclude functions matching the filter from the output.">, Subcommand<mergeSubcommand>;
defm failure_mode : Eq<"failure-mode", "Failure mode:">, Subcommand<mergeSubcommand>;
def sparse : FF<"sparse", "Generate a sparse profile (only meaningful for -instr)">, Subcommand<mergeSubcommand>;
defm num_threads : Eq<"num-threads", "Number of merge threads to use (default: autodetect)">, Subcommand<mergeSubcommand>;
defm prof_sym_list : Eq<"prof-sym-list", "Path to file containing the list of function symbols used to populate profile symbol list">, Subcommand<mergeSubcommand>;
defm convert_sample_profile_layout : Eq<"convert-sample-profile-layout", "Convert the generated profile to a profile with a new layout">, Subcommand<mergeSubcommand>;
def drop_profile_symbol_list : FF<"drop-profile-symbol-list", "Drop the profile symbol list when merging AutoFDO profiles (only meaningful for -sample)">, Subcommand<mergeSubcommand>;
def keep_vtable_symbols : FF<"keep-vtable-symbols", "If true, keep the vtable symbols in indexed profiles">, Subcommand<mergeSubcommand>;
defm memprof_version : Eq<"memprof-version", "Specify the version of the memprof format to use">, Subcommand<mergeSubcommand>;
def memprof_full_schema : FF<"memprof-full-schema", "Use the full schema for serialization">, Subcommand<mergeSubcommand>;
def memprof_random_hotness : FF<"memprof-random-hotness", "Generate random hotness values">, Subcommand<mergeSubcommand>;
defm memprof_random_hotness_seed : Eq<"memprof-random-hotness-seed", "Random hotness seed to use (0 to generate new seed)">, Subcommand<mergeSubcommand>;
==== Config ====
struct DriverConfig {
  Optional<std::string> Output;
  (anonymous namespace)::ProfileKinds ;
  Optional<std::string> ;
  unsigned int MaxDebugInfoCorrelationWarnings;
  Optional<std::string> ProfiledBinary;
  Optional<std::string> DebugInfo;
  Optional<std::string> BinaryFile;
  std::vector<Optional<std::string>> DebugFileDirectory;
  bool Debuginfod = false;
  llvm::InstrProfCorrelator::ProfCorrelatorKind Correlate;
  Optional<std::string> Function;
  std::vector<Optional<std::string>> ;
  std::vector<Optional<std::string>> WeightedInput;
  (anonymous namespace)::ProfileFormat ;
  Optional<std::string> InputFiles;
  bool DumpInputFileList = false;
  Optional<std::string> RemappingFile;
  bool UseMd5 = false;
  bool CompressAllSections = false;
  bool SampleMergeColdContext = false;
  bool SampleTrimColdContext = false;
  unsigned int SampleFrameDepthForColdContext;
  unsigned long OutputSizeLimit;
  bool GenPartialProfile = false;
  bool SplitLayout = false;
  Optional<std::string> SupplementInstrWithSample;
  float ZeroCounterThreshold;
  unsigned int SupplMinSizeThreshold;
  unsigned int InstrProfColdThreshold;
  unsigned long TemporalProfileTraceReservoirSize;
  unsigned long TemporalProfileMaxTraceLength;
  Optional<std::string> NoFunction;
  (anonymous namespace)::FailureMode FailureMode;
  bool Sparse = false;
  unsigned int NumThreads;
  Optional<std::string> ProfSymList;
  llvm::sampleprof::SampleProfileLayout ConvertSampleProfileLayout;
  bool DropProfileSymbolList = false;
  bool KeepVtableSymbols = false;
  bool WritePrevVersion = false;
  llvm::memprof::IndexedVersion MemprofVersion;
  bool MemprofFullSchema = false;
  bool MemprofRandomHotness = false;
  unsigned int MemprofRandomHotnessSeed;
  Optional<std::string> ;
  Optional<std::string> ;
  unsigned long long SimilarityCutoff;
  bool Cs = false;
  unsigned long long ValueCutoff;
  bool Counts = false;
  (anonymous namespace)::ShowFormat ShowFormat;
  bool Text = false;
  bool Json = false;
  bool IcTargets = false;
  bool ShowVtables = false;
  bool MemopSizes = false;
  bool DetailedSummary = false;
  std::vector<unsigned int> ;
  bool HotFuncList = false;
  bool AllFunctions = false;
  bool Showcs = false;
  (anonymous namespace)::ProfileKinds ;
  unsigned int Topn;
  unsigned int ValueCutoff;
  bool ListBelowCutoff = false;
  bool ShowProfSymList = false;
  bool ShowSecInfoOnly = false;
  bool BinaryIds = false;
  bool TemporalProfileTraces = false;
  bool Covered = false;
  bool ProfileVersion = false;
  unsigned int NumTestTraces;
  llvm::sampleprof::FSDiscriminatorPass FsDiscriminatorPass;
};
==== Table ====
using namespace llvm::opt;
enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES, SUBCOMMANDIDS_OFFSET)                                      \
  OPT_##ID,
#include "Opts.inc"
#undef OPTION
};

#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "Opts.inc"
#undef PREFIX

#define OPTTABLE_SUBCOMMAND_IDS_TABLE_CODE
#include "Opts.inc"
#undef OPTTABLE_SUBCOMMAND_IDS_TABLE_CODE

#define OPTTABLE_SUBCOMMANDS_CODE
#include "Opts.inc"
#undef OPTTABLE_SUBCOMMANDS_CODE

static constexpr OptTable::Info InfoTable[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "Opts.inc"
#undef OPTION
};

class ${TOOL}OptTable : public opt::OptTable {
public:
  ${TOOL}OptTable() : OptTable(InfoTable) { setGroupedShortOptions(true); }
};
==== ParseArgs ====
  ${TOOL}OptTable Tbl;
  ToolName = argv[0];
  BumpPtrAllocator A;
  StringSaver Saver{A};
  opt::InputArgList Args =
      Tbl.parseArgs(argc, argv, OPT_UNKNOWN, Saver, [&](StringRef Msg) {
        fatalError(Msg);
        std::exit(1);
      });
  if (Args.hasArg(OPT_help)) {
    Tbl.printHelp(llvm::outs(), ${TODO});
    std::exit(0);
  }
  if (Args.hasArg(OPT_version)) {
    llvm::outs() << ToolName << '\n';
    cl::PrintVersionMessage();
    std::exit(0);
  }

  DriverConfig Config;
  if (const opt::Arg *A = Args.getLastArg(OPT_output_EQ))
    Config.Output = A->getValue();
  // TODO: Config.
  if (const opt::Arg *A = Args.getLastArg(OPT_))
    Config. = A->getValue();
  // TODO: Config.MaxDebugInfoCorrelationWarnings
  if (const opt::Arg *A = Args.getLastArg(OPT_profiled_binary_EQ))
    Config.ProfiledBinary = A->getValue();
  if (const opt::Arg *A = Args.getLastArg(OPT_debug_info_EQ))
    Config.DebugInfo = A->getValue();
  if (const opt::Arg *A = Args.getLastArg(OPT_binary_file_EQ))
    Config.BinaryFile = A->getValue();
  for (const opt::Arg *A : Args.filtered(OPT_))
    Config.DebugFileDirectory.push_back(A->getValue());
  Config.Debuginfod = Args.hasArg(OPT_debuginfod);
  // TODO: Config.Correlate
  if (const opt::Arg *A = Args.getLastArg(OPT_function_EQ))
    Config.Function = A->getValue();
  for (const opt::Arg *A : Args.filtered(OPT__EQ))
    Config..push_back(A->getValue());
  for (const opt::Arg *A : Args.filtered(OPT_weighted_input_EQ))
    Config.WeightedInput.push_back(A->getValue());
  // TODO: Config.
  if (const opt::Arg *A = Args.getLastArg(OPT_input_files_EQ))
    Config.InputFiles = A->getValue();
  Config.DumpInputFileList = Args.hasArg(OPT_dump_input_file_list);
  if (const opt::Arg *A = Args.getLastArg(OPT_remapping_file_EQ))
    Config.RemappingFile = A->getValue();
  Config.UseMd5 = Args.hasArg(OPT_use_md5);
  Config.CompressAllSections = Args.hasArg(OPT_compress_all_sections);
  Config.SampleMergeColdContext = Args.hasArg(OPT_sample_merge_cold_context);
  Config.SampleTrimColdContext = Args.hasArg(OPT_sample_trim_cold_context);
  // TODO: Config.SampleFrameDepthForColdContext
  // TODO: Config.OutputSizeLimit
  Config.GenPartialProfile = Args.hasArg(OPT_gen_partial_profile);
  Config.SplitLayout = Args.hasArg(OPT_split_layout);
  if (const opt::Arg *A = Args.getLastArg(OPT_supplement_instr_with_sample_EQ))
    Config.SupplementInstrWithSample = A->getValue();
  // TODO: Config.ZeroCounterThreshold
  // TODO: Config.SupplMinSizeThreshold
  // TODO: Config.InstrProfColdThreshold
  // TODO: Config.TemporalProfileTraceReservoirSize
  // TODO: Config.TemporalProfileMaxTraceLength
  if (const opt::Arg *A = Args.getLastArg(OPT_no_function_EQ))
    Config.NoFunction = A->getValue();
  // TODO: Config.FailureMode
  Config.Sparse = Args.hasArg(OPT_sparse);
  // TODO: Config.NumThreads
  if (const opt::Arg *A = Args.getLastArg(OPT_prof_sym_list_EQ))
    Config.ProfSymList = A->getValue();
  // TODO: Config.ConvertSampleProfileLayout
  Config.DropProfileSymbolList = Args.hasArg(OPT_drop_profile_symbol_list);
  Config.KeepVtableSymbols = Args.hasArg(OPT_keep_vtable_symbols);
  Config.WritePrevVersion = Args.hasArg(OPT_);
  // TODO: Config.MemprofVersion
  Config.MemprofFullSchema = Args.hasArg(OPT_memprof_full_schema);
  Config.MemprofRandomHotness = Args.hasArg(OPT_memprof_random_hotness);
  // TODO: Config.MemprofRandomHotnessSeed
  if (const opt::Arg *A = Args.getLastArg(OPT_))
    Config. = A->getValue();
  if (const opt::Arg *A = Args.getLastArg(OPT_))
    Config. = A->getValue();
  // TODO: Config.SimilarityCutoff
  Config.Cs = Args.hasArg(OPT_);
  // TODO: Config.ValueCutoff
  Config.Counts = Args.hasArg(OPT_);
  // TODO: Config.ShowFormat
  Config.Text = Args.hasArg(OPT_);
  Config.Json = Args.hasArg(OPT_);
  Config.IcTargets = Args.hasArg(OPT_);
  Config.ShowVtables = Args.hasArg(OPT_);
  Config.MemopSizes = Args.hasArg(OPT_);
  Config.DetailedSummary = Args.hasArg(OPT_);
  for (const opt::Arg *A : Args.filtered(OPT_))
    Config..push_back(A->getValue());
  Config.HotFuncList = Args.hasArg(OPT_);
  Config.AllFunctions = Args.hasArg(OPT_);
  Config.Showcs = Args.hasArg(OPT_);
  // TODO: Config.
  // TODO: Config.Topn
  // TODO: Config.ValueCutoff
  Config.ListBelowCutoff = Args.hasArg(OPT_);
  Config.ShowProfSymList = Args.hasArg(OPT_);
  Config.ShowSecInfoOnly = Args.hasArg(OPT_);
  Config.BinaryIds = Args.hasArg(OPT_);
  Config.TemporalProfileTraces = Args.hasArg(OPT_);
  Config.Covered = Args.hasArg(OPT_);
  Config.ProfileVersion = Args.hasArg(OPT_);
  // TODO: Config.NumTestTraces
  // TODO: Config.FsDiscriminatorPass
