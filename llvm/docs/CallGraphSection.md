# .callgraph Section Layout

The `.callgraph` section is used to store call graph information for each function, which can be used for post-link analyses and optimizations. The section contains a series of records, with each record corresponding to a single function.

For efficiency, we make a distinction between direct and indirect call data. For direct calls, we record the unique callees, not the location of each individual call. For indirect calls, we record the location of each call site and the type ID of the callee. Post link analysis scripts which utilize this information to reconstuct the program call graph can potentially receive more information regarding  indirect callsites from the user to improve the precision of the call graph.

## Per Function Record Layout

The fields in the record are ordered to simplify parsing. The first four fields always have a fixed size, regardless of the target architecture or the function's specific call graph. The subsequent fields are variable-sized or optional, depending on the target architecture and function's properties.

Each record in the `.callgraph` section has the following binary layout:

| Field                        | Type          | Size (bits) | Description                                                                                             |
| ---------------------------- | ------------- | ----------- | ------------------------------------------------------------------------------------------------------- |
| Format Version               | `uint32_t`    | 32          | The version of the record format. The current version is 0.                                             |
| Function Kind                | `uint8_t`     | 8           | An enum indicating the function's properties (e.g., if it's an indirect call target).                   |
| Number of Indirect Callsites | `uint32_t`    | 32          | The number of indirect call sites within the function.                                                  |
| Number of Direct Callees     | `uint32_t`    | 32          | The number of unique direct call destinations from this function.                                       |
| Function Entry PC            | `uintptr_t`   | 32/64       | The address of the function's entry point.                                                              |
| Function Type ID             | `uint64_t`    | 64          | The type ID of the function. This field is **only** present if `Function Kind` is `INDIRECT_TARGET_KNOWN_TID`. |
| Indirect Callsites Array     | `Callsite[]`  | Variable    | An array of `Callsite` records, with a length of `Number of Indirect Callsites`.                        |
| Direct Callees Array         | `uintptr_t[]` | Variable    | An array of unique direct callee entry point addresses, with a length of `Number of Direct Callees`.    |

### Indirect Callsite Record Layout

Each record in the `Indirect Callsites Array` has the following layout:

| Field             | Type        | Size (bits) | Description                               |
| ----------------- | ----------- | ----------- | ----------------------------------------- |
| Type ID           | `uint64_t`  | 64          | The type ID of the indirect call target.  |
| Callsite PC       | `uintptr_t` | 32/64       | The address of the indirect call site.    |
