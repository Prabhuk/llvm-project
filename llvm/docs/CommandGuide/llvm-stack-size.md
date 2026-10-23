# llvm-stack-size - LLVM Whole-Program ELF Stack Usage Estimator

```{program} llvm-stack-size
```

## SYNOPSIS

{program}`llvm-stack-size` \[**options**\] \<input-elf>

## DESCRIPTION

{program}`llvm-stack-size` reconstructs a whole-program PC-keyed call graph and
per-function stack frame sizes directly from a linked ELF binary's
`.llvm.callgraph` (`SHT_LLVM_CALL_GRAPH`, emitted by `-fcallgraph-section`) and
`.stack_sizes` (emitted by `-fstack-size-section`) sections. It condenses
strongly connected components (SCCs), computes worst-case stack usage and
maximal stack paths for entry points, and reports algebraic lower-bound
expressions when recursion cycles, missing `.stack_sizes` records,
uninstrumented/external callees, or unresolved indirect calls are reachable.

It can also enforce stack usage budgets (`--max-stack-usage`, `--limit`) and
leaf-function invariants (`--leaf`) as part of a build or CI check.

## OPTIONS

:::{option} --elf=<file>
Specify the input linked ELF executable or shared object to analyze (alternative
to passing `<input-elf>` positionally). Relocatable object files (`ET_REL`) are
rejected because direct callee addresses in `.llvm.callgraph` are resolved at
link time.
:::

:::{option} --entry-point=<name|glob|addr>
Specify an entry point function by symbol name, glob pattern (`*`, `?`, `[`),
or integer virtual address (`0x...`). May be specified multiple times. When
omitted, defaults to the ELF header entry point (`e_entry`, if non-zero) and
all defined `STT_FUNC` symbols in `.dynsym`.
:::

:::{option} --exclude=<name>
Exclude any function matching symbol `<name>` from the call graph and stack
usage estimation. May be specified multiple times.
:::

:::{option} --call-frame-overhead=<bytes>
Override the per-call stack frame overhead in bytes. Defaults to the target ABI
call instruction overhead (`8` bytes on `EM_X86_64`, `4` bytes on `EM_386`, `0`
bytes otherwise).
:::

:::{option} --max-stack-usage=<bytes>
Verify that every analyzed entry point without a per-function `--limit`
override uses at most `<bytes>` of stack and has a sound upper bound (not a
lower bound).
:::

:::{option} --limit=<name>=<bytes>
Verify that entry point `<name>` uses at most `<bytes>` of stack and has a
sound upper bound. May be specified multiple times.
:::

:::{option} --leaf=<name>
Verify that entry point `<name>` is a leaf function (makes no direct, indirect,
or external calls). May be specified multiple times.
:::

:::{option} --rspfile=<file>
Read additional command-line arguments from `<file>`. When combined with
`--depfile`, `<file>` is recorded as an input dependency.
:::

:::{option} --json
Emit the stack estimation report in structured JSON format instead of
human-readable text.
:::

:::{option} -o <file>, --output=<file>
Write the report to `<file>` instead of standard output.
:::

:::{option} --depfile=<file>
Write a Ninja/Make dependency file to `<file>` (requires `--output`).
:::

:::{option} -h, --help
Display available options.
:::

:::{option} --version
Display the version of this program.
:::
