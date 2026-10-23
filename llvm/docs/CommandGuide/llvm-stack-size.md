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
  Lower bound because:
    unknown callees: asm_helper (0x2200), called by main (0x1000)
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

## LIMITATIONS

The analysis trusts the call graph in the file. A function that makes calls
the compiler did not record, such as an indirect call without type
information, or that is called from code without call graph information,
such as assembly, is not seen. A call to a function outside the file counts as
a call of unknown depth. Tail calls count as nested calls. The estimate counts
one invocation of each function on a cycle, so recursion always yields a lower
bound.

## EXIT STATUS

{program}`llvm-stack-size` exits with 0 when the report was produced, whether
or not the estimates are exact, and with 1 when the file could not be analyzed
or an option was invalid.
