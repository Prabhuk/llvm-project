include "llvm/Option/OptParser.td"

class F<string letter, string help> : Flag<["-"], letter>, HelpText<help>;
class FF<string name, string help> : Flag<["--"], name>, HelpText<help>;

multiclass Eq<string name, string metavar, string help> {
  def NAME #_EQ : Joined<["--"], name #"=">,
                  MetaVarName<metavar>,
                  HelpText<help>;
  def : Separate<["--"], name>, Alias<!cast<Joined>(NAME #_EQ)>;
}

def help : FF<"help", "Display available options">;
def : F<"h", "Alias for --help">, Alias<help>;
def version : FF<"version", "Display the version of this program">;

defm entry_point
    : Eq<"entry-point", "<name|glob|address>",
         "Analyze this function, instead of the ELF entry point and the "
         "exported functions. A name defined at several addresses selects all "
         "of them. Repeatable.">;
defm exclude
    : Eq<"exclude", "<name>",
         "Leave this function out of the analysis along with every call to "
         "it, as if it did not exist. Repeatable.">;
defm call_frame_overhead
    : Eq<"call-frame-overhead", "<bytes>",
         "Bytes a call pushes before the callee's frame; the default is the "
         "return address size on x86 and 0 elsewhere">;
defm red_zone
    : Eq<"red-zone", "<bytes>",
         "Bytes added to every leaf function for the red zone below its stack "
         "pointer; the default is the ABI's size, 128 on x86-64, 288 on PPC64 "
         "and 0 elsewhere">;
defm max_stack_usage
    : Eq<"max-stack-usage", "<bytes>",
         "Fail if an entry point without a --limit of its own may use more "
         "than <bytes> of stack, or if its estimate is only a lower bound">;
defm limit
    : Eq<"limit", "<name>=<bytes>",
         "Fail if the function may use more than <bytes> of stack, or if its "
         "estimate is only a lower bound. The function is analyzed even when "
         "it is not an entry point otherwise. Repeatable.">;
defm leaf
    : Eq<"leaf", "<name>",
         "Fail if the function makes any call. The function is analyzed even "
         "when it is not an entry point otherwise. Repeatable.">;
def json : FF<"json", "Write the report as JSON">;
def symbolizer_markup
    : FF<"symbolizer-markup",
         "Write function addresses in the text report as symbolizer markup, "
         "preceded by the module and mmap context of the file, so that "
         "llvm-symbolizer --filter-markup can add source locations">;
defm output : Eq<"output", "<file>", "Write the report to <file>">;
def : JoinedOrSeparate<["-"], "o">, Alias<output_EQ>,
      HelpText<"Alias for --output">, MetaVarName<"<file>">;
