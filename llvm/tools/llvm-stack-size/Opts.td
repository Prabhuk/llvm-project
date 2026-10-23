include "llvm/Option/OptParser.td"

class F<string letter, string help> : Flag<["-"], letter>, HelpText<help>;
class FF<string name, string help> : Flag<["--"], name>, HelpText<help>;

multiclass Eq<string name, string metavar, string help> {
  def NAME #_EQ : Joined<["--"], name #"=">,
                  MetaVarName<metavar>,
                  HelpText<help>;
  def : Separate<["--"], name>,
        MetaVarName<metavar>,
        Alias<!cast<Joined>(NAME #_EQ)>;
}

def help : FF<"help", "Display available options">;
def : F<"h", "Alias for --help">, Alias<help>;
def version : FF<"version", "Display the version of this program">;

defm elf : Eq<"elf", "<file>", "Input linked ELF binary to analyze">;
defm entry_point
    : Eq<"entry-point", "<name|glob|addr>",
         "Entry point function name, glob pattern, or address (may be specified multiple times; defaults to e_entry and .dynsym functions)">;
defm exclude
    : Eq<"exclude", "<name>",
         "Function symbol name to exclude from call graph and stack estimation (may be specified multiple times)">;
defm call_frame_overhead
    : Eq<"call-frame-overhead", "<bytes>",
         "Override per-call frame overhead in bytes (defaults to 8 on x86_64, 4 on i386, 0 otherwise)">;
def json : FF<"json", "Emit stack usage report as JSON">;
defm output : Eq<"output", "<file>", "Write output report to <file> instead of stdout">;
def : JoinedOrSeparate<["-"], "o">, MetaVarName<"<file>">, Alias<output_EQ>, HelpText<"Alias for --output">;
defm depfile : Eq<"depfile", "<file>", "Write Ninja/Make dependency file to <file> (requires --output)">;
