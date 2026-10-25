// Global initialization and cleanup functions are called through function
// pointers from .init_array, .fini_array and atexit registrations, so they get
// call graph type IDs like any other address-taken function.

// RUN: %clang_cc1 -triple aarch64-unknown-fuchsia -emit-llvm -o - \
// RUN:   -fexperimental-call-graph-section %s | FileCheck %s
// RUN: %clang_cc1 -triple aarch64-unknown-fuchsia -emit-llvm -o - \
// RUN:   -fexperimental-call-graph-section \
// RUN:   -fsanitize-cfi-icall-experimental-normalize-integers %s | \
// RUN:   FileCheck %s --check-prefix=NORM

struct S {
  S();
  ~S();
};

S Global;

// CHECK-DAG: define internal void @__cxx_global_var_init() {{.*}}!callgraph [[VOID:![0-9]+]]
// CHECK-DAG: define internal void @_GLOBAL__sub_I_{{.*}}() {{.*}}!callgraph [[VOID]]
// CHECK-DAG: [[VOID]] = !{!"_ZTSFvvE"}

// NORM-DAG: define internal void @_GLOBAL__sub_I_{{.*}}() {{.*}}!callgraph [[VOIDN:![0-9]+]]
// NORM-DAG: [[VOIDN]] = !{!"_ZTSFvvE.normalized"}
