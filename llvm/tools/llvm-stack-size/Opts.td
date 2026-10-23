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
defm call_frame_overhead
    : Eq<"call-frame-overhead", "<bytes>",
         "Bytes a call pushes before the callee's frame; the default is the "
         "return address size on x86 and 0 elsewhere">;
defm red_zone
    : Eq<"red-zone", "<bytes>",
         "Bytes added to every leaf function for the red zone below its stack "
         "pointer; the default is the ABI's size, 128 on x86-64, 288 on PPC64 "
         "and 0 elsewhere">;
def json : FF<"json", "Write the report as JSON">;
defm output : Eq<"output", "<file>", "Write the report to <file>">;
def : JoinedOrSeparate<["-"], "o">, Alias<output_EQ>,
      HelpText<"Alias for --output">, MetaVarName<"<file>">;
