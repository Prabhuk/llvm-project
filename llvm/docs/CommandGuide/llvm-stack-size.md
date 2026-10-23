# llvm-stack-size - estimate the stack usage of a linked ELF file

```{program} llvm-stack-size
```

## SYNOPSIS

{program}`llvm-stack-size` \[*options*\] *file*

## DESCRIPTION

{program}`llvm-stack-size` estimates the worst-case stack usage of the entry
points of a linked ELF executable or shared object. It reconstructs the call
graph from the `.llvm.callgraph` section that clang emits under
`-fexperimental-call-graph-section` and reads each function's frame size from
the `.stack_sizes` section emitted under `-fstack-size-section`, both of which
survive linking. For every entry point it reports the largest stack usage of
any call path, the path that reaches it, and whether that number is exact.

The estimate is exact when every function on every path from the entry point
has a known frame size and known callees. Otherwise it is a lower bound of the
true worst case, and the report says why. The causes are recursion, a function
without a frame size (for example one with a dynamically sized frame), a
callee without call graph information (assembly, code from another compiler,
or a function in another module, which a linked file identifies only as
address 0), an indirect call whose type matches no function, and an indirect
call while the file has indirect call targets of unknown type, which no call
site can rule out.

Functions are identified by address. Symbol names only select entry points and
label the report, so several functions may share a name and a function may
have several names.

## OPTIONS

:::{option} --entry-point=<name|glob|address>
Analyze this function. A symbol name selects every function defined under it,
a glob pattern (with `*`, `?` and `[...]`) selects every function whose name
matches, and an address (such as `0x1000`) selects the function at it. The
option is repeatable. Without it, the entry points are the function at the ELF
entry point and the functions exported through the dynamic symbol table.
:::

:::{option} --exclude=<name>
Leave the functions named *name* out of the analysis, along with every call
to them, as if they did not exist. This asserts that they are never reached or
use no stack; the tool does not check it. An indirect call whose only targets
were excluded has no known targets left and is reported as unresolved. The
option is repeatable.
:::

:::{option} --call-frame-overhead=<bytes>
The bytes a call pushes on the stack before the callee's frame, counted once
for every function on a path. The default is the size of the return address on
x86 (8 bytes on x86-64, 4 on i386) and 0 on architectures that pass it in a
register.
:::

:::{option} --red-zone=<bytes>
The bytes below the stack pointer that a leaf function may use without moving
it, which `.stack_sizes` does not include. They are added to every function
that makes no calls. The default is the size the ABI grants, 128 bytes on
x86-64 and 288 on PPC64, and 0 elsewhere. Pass 0 for code built with
`-mno-red-zone`.
:::

:::{option} --max-stack-usage=<bytes>
Fail, with exit status 1, if any analyzed function without a `--limit` of its
own may use more than *bytes* of stack. A limit is a guarantee, so an estimate
that is only a lower bound fails it whatever its value.
:::

:::{option} --limit=<name>=<bytes>
Fail if the functions named *name* may use more than *bytes* of stack, under
the same rule as `--max-stack-usage`, which this overrides for them. The
functions are analyzed even when they are not entry points otherwise. When
several limits name one function, through aliases or repeated options, the
smallest applies. The option is repeatable.
:::

:::{option} --leaf=<name>
Fail if the functions named *name* make any call, directly or indirectly, or
have no call graph information. The functions are analyzed even when they are
not entry points otherwise. The option is repeatable.
:::

:::{option} --json
Write the report as JSON instead of text. See JSON OUTPUT below.
:::

:::{option} --symbolizer-markup
Write every function address in the text report as a `{{{pc:...}}}` symbolizer
markup element, preceded by the `{{{module:...}}}` and `{{{mmap:...}}}` context
of the file, so that `llvm-symbolizer --filter-markup` turns the report into
one with source locations. The file needs a build ID.
:::

:::{option} -o <file>, --output=<file>
Write the report to *file* instead of standard output.
:::

:::{option} -h, --help
Print a summary of command line options.
:::

:::{option} --version
Display the version of the program.
:::

:::{option} @<file>
Read command-line options from response file *file*.
:::

## OUTPUT

The report lists the call frame overhead and red zone in effect when either is
nonzero, the indirect call targets of unknown type when there are any, and then
every entry point:

```
Entry point: main (0x1000)
  Stack usage: at least 4112 bytes (lower bound)
  Stack usage expression: max(4112 + calls(asm_helper), 48 + calls(<external>))
  Lower bound because:
    unknown callees: asm_helper (0x2200), called by main (0x1000)
    unknown callees: <external> (0x0), called by helper (0x1100)
  Max stack path:
    main (0x1000): 16 bytes
    asm_helper (0x2200): 4096 bytes, callees unknown
```

`Stack usage` is the largest usage of any path from the entry point, in bytes.
`Max stack path` lists the functions on that path in call order with the frame
size of each, followed by the red zone and call frame overhead counted for it.
Every member of a recursive component on the path is listed, since every
member's frame counts once. `Lower bound because` lists what the analysis
could not see.

A large binary can have thousands of functions behind one of these lines, so
every list in the text report shows at most ten items and names at most three
functions inline, then says how many more there are, as in
`and 2 more functions of this cycle with 64 more bytes` after the first ten
members of a recursive component on the path, or `called by a (0x1000),
b (0x1100), c (0x1200) and 5 more`. The counts are always complete, only the
names are left out.

For a lower bound, `Stack usage expression` gives the usage with the unknowns
kept as terms. Every path from the entry point contributes the sum of its known
frames plus a term for each unknown on it: `frame(f)` for a function without a
frame size, `calls(f)` for the callees of a function without call graph
information (and its own frame, when that is unknown too), `cycle(f, g)` for
the further invocations of a recursive component, `indirect(type 0x1234)` for
the unknown targets of an indirect call whose type ID matches no function, and
`indirect(unknown type)` for an indirect call that may reach one of the file's
indirect call targets of unknown type. An unknown is named by what is unknown
rather than by the function that reaches it, so that paths which differ only in
that function share a term. A path that cannot use more stack than another for
any value of the unknowns is dropped, and the remaining paths form a
`max(...)`. An expression that would name more than 64 unknowns is given as
its number of terms and unknowns instead.

The incomparable paths of a large binary can run to tens of thousands per
function, so a function keeps the 32 terms with the largest constants and
merges the rest into one term with the largest of their constants and all of
their unknowns, which bounds every path it stands for. The expression is then
marked `(coarsened)`: it is an upper bound of the exact expression rather than
equal to it, and its largest constant is still the stack usage. The term limit
only ever affects the expression, never the stack usage or the reasons.

## JSON OUTPUT

With `--json` the report is one object, written one entry function at a time.
Addresses and type IDs are integers. The recursive components and the unknowns
of the expressions are listed once, for the whole file, and everything else
refers to them by index, so that the report does not repeat a large component
for every entry function that reaches it.

```json
{
  "file": "libfoo.so",
  "call_frame_overhead": 8,
  "red_zone_size": 128,
  "unknown_type_indirect_targets": [ 12288 ],
  "cycles": [
    [ { "address": 8192, "name": "rec", "stack": 64 } ]
  ],
  "unknowns": [
    { "name": "calls(<external>)", "kind": "calls", "address": 0 },
    { "name": "cycle(rec)", "kind": "cycle", "cycle": 0 }
  ],
  "entry_functions": [
    {
      "entry_function": "main",
      "address": 4096,
      "max_stack_usage": 96,
      "is_lower_bound": true,
      "stack_usage_expression": {
        "terms": [
          { "constant": 96, "unknowns": [ 1 ] },
          { "constant": 32, "unknowns": [ 0 ] }
        ],
        "coarsened": false
      },
      "max_stack_path": [
        { "address": 4096, "name": "main", "stack": 16 },
        { "cycle": 0 }
      ],
      "cycles": [ 0 ],
      "unknown_stack_sizes": [],
      "uninstrumented_callees": [
        { "address": 0, "name": "<external>", "callers": [ 4096 ] }
      ],
      "unresolved_indirect_calls": [],
      "indirect_calls_with_unknown_type_targets": []
    }
  ]
}
```

`unknown_type_indirect_targets` lists the functions of the file that are
indirect call targets of unknown type. `cycles` lists every recursive
component of the file by its members, each with its frame size as `stack`,
or `null` when it is unknown. `unknowns` lists the unknowns of the
expressions, each with its name as in the text report and its `kind`:
`frame` and `calls` name the function by `address`, `cycle` the component by
its index in `cycles`, and `indirect` the type of the call by `type_id`, with
0 standing for the indirect call targets of unknown type.

Per entry function, `stack_usage_expression` is the maximum of its `terms`,
each a `constant` plus the unknowns given by their indexes in `unknowns`, and
`coarsened` says whether terms were merged, as described under OUTPUT.
`max_stack_path` lists the functions on the path as `address`, `name` and
`stack`, or a recursive component as its index in `cycles`, without the red
zone and call frame overhead that `max_stack_usage` includes. `cycles` gives
the reachable recursive components as indexes, `unknown_stack_sizes` the
reachable functions without a frame size, `uninstrumented_callees` the
reachable functions without call graph information with their callers,
`unresolved_indirect_calls` the reachable indirect calls whose type matches no
function, each as the caller and the type ID, and
`indirect_calls_with_unknown_type_targets` the reachable functions whose
indirect calls might reach a target of unknown type. Unlike the text report,
the JSON report has no size limits.

## VERIFICATION

With `--max-stack-usage`, `--limit` or `--leaf`, the report is followed by an
error for every function that breaks the policy, and the exit status is 1. An
error for a function over its limit shows the path that reaches the limit, and
one for a lower bound shows what the analysis could not see, so that the
output of a build step says what to fix. With `--symbolizer-markup`, the
errors carry the same markup as the report.

```
error: main (0x1000) uses 4296 bytes of stack, more than the limit of 4096 bytes:
  main (0x1000): 16 bytes + 8 bytes call overhead
  big (0x2000): 4096 bytes + 8 bytes call overhead
  helper (0x3000): 32 bytes + 128 bytes red zone + 8 bytes call overhead
```

## LIMITATIONS

The analysis trusts the call graph in the file. A function that makes calls
the compiler did not record, such as an indirect call without type
information, or that is called from code without call graph information,
such as assembly, is not seen. A call to a function outside the file counts as
a call of unknown depth. Tail calls count as nested calls. The estimate counts
one invocation of each function on a cycle, so recursion always yields a lower
bound. Excluding a function with `--exclude` makes the result depend on the
user's word that it is never reached.

## EXIT STATUS

{program}`llvm-stack-size` exits with 0 when the report was produced and no
policy was broken, and with 1 when a function broke a `--max-stack-usage`,
`--limit` or `--leaf` policy, when the file could not be analyzed, or when an
option was invalid. Without a policy the estimates need not be exact for the
exit status to be 0.
